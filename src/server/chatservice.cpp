#include "chatservice.hpp"
#include "public.hpp"
#include <muduo/base/Logging.h>
#include <vector>
#include <iostream>
using namespace muduo;
using namespace std;
//获取单例对象的接口函数
ChatService* ChatService::instance()
{
    static ChatService service;
    return &service;
}

void ChatService::login(const TcpConnectionPtr &conn,json &js,Timestamp time)
{
    int id = js["id"].get<int>();
    string pwd = js["password"];

    User user = _userModel.query(id);
    if(user.getId() != -1 && user.getPwd() == pwd){
        if(user.getState() == "online"){
            //该用户已经登录，不允许重复登录
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 1;
            response["errmsg"] = "this account is using, input another!";
            conn -> send(response.dump());
        }else {
            //登录成功，记录用户连接信息
            {
                lock_guard<mutex> lock(_connMutex);
                _userConnMap.insert({id,conn});
            }
            //id用户登录成功后，向redis订阅channel
            _redis.subscribe(id);

            //登录成功，更新用户状态信息
            user.setState("online");
            _userModel.updateState(user);
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 0;
            response["id"] = user.getId();
            response["name"] = user.getName();
            
            //查询用户是否有离线消息
            vector<string> vec = _offlineMsgModel.query(id);
            if(!vec.empty()){
                response["offlinemsg"] = vec;
                _offlineMsgModel.remove(id);
            }

            vector<User> userVec = _friendModel.query(id);
            if(!userVec.empty()){
                vector<string> vec2;
                for(User &user : userVec){
                    json js;
                    js["id"] = user.getId();
                    js["name"] = user.getName();
                    js["state"] = user.getState();
                    vec2.push_back(js.dump());
                }
                // _offlineMsgModel.remove(id);
                response["friends"] = vec2;
            }

            //查询用户的群组信息
            vector<Group> groupuserVec = _groupModel.queryGroups(id);
            if(!groupuserVec.empty()){
                vector<string> groupV;
                for(Group &group:groupuserVec){
                    json grpjson;
                    grpjson["id"] = group.getId();
                    grpjson["groupname"] = group.getName();
                    grpjson["groupdesc"] = group.getDesc();
                    vector<string> userV;
                    for(GroupUser &user : group.getUsers()){
                        json js;
                        js["id"] = user.getId();
                        js["name"] = user.getName();
                        js["state"] = user.getState();
                        js["role"] = user.getRole();
                        userV.push_back(js.dump());
                    }
                    grpjson["users"] = userV;
                    groupV.push_back(grpjson.dump());
                }

                response["groups"] = groupV;
            }
            conn -> send(response.dump());
        }
    }else{
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "id or password is invaild!";
        conn -> send(response.dump());
    }
}
    
void ChatService::reg(const TcpConnectionPtr &conn,json &js,Timestamp time)
{
    string name = js["name"];
    string pwd = js["password"];
    User user;
    user.setName(name);
    user.setPwd(pwd);
    bool state = _userModel.insert(user);
    if(state){
        //注册成功
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 0;
        response["id"] = user.getId();
        conn -> send(response.dump());
    }else {
        //注册失败
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        conn -> send(response.dump());
    }
}

//注册消息及对应的回调操作
ChatService::ChatService()
{
    _msgHandlerMap.insert({LOGIN_MSG,std::bind(&ChatService::login,this,_1,_2,_3)});
    _msgHandlerMap.insert({REG_MSG,std::bind(&ChatService::reg,this,_1,_2,_3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG,std::bind(&ChatService::oneChat,this,_1,_2,_3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG,std::bind(&ChatService::addFriend,this,_1,_2,_3)});
    _msgHandlerMap.insert({CREATE_GROUP_MSG,std::bind(&ChatService::createGroup,this,_1,_2,_3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG,std::bind(&ChatService::addGroup,this,_1,_2,_3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG,std::bind(&ChatService::groupChat,this,_1,_2,_3)});
    _msgHandlerMap.insert({LOGINOUT_MSG,std::bind(&ChatService::loginout,this,_1,_2,_3)});

    if(_redis.connect()){
        //设置上报消息的回调
        _redis.init_notify_handler(std::bind(&ChatService::handleRedisSubscribeMessage,this,_1,_2));
    }
}

MsgHandler ChatService::getHandler(int msgid)
{
    //记录错误日志，msgid没有对应的事件处理回调
    auto it = _msgHandlerMap.find(msgid);
    if(it == _msgHandlerMap.end())
    {
        // 返回一个空的处理器
        return [=](const TcpConnectionPtr &conn,json &js,Timestamp time){
            LOG_ERROR << "msgid:" << msgid << " can not find handler!";
        };
    }
    else 
    {
        return _msgHandlerMap[msgid];
    }
}

//{"msgid":1,"id":23,"password":"123456"}

void ChatService::clientCloseException(const TcpConnectionPtr &conn){
    User user;
    {
        lock_guard<mutex> lock(_connMutex);
        for(auto it = _userConnMap.begin();it != _userConnMap.end(); ++it){
            if(it -> second == conn){
                //从map表删除用户的连接信息
                user.setId(it -> first);
                _userConnMap.erase(it);
                break;
            }
        }
    }

    //用户注销，在redis中取消订阅通道
    _redis.unsubscribe(user.getId());

    //更新用户的状态信息
    if(user.getId() != -1){
        user.setState("offline");
        _userModel.updateState(user);
    }
}

//一对一聊天业务
void ChatService::oneChat(const TcpConnectionPtr &conn,json &js,Timestamp time){
    int toid = js["to"].get<int>();
    bool userState = false;
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toid);
        if(it != _userConnMap.end()){
            //toid在线，转发消息 服务器主动推送消息给toid用户
            it -> second -> send(js.dump());
            return;
        }
    }
    //查询toid是否在线
    User user = _userModel.query(toid);
    if(user.getState() == "online"){
        // cout << js.dump() << endl;
        _redis.publish(toid,js.dump());
        return;
    }

    //toid不在线，存储离线消息
    _offlineMsgModel.insert(toid,js.dump());
}

void ChatService::reset(){
    //把online状态用户，设置成offline
    _userModel.resetState();
}

void ChatService::addFriend(const TcpConnectionPtr &conn,json &js,Timestamp time){
    int userid = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();

    _friendModel.insert(userid,friendid);
}

//创建群组业务
void ChatService::createGroup(const TcpConnectionPtr &conn,json &js,Timestamp time){
    int userid = js["id"].get<int>();
    string name = js["groupname"];
    string desc = js["groupdesc"];

    //存储新创建的群组信息
    Group group(-1,name,desc);
    if(_groupModel.createGroup(group)){
        //存储群组创建人信息
        _groupModel.addGroup(userid,group.getId(),"creator");
    }
}
//加入群组业务
void ChatService::addGroup(const TcpConnectionPtr &conn,json &js,Timestamp time){
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    _groupModel.addGroup(userid,groupid,"normal");
}
//群组聊天业务
void ChatService::groupChat(const TcpConnectionPtr &conn,json &js,Timestamp time){
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    vector<int> useridVec = _groupModel.queryGroupUsers(userid,groupid);

    lock_guard<mutex> lock(_connMutex);
    for(int id : useridVec){
        auto it = _userConnMap.find(id);
        if(it != _userConnMap.end()){
            it -> second -> send(js.dump());
        }else {
            User user = _userModel.query(id);
            if(user.getState() == "online"){
                _redis.publish(id,js.dump());
                return;
            }else
                _offlineMsgModel.insert(id,js.dump());
        }
    }
}

void ChatService::loginout(const TcpConnectionPtr &conn,json &js,Timestamp time){
    int userid = js["id"].get<int>();
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if(it != _userConnMap.end()){
            _userConnMap.erase(it);
        }
    }

    //用户注销，在redis中取消订阅通道
    _redis.unsubscribe(userid);

    //更新用户的状态信息
    User user(userid,"","","offline");
    _userModel.updateState(user);
}

void ChatService::handleRedisSubscribeMessage(int userid,string msg){
    json js = json::parse(msg.c_str());

    lock_guard<mutex> lock(_connMutex);
    auto it = _userConnMap.find(userid);
    if(it != _userConnMap.end()){
        it -> second -> send(msg);
        return;
    }

    _offlineMsgModel.insert(userid,msg);
}

//{"msgid":1,"id":15,"password":"666666"}
//{"msgid":1,"id":16,"password":"123456"}
//{"msgid":5,"id":15,"from":"li si","to":16,"msg":"hello!"}
//{"msgid":6,"id":15,"friendid":16}
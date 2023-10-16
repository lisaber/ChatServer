#ifndef USERMODEL_H
#define USERMODEL_H

#include "user.hpp"

//User表的数据操作类
class UserModel{
public:
    //User表的增加方法
    bool insert(User &user);
    User query(int id);//根据用户号码查询用户信息
    //更新用户状态信息
    bool updateState(User user);
    //重置用户状态信息
    void resetState();
};

#endif
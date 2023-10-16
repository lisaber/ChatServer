#include "chatserver.hpp"
#include "chatservice.hpp"
#include <iostream>
#include <signal.h>
using namespace std;
//服务器ctrl+c退出后，重置user的状态信息
void resetHandler(int){
    ChatService::instance() -> reset();
    exit(0);
}

int main(int argc,char **argv)
{
    signal(SIGINT,resetHandler);
    if(argc < 3){
        cerr << "command invaild! example: ./ChatClient 127.0.0.1 6000" << endl;
        exit(-1);
    }
    char *ip = argv[1];
    uint16_t port = atoi(argv[2]);
    EventLoop loop;
    InetAddress addr(ip, port);
    ChatServer server(&loop, addr, "ChatServer");

    server.start();
    loop.loop();

    return 0;
}
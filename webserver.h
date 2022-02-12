#ifndef WEBSERVER_H
#define WEBSERVER_H

//c库文件
#include <cstdlib> //包含一些工具atoi，malloc等
#include <cassert> //assert库，检错则报异常
#include <cstring> //bzero，strcat等字符串操作函数

//c++标准库
#include<iostream>

//其他库
#include <sys/epoll.h>  //linux的epoll库
#include <unistd.h>     //包含大量unix系统调用接口
#include <sys/socket.h> //socket主要库
#include <netinet/in.h> //定义数据结构sockaddr_in
#include <errno.h>      //包含了errno宏
#include <arpa/inet.h>  //用于网络地址转换等操作


//本项目内.h文件
#include "./epoll/epoll_function.h"
#include "./http/http_conn.h"
#include "./threadpool/threadpool.h"


const int kEVENT_MAX = 10000; //最大监听事件数
const int kFD_MAX = 65536;           //最大文件描述符
const int kTIMESLOT = 5;             //最小超时单位


//服务器类
class WebServer
{

public:
    WebServer();
    ~WebServer();

    //Web参数配置
    void ParameterSet(int argc, char *argv[]);


    //监听socket初始化,并开始监听
    void SocketInit();

    // 定时器初始化，并启动定时器
    void ListTimerInit()
    {
    };

    //主线程（将监听socketfd加入epoll监听事件表，并开始循环处理事件）
    void EventLoop();

    //处理用户新连接
    bool HandleNewConn();
    //读事件，用户请求事件到了
    void HandleReadEvent(int sockfd);
    //写事件，用户请求处理完了，并写入了缓存区，等待写入用户socket
    void HandleWriteEvent(int sockfd);


public:
    //主机监听socket基本配置参数
    int web_port = 9006;                            //主机端口号 default=9006
    int web_listen_fd;                              //主机socket监听fd
    TriggerMode web_listen_trigger_mode = LT_MODE; //web主机socket监听fd模式
    bool so_linger_opt = 0;                          //优雅关闭socket选项 不启用为0 启用为1 default=0

    //已连接用户socket配置参数
    TriggerMode client_sockfd_trigger_mode = LT_MODE; //已连接客户端socket监听fd模式

    //epoll监听
    int web_epoll_fd; //指向内核监听事件表的fd
    epoll_event events[kEVENT_MAX]; //用于获取和写入事件表结构体

    //client的http_conn类对象首指针
    http_conn* client_http_conn;

    //线程池模式
    concurrency_model web_concurrency_model=PROACTOR_MODEL;  //默认proactor模式，让主线程处理读写


};

#endif

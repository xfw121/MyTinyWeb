#ifndef HTTP_CONN_H
#define HTTP_CONN_H

//c库文件
#include <string.h>
#include <stdio.h>
#include <stdarg.h> //va_start和va_end使用

//c++标准库
#include <string>
using std::string;

//其他库
#include <sys/epoll.h>  //linux的epoll库
#include <unistd.h>     //包含大量unix系统调用接口
#include <sys/socket.h> //socket主要库
#include <netinet/in.h> //定义数据结构sockaddr_in
#include <errno.h>      //包含了errno宏

//本项目内.h文件
#include "../epoll/epoll_function.h"

class http_conn
{

public:
    //静态成员
    static const int kREAD_BUFFER_SIZE = 2048;  //读缓存大小
    static const int kWRITE_BUFFER_SIZE = 1024; //写缓存大小
    static int epoll_fd;                        //事件表描述符，由主线获得
    static int client_count;                    //当前用户数量

    //Http状态
    enum HttpCode
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //对外封装的基本操作（初始化，读取http请求，处理http请求，写http请求，）
    //初始化函数
    void Init(int client_sockfd, const sockaddr_in &client_address, TriggerMode trigger_mode);
    void HttpProcess();             //http处理用户请求函数
    bool ReadOnce();                //读取内核接收缓存到http缓存处
    bool Write();                   //写入http缓存到内核发送缓存
    void CloseConn(bool close_opt); //关闭该已连接用户

private:
    //用户网络信息
    int client_sockfd;          //用户已连接sockfd
    sockaddr_in client_address; //网络地址
    TriggerMode trigger_mode;   //触发模式
    bool linger_opt;             //长连接选项

    //读缓存
    char read_buffer[kREAD_BUFFER_SIZE]; //读数组
    int read_index;                      //读索引，记录已读数据数

    //写缓存
    char write_buffer[kWRITE_BUFFER_SIZE]; //写数组
    int write_index;                       //写索引，记录已写数据（不含文件）

    
    struct iovec m_iv[2]; //用于写非连续内存结构体
    int m_iv_count;       //非连续内存个数

    char *file_address; //文件缓存地址

    int all_bytes_have_send; //所有已写数据（包含文件）
    int all_bytes_to_send;   //所有待写数据（包含文件）

public:
    //用于reactor模式下，提示主线程，已经完成http缓存的读取/写入
    int improv;

public:
    //http读写状态
    enum
    {
        write_state,
        read_state,
    } http_state;

private:
    //Http初始化
    void HttpInit();

    //处理GET请求
    HttpCode DoRequest();
    //读取http内容，处理请求
    HttpCode HttpRead();
    //将http内容写入缓存，确定要写的内容大小
    bool HttpWrite(HttpCode ret);


    //http内容处理函数
    bool AddResponse(const char *format, ...);

    //http行内容编写函数
    //状态行编写
    bool AddStatusLine(int status, const char *title);//状态行
    
    bool AddHeaders(int content_len);        //添加头

    bool AddContent(const char *content);    //添加内容

    //添加头的子函数
    bool AddContentLength(int content_len);  //添加内容长度

    bool AddLinger();    //添加长连接linger选项

    bool AddBlankLine(); //添加空白行

};

#endif
#ifndef HTTP_CONN_H
#define HTTP_CONN_H

// c库文件
#include <string.h>
#include <stdio.h>
#include <stdarg.h> //va_start和va_end使用
#include <sys/stat.h> //用于stat文件状态结构体使用
#include <sys/mman.h> //用于mmap和ummap函数使用

// c++标准库
#include <string>
#include <map>

using std::string;

//其他库
#include <sys/epoll.h>   //linux的epoll库
#include <unistd.h>      //包含大量unix系统调用接口
#include <sys/socket.h>  //socket主要库
#include <netinet/in.h>  //定义数据结构sockaddr_in
#include <errno.h>       //包含了errno宏
#include <mysql/mysql.h> //数据库操作api

//本项目内.h文件
#include "../util/util.h"
#include "../cgi_mysql/sql_connection_pool.h"

class HttpConn
{

public:
    // http相关类型
    enum HttpStatus // Http响应状态
    {
        NO_REQUEST,        //还没有解析完整个报文，需要继续读取请求报文数据/监听连接获取更多报文数据
        GET_REQUEST,       //解析了完整的HTTP请求
        BAD_REQUEST,       // HTTP请求报文有语法错误或请求资源为目录
        NO_RESOURCE,       //请求资源不存在
        FORBIDDEN_REQUEST, //请求资源禁止访问，没有读取权限
        FILE_REQUEST,      //请求资源可以正常访问
        INTERNAL_ERROR,    //服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        CLOSED_CONNECTION  
    };
    //构造函数和析构函数
    HttpConn() {}
    ~HttpConn() {}
    //对外封装的基本操作（初始化，处理http请求，关闭连接等操作）
    void Init(int client_sockfd_t, const sockaddr_in &client_address_t); //初始化函数
    void Process();                                                  // http处理用户请求函数
    bool ReadOnce();                                                     //读取内核接收缓存到http缓存处
    bool Write();                                                        //写入http缓存到内核发送缓存
    void CloseConn(bool close_opt);                                      //关闭该已连接用户
    sockaddr_in *GetAddress()                                            //获取地址，日志要用
    {
        return &address_;
    }
    void MysqlResultInit(ConnectionPool *connPool); //先把已存储用户和密码预读取出来到uses_cache_中

    //对外的接口变量
    static int epoll_fd_;                    //事件表描述符，由主线程获得
    static int client_counts_;               //当前用户数量，主线程判断文件描述符是否足够
    static TriggerMode socket_trigger_mode_; //触发模式，主线程设置
    static char *root_path_;       //根目录路径 主线程设置
    static int log_switch_;        //日志开关 主线程设置
    //主线程和工作线程之间的沟通变量（主要是事件处理）
    int completed_flag_;                     //用于reactor模式下，提示主线程，已经完成http缓存的读取/写入
    int close_conn_flag_;                      //工作线程处理连接超时标志 告诉主线程处理连接
    int http_state_;                         // http读写状态 0读 1写 告诉工作线程

    //数据库相关
    MYSQL *mysql_; //数据库连接

private:
    // http相关类型
    enum HttpMethod // http请求方法
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CheckState //当前http处理阶段
    {
        CHECK_STATE_REQUESTLINE = 0, //请求行阶段
        CHECK_STATE_HEADER,          //消息头阶段
        CHECK_STATE_CONTENT          //消息体阶段
    };
    enum LineStatus //行处理状态
    {
        LINE_OK = 0, //完整读取一行
        LINE_BAD,    //报文语法有误
        LINE_OPEN    //读取的行不完整
    };

    //静态成员 只有static const声明的整型成员能在类内部初始化，并且初始化值必须是常量表达式
    static const int kREAD_BUFFER_SIZE = 2048;  //读缓存大小
    static const int kWRITE_BUFFER_SIZE = 1024; //写缓存大小
    static const int kFILENAME_LEN = 200;       //文件名长度

    //处理http过程函数
    void HttpClear();                  //清空http读写缓存及状态
    HttpStatus ProcessRead();          //读取http缓存内容，处理请求，确定响应状态和返回文件的映射
    bool ProcessWrite(HttpStatus ret); //依据回应状态，将响应报文写入http写缓存，并将http写缓存和文件映射填入非连续内存写结构体m_iv_

    //处理http请求过程状态机子函数
    LineStatus ParseLine(); //从状态机 读取一行并把返回取行状态         
    char *GetLine() //获取当前准备处理行的首地址
    {
        return read_buffers_ + start_line_;
    };     
    HttpStatus ParseRequestLine(char *text);//解析请求行
    HttpStatus ParseHeaders(char *text); //解析头信息     
    HttpStatus ParseContent(char *text);//解析消息体
         
    //处理http请求文件过程函数
    HttpStatus DoRequest(); //处理好登录和注册等逻辑，如果需要返回文件，把待发送相关文件大小按照写缓存参数写好
    void Unmap();  //解除文件映射

    // http内容处理函数
    bool AddResponse(const char *format, ...);         //格式化写入缓存（以下都是调用该函数完成读写）
    bool AddStatusLine(int status, const char *title); //状态行编写
    bool AddHeaders(int content_len);                  //添加消息头
    bool AddContent(const char *content);              //添加消息体

    //添加头的子函数
    bool AddContentLength(int content_len); //添加内容长度
    bool AddLingerOpt();                    //添加长连接linger选项
    bool AddBlankLine();                    //添加空白行

    //用户网络信息
    int sockfd_;             //用户已连接sockfd
    sockaddr_in address_;    //网络地址
    bool socket_linger_opt_; //长连接选项

    // http读缓存
    char read_buffers_[kREAD_BUFFER_SIZE]; //读数组
    int read_index_;                       //读索引，记录已读数据数
    int checked_idx_;                      //当前已处理http字节数
    int start_line_;                       //下一处理行首字节

    // http写缓存
    char write_buffers_[kWRITE_BUFFER_SIZE]; //写数组
    int write_index_;                        //写索引，记录已写数据（不含文件）

    //非连续内存写结构体
    struct iovec m_iv_[2];                   //用于写非连续内存结构体
    int iv_count_;                           //非连续内存个数
    int all_bytes_have_send_;                //所有已写数据（包含文件）
    int all_bytes_to_send_;                  //所有待写数据（包含文件）

    //文件映射相关
    char *file_address_;     //真实文件映射缓存地址
    struct stat file_stat_; //文件状态结构体 stat函数用于取得指定文件的文件属性，并将文件属性存储在结构体stat里
                            //包含 /* st_mode文件类型和权限 */  ，/* st_size文件大小，字节数
    char real_file[kFILENAME_LEN]; //真实文件地址


    // http信息相关变量
    CheckState check_state_; //当前http处理阶段
    HttpMethod method_ ;     //当前http处理方法
    char *url_;              //目标url
    char *version_;          // http版本号
    char *host_;             //主机信息
    int content_length_;     //请求消息体长度
    int cgi_;                //用户提交post标志
    char *heard_string_;     //请求头信息


};

#endif
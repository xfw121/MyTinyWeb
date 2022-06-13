#ifndef LIST_TIMER
#define LIST_TIMER

#include <time.h>  //获取时间
#include <sys/socket.h> //要利用到socket文件描述符
#include <sys/epoll.h>  //时间监听要用
#include <signal.h>     //信号
#include <unistd.h>     //包含大量unix系统调用接口
#include <netinet/in.h>  //定义数据结构sockaddr_in


//本项目内.h文件
#include "../http/http_conn.h"
#include "../util/util.h"       //包含epoll和sig等工具函数



class Timer;  //先声明定时器类

//资源结构体 （与定时器互相绑定）
struct ClientResource
{
    sockaddr_in address;  //网络ip地址
    int sockfd;           //文件描述符
    Timer *timer;         //加入定时器指针，互相绑定
};

//定时器结构体 （与资源结构体互相绑定）
class Timer
{
public:
    Timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;                   //过期时间
    
    void (* cb_func)(ClientResource *); //这是定时事件函数指针
    ClientResource *user_data;    //加入资源指针，互相绑定
    Timer *prev;
    Timer *next;
};


//升序链表
class SortTimerList
{
public:
    SortTimerList();
    ~SortTimerList();


    void Init( int epoll_fd_t,int *pipefd_t,int timeslot_t); //对时隙变量进行初始化

    void AddTimer(Timer *timer);        //添加定时器
    void AdjustTimer(Timer *timer);     //调整定时器
    void RemoveTimer(Timer *timer);     //去除定时器

    //信号中断函数 （必须定义静态成员函数，不然多了this参数，不能使用信号设置函数）（向管道发送数据）
    static void SigHandler(int sig);

    //定时事件处理函数
    void Tick();                       //去除超时定时器，及调用绑定的定时事件函数（释放资源），并再次启动定时

    static int *pipefd_;       //与主线程通信管道     （初始化） 
    static int epollfd_;       //事件表epollfd         （初始化）
    int TIMESLOT;            //时隙                 （初始化）

private:
    void AddTimer(Timer *timer, Timer *lst_head);

    Timer *head;
    Timer *tail;
};


void ReleseResource(ClientResource *user_data);   //释放用户资源

#endif

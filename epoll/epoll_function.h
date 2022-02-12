#ifndef EPOLL_FUNCTION_H
#define EPOLL_FUNCTION_H

//其他库
#include <sys/epoll.h>  //linux的epoll库
#include <unistd.h>     //包含大量unix系统调用接口
#include <fcntl.h>      //包含fctl使用的宏

//定义枚举类型sockefd触发模式 LT为0 ET为1 默认为LT模式
enum TriggerMode
{
    LT_MODE = 0,
    ET_MODE = 1,
} ;


//EpollAddFd 添加文件描述符到事件表函数
void EpollAddFd(int epollfd, int fd, bool one_shot_opt, TriggerMode trigger_mode);

//EpollModFd 修改文件描述符
void EpollModFd(int epollfd, int fd, int ev, int TRIGMode);

//EpollRemoveFd 删除文件描述符
void EpollRemoveFd(int epollfd, int fd);


//对文件描述符设置非阻塞
int Setnonblocking(int fd);




#endif
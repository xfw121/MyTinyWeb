#include "util.h"

//EpollAddFd 添加文件描述符到事件表函数
void EpollAddFd(int epollfd, int fd, bool one_shot_opt, TriggerMode TRIGMode)
{

    epoll_event event;
    event.data.fd = fd;

    if (ET_MODE == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot_opt)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    Setnonblocking(fd);
}

//EpollModFd 修改文件描述符
//这里都是oneshot模式
void EpollModFd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//对文件描述符设置非阻塞
int Setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//EpollRemoveFd 删除文件描述符
void EpollRemoveFd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}


//设置信号函数（restart缺省值为true，在函数声明中）
void Addsig(int sig, void(handler)(int), bool restart) {

    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);

};


//关闭socket连接，向用户发送错误信息
void ShowError(int connfd, const char *info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
};
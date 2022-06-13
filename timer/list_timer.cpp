#include "list_timer.h"


int* SortTimerList::pipefd_=nullptr;
int SortTimerList::epollfd_=0;


//升序定时器链表
SortTimerList::SortTimerList()
{
    head = NULL;
    tail = NULL;
}

//升序定时器链表 析构
SortTimerList::~SortTimerList()
{
    Timer *tmp = head;    
    while (tmp)
    {
        head = tmp->next;               // 删除定时器
        tmp->cb_func( tmp->user_data ); // 删除对应用户资源
        delete tmp;
        tmp = head;
    }
}

//定时器初始化
void SortTimerList::Init( int epoll_fd_t,int *pipefd_t,int timeslot_t) {
    pipefd_=pipefd_t;            //与主线程通信管道     （初始化）
    epollfd_=epoll_fd_t;         //事件表epollfd         （初始化）
    TIMESLOT=timeslot_t;        //时隙                 （初始化）
}; 



//添加定时器
void SortTimerList::AddTimer(Timer *timer)
{
    if (!timer)
    {
        return;
    }
    //定时器链表为空
    if (!head)
    {
        head = tail = timer;
        return;
    }
    //定时器添加到头的情况
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    AddTimer(timer, head);  //添加到head之后
}

//当改变定时器事件后，需要进行调整
void SortTimerList::AdjustTimer(Timer *timer)
{
    if (!timer)
    {
        return;
    }
    Timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        AddTimer(timer, head);
    }
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        AddTimer(timer, timer->next);
    }
}
//去除某个定时器
void SortTimerList::RemoveTimer(Timer *timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

//去除超时定时器，及调用绑定的定时事件函数（释放资源），并再次启动定时
void SortTimerList::Tick()
{
    if (!head)
    {
        alarm(TIMESLOT);  //去除定时器后，再次启动定时
        return;
    }
    
    time_t cur = time(NULL);
    Timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire)
        {
            break;
        }
        tmp->cb_func(tmp->user_data); //调用绑定的定时事件函数（释放资源
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
    alarm(TIMESLOT);  //去除定时器后，再次启动定时
}

//添加定时器到head之后的情况
void SortTimerList::AddTimer(Timer *timer, Timer *lst_head)
{
    Timer *prev = lst_head;
    Timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

//定时信号中断函数
void SortTimerList::SigHandler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd_[1], (char *)&msg, 1, 0); //将信号值通过管道发给主线程
    errno = save_errno;
}

//释放资源函数
void ReleseResource(ClientResource *user_data)  //释放用户资源
{
    epoll_ctl(SortTimerList::epollfd_, EPOLL_CTL_DEL, user_data->sockfd, 0);   //监听事件的删除
    assert(user_data);  
    close(user_data->sockfd);                                                 //sockfd的关闭
    HttpConn::client_counts_--;
}

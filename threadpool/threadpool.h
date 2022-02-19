#ifndef THREAD_POOL_H
#define THREAD_POOL_H

// c库文件

// c++标准库
#include <list>

//其他库
#include <pthread.h>

//本项目内.h文件
#include "../lock/locker.h"
#include "../cgi_mysql/sql_connection_pool.h"
#include "../http/http_conn.h"

// 并发模式类型
enum ConcurrencyModel
{
    REACTOR_MODEL = 0,
    PROACTOR_MODEL = 1
};

template <typename T>
class ThreadPool
{
public:
    /*threadpool_max_t是线程池中线程的数量，requests_list_max_t是请求队列中最多允许的、等待处理的请求的数量*/
    ThreadPool(ConcurrencyModel concurrency_model_t, connection_pool *sql_conn_pool_t, int threadpool_max_t = 8, int requests_list_max_t = 10000);
    ~ThreadPool();
    bool ReactorAppendRequests(T *request, int state);  //Reactor模式添加队列，0读，1写
    bool ProactorAppendRequests(T *request);            //Proactor模式添加队列

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *Worker(void *arg);
    void Run();

private:
    int threadpool_max_;             //线程池中的线程数
    int requests_list_max_;          //请求队列中允许的最大请求数
    pthread_t *threads_;             //线程池的描述符数组，其大小为threadpool_max_
    std::list<T *> workqueue_;       //请求队列
    Locker workqueue_locker_;        //保护请求队列的互斥锁
    Sem workqueue_sem_;              //是否有任务需要处理
    connection_pool *sql_conn_pool_; //数据库连接池
    ConcurrencyModel concurrency_model_;          //模型切换
};

template <typename T>
ThreadPool<T>::ThreadPool(ConcurrencyModel concurrency_model_t, connection_pool *sql_conn_pool_t, int threadpool_max_t, int requests_list_max_t)
    : concurrency_model_(concurrency_model_t), threadpool_max_(threadpool_max_t),
      requests_list_max_(requests_list_max_t), threads_(NULL),
      sql_conn_pool_(sql_conn_pool_t)
{
    //判断线程池和请求队列数量是否大于0
    if (threadpool_max_ <= 0 || requests_list_max_ <= 0)
        throw std::exception();
    //构造线程池描述符数组
    threads_ = new pthread_t[threadpool_max_];
    if (!threads_)
        throw std::exception();

    //创建线程池，运行
    for (int i = 0; i < threadpool_max_t; ++i)
    {
        //创建线程
        if (pthread_create(threads_ + i, NULL, Worker, this) != 0)
        {
            delete[] threads_;
            throw std::exception();
        }
        //设置线程结束自动回收资源
        if (pthread_detach(threads_[i]))
        {
            delete[] threads_;
            throw std::exception();
        }
    }
};

template <typename T>
ThreadPool<T>::~ThreadPool()
{
    //回收线程描述符
    delete[] threads_;
}
#endif

template <typename T>
bool ThreadPool<T>::ReactorAppendRequests(T *request, int state)
{
    //加锁，添加消息
    workqueue_locker_.Lock();
    if (workqueue_.size() >= requests_list_max_)
    {
        workqueue_locker_.Unlock();
        return false;
    }
    request->http_state_ = state;//设置消息读写状态
    workqueue_.push_back(request); //加入消息队列
    //解锁，消息队列
    workqueue_locker_.Unlock();
    //添加信号量，来保存队列数
    workqueue_sem_.Post();
    return true;
}

template <typename T>
bool ThreadPool<T>::ProactorAppendRequests(T *request)
{
    //添加锁，保护消息队列
    workqueue_locker_.Lock();
    if (workqueue_.size() >= requests_list_max_)
    {
        workqueue_locker_.Unlock();
        return false;
    }
    workqueue_.push_back(request);  //加入消息队列
    //解锁，消息队列
    workqueue_locker_.Unlock();
    //添加信号量，来保存队列数
    workqueue_sem_.Post();
    return true;
}

template <typename T>
void *ThreadPool<T>::Worker(void *arg)
{
    //输入为空指针，需强制转化
    ThreadPool *pool = (ThreadPool *)arg;
    pool->Run();    //运行实际操作
    return pool;
}

template <typename T>
void ThreadPool<T>::Run()
{
    while (true)
    {
        //判断消息队列是否有消息，没有则阻塞
        workqueue_sem_.Wait();

        //加锁，取出消息
        workqueue_locker_.Lock();
        if (workqueue_.empty())
        {
            workqueue_locker_.Unlock();
            continue;
        }
        T *request = workqueue_.front();   //取出消息
        workqueue_.pop_front();            //注意更新队列
        workqueue_locker_.Unlock();             //解锁

        //进行处理
        //没有消息
        if (!request)
            continue;
        //REACTOR模式
        if (REACTOR_MODEL == concurrency_model_)
        {   
            //读状态
            if (0 == request->http_state_)
            {
                //先读取内容到http缓存
                if (request->ReadOnce())
                {
                    request->completed_flag_ = 1;    //告诉主线程已经读取完
                    //从数据库连接池获取一个连接到mysql
                    // connectionRAII mysqlcon(&request->mysql, m_connPool);
                    //对请求处理
                    request->HttpProcess();
                }
                else
                {  
                    request->completed_flag_ = 1; //告诉主线程已经读取完
                    request->timerout_flag_ = 1; //因为失败了，所以标记超时，清除该连接
                }
            }
            //写状态
            else
            {
                if (request->Write())
                {
                    request->completed_flag_ = 1; //告诉主线程已经写完
                }
                else
                {
                    request->completed_flag_ = 1; //告诉主线程已经写完
                    request->timerout_flag_ = 1; //因为失败了，所以标记超时，清除该连接
                }
            }
        }
        //PROACTOR模式
        else if(PROACTOR_MODEL == concurrency_model_)
        {
            //从数据库连接池获取一个连接到mysql
            // connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->HttpProcess();
        }
    }
}
#ifndef LOCKER_H
#define LOCKER_H

//c库文件

//c++标准库
#include <list>
//其他库
#include <pthread.h>    //线程
#include <exception>     //异常
#include <semaphore.h>  //信号量
//本项目内.h文件


class Sem
{
public:
    Sem()
    {
        if (sem_init(&sem_, 0, 0) != 0)
        {
            throw std::exception();
        }
    }
    Sem(int num)
    {
        if (sem_init(&sem_, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~Sem()
    {
        sem_destroy(&sem_);
    }
    bool Wait()
    {
        return sem_wait(&sem_) == 0;
    }
    bool Post()
    {
        return sem_post(&sem_) == 0;
    }

private:
    sem_t sem_;
};
class Locker
{
public:
    Locker()
    {
        if (pthread_mutex_init(&Mitex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~Locker()
    {
        pthread_mutex_destroy(&Mitex);
    }
    bool Lock()
    {
        return pthread_mutex_lock(&Mitex) == 0;
    }
    bool Unlock()
    {
        return pthread_mutex_unlock(&Mitex) == 0;
    }
    pthread_mutex_t *get()
    {
        return &Mitex;
    }

private:
    pthread_mutex_t Mitex;
};
class Cond
{
public:
    Cond()
    {
        if (pthread_cond_init(&cond_, NULL) != 0)
        {
            //pthread_mutex_destroy(&Mitex);
            throw std::exception();
        }
    }
    ~Cond()
    {
        pthread_cond_destroy(&cond_);
    }
    bool Wait(pthread_mutex_t *Mitex)
    {
        int ret = 0;
        //pthread_mutex_lock(&Mitex);
        ret = pthread_cond_wait(&cond_, Mitex);
        //pthread_mutex_unlock(&Mitex);
        return ret == 0;
    }
    bool Timewait(pthread_mutex_t *Mitex, struct timespec t)
    {
        int ret = 0;
        //pthread_mutex_lock(&Mitex);
        ret = pthread_cond_timedwait(&cond_, Mitex, &t);
        //pthread_mutex_unlock(&Mitex);
        return ret == 0;
    }
    bool Signal()
    {
        return pthread_cond_signal(&cond_) == 0;
    }
    bool Broadcast()
    {
        return pthread_cond_broadcast(&cond_) == 0;
    }

private:
    //static pthread_mutex_t Mitex;
    pthread_cond_t cond_;
};

#endif

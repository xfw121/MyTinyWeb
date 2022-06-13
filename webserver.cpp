#include "webserver.h"

WebServer::WebServer()
{

    // epoll创建内核事件表
    epoll_fd_ = epoll_create(5);
    assert(epoll_fd_ != -1);

    //对客户网页处理缓存进行初始化    client的http_conn类对象首指针
    http_conns_ = new HttpConn[kFD_MAX];

    //对用户epoll和socket触发模式进行设置
    http_conns_->epoll_fd_ = epoll_fd_;                           //对用户连接的epoll_fd也进行更新
    HttpConn::socket_trigger_mode_ = client_socket_trigger_mode_; //用户触发模式设置

    //设置http根目录路径
    char server_path[200];
    getcwd(server_path, 200); //获取服务器路径
    //设置根路径
    HttpConn::root_path_ = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(HttpConn::root_path_, server_path); //先添加服务器路径在前面
    strcat(HttpConn::root_path_, root);        //然后添加root路径

    //定时器
    client_resource_ = new ClientResource[kFD_MAX];
};

WebServer::~WebServer()
{

    close(epoll_fd_);      //关闭事件fd
    close(web_socket_fd_); //关闭监听fd
    close(pipefd_[1]);     //关闭管道
    close(pipefd_[0]);     //关闭管道

    delete[] http_conns_;      //消除client的http_conn类对象首指针
    delete[] client_resource_; //消除用户资源
    delete thread_pool_;       //消除线程池
};

// Web参数配置
void WebServer::ParameterSet(int argc, char *argv[])
{

    //解析命令，并设置参数
    int opt;
    const char *str = "p:l:m:o:s:t:c:a:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            web_port_ = atoi(optarg);
            break;
        }
        case 'a':
        {
            if(*optarg == 'r')
                concurrency_model_ = REACTOR_MODEL;
            else if(*optarg == 'p')
                concurrency_model_ = PROACTOR_MODEL;
            break;
        }
            // case 'm':
            // {
            //     TRIGMode = atoi(optarg);
            //     break;
            // }
            // case 'o':
            // {
            //     web_socket_linger_opt_ = atoi(optarg);
            //     break;
            // }
            // case 's':
            // {
            //     sql_num = atoi(optarg);
            //     break;
            // }
            // case 't':
            // {
            //     thread_num = atoi(optarg);
            //     break;
            // }
            // case 'c':
            // {
            //     close_log = atoi(optarg);
            //     break;
            // }

        default:
            break;
        }
    }
    //设置数据库参数
}

//监听socket初始化,并开始监听
void WebServer::SocketInit()
{
    //获取socket，设置为ipv4协议族，流数据
    web_socket_fd_ = socket(PF_INET, SOCK_STREAM, 0);
    assert(web_socket_fd_ >= 0);

    //设置优雅关闭连接（优雅关闭会把还没有发送的数据发送完再关闭）
    if (0 == web_socket_linger_opt_)
    {
        struct linger tmp = {0, 1};
        setsockopt(web_socket_fd_, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == web_socket_linger_opt_)
    {
        struct linger tmp = {1, 1};
        setsockopt(web_socket_fd_, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    //定义Ipv4协议族地址结构 该结构体内都是以网络字节顺序保存
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY); // INADDR_ANY为任意可用网卡地址
    address.sin_port = htons(web_port_);
    //设置socket重用地址选项为开启,允许服务器关闭后，又能重用地址，方便服务器反复测试
    int flag = 1;
    setsockopt(web_socket_fd_, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    //绑定监听fd到网卡地址
    int ret = 0;
    ret = bind(web_socket_fd_, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    //将监听fd加入监听队列
    ret = listen(web_socket_fd_, 5);
    assert(ret >= 0);
}

//定时器初始化，并启动定时器
void WebServer::ListTimerInit()
{

    //初始化管道(与定时器沟通使用)
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd_); //获取管道
    assert(ret != -1);
    Setnonblocking(pipefd_[1]);
    EpollAddFd(epoll_fd_, pipefd_[0], false, LT_MODE);

    //先初始化定时器时隙和监听符和管道
    timer_list_.Init(epoll_fd_, pipefd_, kTIMESLOT);

    Addsig(SIGPIPE, SIG_IGN);                       //忽略SIGCHLD信号，这常用于并发服务器的性能的一个技巧，因为并发服务器常常fork很多子进程，
                                                    //子进程终结之后需要服务器进程去wait清理资源。如果将此信号的处理方式设为忽略，
                                                    //可让内核把僵尸子进程转交给init进程去处理，省去了大量僵尸进程占用系统资源
    Addsig(SIGALRM, timer_list_.SigHandler, false); //定时信号
    Addsig(SIGTERM, timer_list_.SigHandler, false); //进程终止信号

    alarm(kTIMESLOT); //开启定时
}

//数据库连接池初始化
void WebServer::SqlPoolInit()
{

    //初始化数据库连接池
    mysql_pool_ = ConnectionPool::GetInstance();
    mysql_pool_->Init("localhost", sql_user_, sql_password_, database_name_, 3306, sql_max_);

    //读取数据库中所有用户信息，进行存储在users_cache_中
    http_conns_->MysqlResultInit(mysql_pool_);
}

//线程池初始话
void WebServer::ThreadPoolInit()
{
    //线程池
    thread_pool_ = new ThreadPool<HttpConn>(concurrency_model_, mysql_pool_, threadpool_max_);
}

//主线程（将监听socketfd加入epoll监听事件表，并开始循环处理事件）
void WebServer::EventLoop()
{
    //将监听socketfd加入epoll监听事件表
    EpollAddFd(epoll_fd_, web_socket_fd_, false, web_socket_trigger_mode_);

    //开始循环
    bool clock_flag(false);
    bool stop_server_flag(false);

    while (!stop_server_flag)
    {
        int epoll_N = 0;

        epoll_N = epoll_wait(epoll_fd_, events_, kEVENT_MAX, -1); //返回事件数量

        if (epoll_N < 0 && errno != EINTR)
        {
            cout << "\nepoll failure\n";
            break;
        }

        for (int i = 0; i < epoll_N; i++)
        {
            int sockfd = events_[i].data.fd; //获取事件sockfd

            //处理新到的客户连接，并开始监听读事件
            if (sockfd == web_socket_fd_)
            {
                //处理用户新连接
                bool flag = HandleNewConn();
                if (false == flag)
                    continue;
            }
            //服务器端关闭连接，移除对应的定时器
            // EPOLLRDHUP对端关闭时
            // EPOLLHUP 本端描述符产生一个挂断事件，默认监测事件
            // EPOLLERR 描述符产生错误时触发，默认检测事件
            else if (events_[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //移除对应的定时器
                Timer *timer = client_resource_[sockfd].timer;
                DelTimer(timer, sockfd);
                cout << "\n断开连接事件\n";
            }
            //处理定时器发送的信号
            else if ((sockfd == pipefd_[0]) && (events_[i].events & EPOLLIN))
            {
                bool flag = DealWithSigna(clock_flag, stop_server_flag);
                if (false == flag)
                    cout << "管道接收失败" << endl;
            }
            //处理客户连接上接收到的数据
            else if (events_[i].events & EPOLLIN)
            {
                HandleReadEvent(sockfd);
            }
            //处理要返回给客户连接的数据
            else if (events_[i].events & EPOLLOUT)
            {
                cout << "\n检测到写事件,准备处理写事件\n";
                HandleWriteEvent(sockfd);
                cout << "\n已经响应客户,发送了请求文件\n";
            }
        }

        //处理超时的不活跃连接
        if (clock_flag)
        {
            timer_list_.Tick(); //定时事件处理
            cout << "\n时钟信号到了\n";
            clock_flag = false;
        }
    }
}

//处理新client连接
bool WebServer::HandleNewConn()
{
    struct sockaddr_in client_address_t;                      //网络地址临时变量
    socklen_t client_addrlength_t = sizeof(client_address_t); //客户长度临时变量

    // LT模式只需处理一个连接即可
    if (LT_MODE == web_socket_trigger_mode_)
    {
        //获取已连接fd
        int connfd = accept(web_socket_fd_, (struct sockaddr *)&client_address_t, &client_addrlength_t);
        //判断连接是否错误
        if (connfd < 0)
        {
            cout << "\n接收新连接错误\n";
            return false;
        }
        HttpConn::client_counts_++; //用户数量记录增加
        //判断是否超过最大用户连接数
        if (HttpConn::client_counts_ >= kFD_MAX)
        {
            cout << "\n超过最大连接数";
            ShowError(connfd, "Internal server busy");
            return false;
        }
        //将已连接client对应的http初始化,开始监听读事件
        http_conns_[connfd].Init(connfd, client_address_t);
        cout << "\n已处理完新连接,已注册新连接的EPOLLIN事件\n";

        //创建已连接client的定时器
        AddTimer(connfd, client_address_t);
    }
    // ET模式必须读完所有连接，否则状态会被清空
    else
    {
        while (1)
        {
            int connfd = accept(web_socket_fd_, (struct sockaddr *)&client_address_t, &client_addrlength_t);
            if (connfd < 0)
            {
                cout << "\n接收新连接错误\n"; //报错
                break;
            }
            if (HttpConn::client_counts_ >= kFD_MAX)
            {
                cout << "\n超过最大连接数";
                ShowError(connfd, "Internal server busy");
                break;
            }
            //将已连接client对应的http初始化,开始监听读事件
            http_conns_[connfd].Init(connfd, client_address_t);
            //创建已连接client的定时器
            AddTimer(connfd, client_address_t);
        }
        return false;
    }
    return true;
}

//处理client读事件
void WebServer::HandleReadEvent(int sockfd)
{
    //获取当前客户sockfd的定位器
    Timer *timer = client_resource_[sockfd].timer;

    // reactor模式，线程池来完成读
    if (REACTOR_MODEL == concurrency_model_)
    {
        cout << "\n当前为Reactor模式,开始处理读事件\n";

        //因为当前在读取该sockd文件要花费时间，所以延时该sockfd的到期时间
        if (timer)
        {
            DelayTimer(timer);
        }
        // reactor模式，若监测到读事件，将该事件放入请求队列，
        //并注册其读状态，让线程池来读
        thread_pool_->ReactorAppendRequests(http_conns_ + sockfd, 0);

        //等线程池完成读事件。
        while (true)
        {
            //线程池完成读事件
            if (1 == http_conns_[sockfd].completed_flag_)
            {
                //处理连接超时
                if (1 == http_conns_[sockfd].close_conn_flag_)
                {
                    DelTimer(timer, sockfd);
                    cout << "\n连接处理超时\n";
                    http_conns_[sockfd].close_conn_flag_ = 0;
                }
                http_conns_[sockfd].completed_flag_ = 0;
                break;
            }
        }
    }
    // proactor模式，主线程来处理读
    else if (PROACTOR_MODEL == concurrency_model_)
    {

        cout << "\n当前为Proactor模式,开始处理读事件\n";

        // proactor模式，主线程直接进行读取操作
        if (http_conns_[sockfd].ReadOnce())
        {
            //若监测到读事件，将该事件放入请求队列
            thread_pool_->ReactorAppendRequests(http_conns_ + sockfd, 0); //在proactor模式下，只有请求事件加入请求队列，因此不用标记是读状态还是写状态。

            //因为当前在读取该sockd文件要花费时间，所以延时该sockfd的到期时间
            if (timer)
            {
                DelayTimer(timer);
            }
        }
        else
        {
            //直接关闭该连接和定时器资源
            DelTimer(timer, sockfd);
        }
    }
}

//处理向client写事件
void WebServer::HandleWriteEvent(int sockfd)
{
    //获取当前客户sockfd的定位器
    Timer *timer = client_resource_[sockfd].timer;

    // reactor模式，线程池来完成写
    if (REACTOR_MODEL == concurrency_model_)
    {
        //因为当前在写该sockd文件要花费时间，所以延时该sockfd的到期时间
        if (timer)
        {
            DelayTimer(timer);
        }

        // reactor模式，若监测到写事件，将该事件放入请求队列，
        thread_pool_->ReactorAppendRequests(http_conns_ + sockfd, 1); //并注册写状态，通过让线程池来写
        //等线程池完成写事件。
        while (true)
        {
            //等线程池完成写事件。
            if (1 == http_conns_[sockfd].completed_flag_)
            {
                //处理连接超时
                if (1 == http_conns_[sockfd].close_conn_flag_)
                {
                    DelTimer(timer, sockfd);
                    http_conns_[sockfd].close_conn_flag_ = 0;
                }
                http_conns_[sockfd].completed_flag_ = 0;
                break;
            }
        }
    }
    // proactor模式，线程池来完成写
    else if (PROACTOR_MODEL == concurrency_model_)
    {
        // proactor模式，主线程直接进行写操作
        if (http_conns_[sockfd].Write())
        {
            //因为当前在写该sockd文件要花费时间，所以延时该sockfd的到期时间
            if (timer)
            {
                DelayTimer(timer);
            }
        }
        else
        {
            //直接关闭该连接和定时器资源
            DelTimer(timer, sockfd);
        }
    }
}

//添加定时器
void WebServer::AddTimer(int connfd, struct sockaddr_in client_address)
{

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    client_resource_[connfd].address = client_address;
    client_resource_[connfd].sockfd = connfd;
    Timer *timer = new Timer;
    timer->user_data = &client_resource_[connfd];
    timer->cb_func = ReleseResource; //将定时事件绑定释放资源函数
    time_t cur = time(NULL);
    timer->expire = cur + 3 * kTIMESLOT; //超时时间设置为3各时隙后
    client_resource_[connfd].timer = timer;
    timer_list_.AddTimer(timer); //添加定时器
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::DelayTimer(Timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * kTIMESLOT;
    timer_list_.AdjustTimer(timer);
}
//删除定时器
void WebServer::DelTimer(Timer *timer, int sockfd)
{
    timer->cb_func(&client_resource_[sockfd]); //执行定时事件，关闭资源
    if (timer)
    {
        timer_list_.RemoveTimer(timer);
    }
}

bool WebServer::DealWithSigna(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(pipefd_[0], signals, sizeof(signals), 0); //接收管道到来的数据
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM: //如果是定时事件，标记定时标记
            {
                timeout = true;
                break;
            }
            case SIGTERM: //如果是进程终止信号，停止服务器运行
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}
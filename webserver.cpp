#include "webserver.h"

WebServer::WebServer()
{

    //对客户网页处理缓存进行初始化    client的http_conn类对象首指针
    http_conns_ = new HttpConn[kFD_MAX];

    //设置http根目录路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    HttpConn::root_path_ = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(HttpConn::root_path_, server_path);
    strcat(HttpConn::root_path_, root);

    //定时器
};

WebServer::~WebServer()
{

    close(epoll_fd_);
    close(web_socket_fd_);

    //消除client的http_conn类对象首指针
    delete[] http_conns_;
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
        // case 'l':
        // {
        //     LOGWrite = atoi(optarg);
        //     break;
        // }
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
        // case 'a':
        // {
        //     actor_model = atoi(optarg);
        //     break;
        // }
        default:
            break;
        }
    }
    //设置数据库参数
}

//日志初始化
void WebServer::Log_Init()
{
    //各部件日志开关设置

    if (1 == log_switch_)
    {
        //初始化日志
        // if (1 == m_log_write)
        //     Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        // else
        //     Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
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
void WebServer::ListTimerInit() {

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
    // epoll创建内核事件表
    epoll_fd_ = epoll_create(5);
    assert(epoll_fd_ != -1);

    //对用户epoll和socket触发模式进行设置
    http_conns_->epoll_fd_ = epoll_fd_;                           //对用户连接的epoll_fd也进行更新
    HttpConn::socket_trigger_mode_ = client_socket_trigger_mode_; //用户触发模式设置

    //将监听socketfd加入epoll监听事件表
    EpollAddFd(epoll_fd_, web_socket_fd_, false, web_socket_trigger_mode_);

    //开始循环
    bool clock_flag(false);
    bool stop_server_flag(false);

    while (!stop_server_flag)
    {
        int number = 0;

        number = epoll_wait(epoll_fd_, events_, kEVENT_MAX, -1);

        if (number < 0 && errno != EINTR)
        {
            // LOG_ERROR("%s", "epoll failure");
            cout << "\nepoll failure\n";
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int new_sockfd = events_[i].data.fd;

            //处理新到的客户连接，并开始监听读事件
            if (new_sockfd == web_socket_fd_)
            {
                //处理用户新连接
                bool flag = HandleNewConn();
                if (false == flag)
                    continue;
            }
            //服务器端关闭连接，移除对应的定时器
            else if (events_[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                cout << "\n断开连接事件\n";
            }
            //处理定时器发送的信号
            // else if ((new_sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            // {
            // }
            //处理客户连接上接收到的数据
            else if (events_[i].events & EPOLLIN)
            {
                HandleReadEvent(new_sockfd);
            }
            //处理要返回给客户连接的数据
            else if (events_[i].events & EPOLLOUT)
            {
                cout << "\n检测到写事件,准备处理写事件\n" ;
                HandleWriteEvent(new_sockfd);
                cout << "\n已经响应客户,发送了请求文件\n" ;
            }
        }

        //处理超时的不活跃连接
        if (clock_flag)
        {
            cout << "\n时钟信号到了\n" ;
            clock_flag=false;
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
            //报错
            cout << "\n接收新连接错误\n";
            // LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        //判断是否超过最大用户连接数
        if (HttpConn::client_counts_ >= kFD_MAX)
        {
            cout << "\n超过最大连接数";
            // utils.show_error(connfd, "Internal server busy");
            // LOG_ERROR("%s", "Internal server busy");
            return false;
        }

        //将已连接client对应的http初始化,开始监听读事件
        http_conns_[connfd].Init(connfd, client_address_t);

        cout << "\n已处理完新连接,已注册新连接的EPOLLIN事件\n";
        //创建已连接client的定时器
        // timer(connfd, client_address_ );
    }
    // ET模式必须读完所有连接，否则状态会被清空
    else
    {
        while (1)
        {
            int connfd = accept(web_socket_fd_, (struct sockaddr *)&client_address_t, &client_addrlength_t);
            if (connfd < 0)
            {
                //报错
                cout << "\n接收新连接错误\n";
                // LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }

            if (HttpConn::client_counts_ >= kFD_MAX)
            {
                cout << "\n超过最大连接数";
                // utils.show_error(connfd, "Internal server busy");
                // LOG_ERROR("%s", "Internal server busy");
                break;
            }

            //将已连接client对应的http初始化,开始监听读事件
            http_conns_[connfd].Init(connfd, client_address_t);

            //创建已连接client的定时器
            // timer(connfd, client_address_ );
        }
        return false;
    }
    return true;
}

//处理client读事件
void WebServer::HandleReadEvent(int sockfd)
{
    //获取当前客户sockfd的定位器
    // util_timer *timer = users_timer[sockfd].timer;

    // reactor模式，线程池来完成读
    if (REACTOR_MODEL == concurrency_model_)
    {
        cout << "\n当前为Reactor模式,开始处理读事件\n";

        //因为当前在读取该sockd文件要花费时间，所以延时该sockfd的到期时间
        // if (timer)
        // {
        //     adjust_timer(timer);
        // }

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
                if (1 == http_conns_[sockfd].timerout_flag_)
                {
                    // deal_timer(timer, sockfd);
                    cout<<"\n连接处理超时\n";
                    http_conns_[sockfd].timerout_flag_= 0;
                }
                http_conns_[sockfd].completed_flag_= 0;
                break;
            }
        }
    }
    // proactor模式，主线程来处理读
    else if(PROACTOR_MODEL == concurrency_model_)
    {

        cout << "\n当前为Proactor模式,开始处理读事件\n";

        // proactor模式，主线程直接进行读取操作
        if (http_conns_[sockfd].ReadOnce())
        {
            // LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            thread_pool_->ReactorAppendRequests(http_conns_ + sockfd, 0);    //在proactor模式下，只有请求事件加入请求队列，因此不用标记是读状态还是写状态。

            //因为当前在读取该sockd文件要花费时间，所以延时该sockfd的到期时间
            // if (timer)
            // {
            //     adjust_timer(timer);
            // }
        }
        else
        {
            //直接关闭该连接和定时器资源
            // deal_timer(timer, sockfd);
        }
    }
}

//处理向client写事件
void WebServer::HandleWriteEvent(int sockfd)
{
    //获取当前客户sockfd的定位器
    // util_timer *timer = users_timer[sockfd].timer;

    // reactor模式，线程池来完成写
    if (REACTOR_MODEL == concurrency_model_)
    {
        //因为当前在写该sockd文件要花费时间，所以延时该sockfd的到期时间
        // if (timer)
        // {
        //     adjust_timer(timer);
        // }

        // reactor模式，若监测到写事件，将该事件放入请求队列，
        thread_pool_->ReactorAppendRequests(http_conns_ + sockfd, 1);        //并注册写状态，通过让线程池来写
        //等线程池完成写事件。
        while (true)
        {
            //等线程池完成写事件。
            if (1 == http_conns_[sockfd].completed_flag_)
            {
                //处理连接超时
                if (1 == http_conns_[sockfd].timerout_flag_)
                {
                    // deal_timer(timer, sockfd);
                    cout<<"\n连接处理超时\n";
                    http_conns_[sockfd].timerout_flag_= 0;
                }
                http_conns_[sockfd].completed_flag_ = 0;
                break;
            }
        }
    }
    // proactor模式，线程池来完成写
    else if(PROACTOR_MODEL == concurrency_model_)
    {
        // proactor模式，主线程直接进行写操作
        if (http_conns_[sockfd].Write())
        {
            // LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //因为当前在写该sockd文件要花费时间，所以延时该sockfd的到期时间
            // if (timer)
            // {
            //     adjust_timer(timer);
            // }
        }
        else
        {
            //直接关闭该连接和定时器资源
            // deal_timer(timer, sockfd);
        }
    }
}

#include "http_conn.h"
#include <iostream>

//静态变量
int HttpConn::client_counts_ = 0;                     //用户数量
int HttpConn::epoll_fd_ = -1;                         // epoll事件表fd
TriggerMode HttpConn::socket_trigger_mode_ = LT_MODE; //用户socket模式
char *HttpConn::root_path_ = NULL;                    //根目录路径
int HttpConn::log_switch_ = 0;                        //日志默认0关闭

//数据库相关
map<string, string> cgi_users_; //用户登录信息缓存
Locker sql_lock_;               //保护信息缓存的读写

//定义http响应的一些状态信息
const char *kOK_200_TITLE = "OK";
const char *kERROR_400_TITLE = "Bad Request";
const char *kERROR_400_FORM = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *kERROR_403_TITLE = "Forbidden";
const char *kERROR_403_FORM = "You do not have permission to get file form this server.\n";
const char *kERROR_404_TITLE = "Not Found";
const char *kERROR_404_FORM = "The requested file was not found on this server.\n";
const char *kERROR_500_TITLE = "Internal Error";
const char *kERROR_500_FORM = "There was an unusual problem serving the request file.\n";

// Http初始化新连接 _t表示临时变量
void HttpConn::Init(int sockfd_t, const sockaddr_in &address_t)
{
    // socket相关配置
    sockfd_ = sockfd_t;
    address_ = address_t;

    //开始监听已连接用户请求的事件 因为后面还要进行数据处理，可能需要花点时间，所以尽量早点监听，防止用户等待
    //关于oneshot模式，除非重新设置该sockfd，否则只会触发一次，这样每次只在线程处理完读写后，再重新设置。
    EpollAddFd(epoll_fd_, sockfd_, true, socket_trigger_mode_);

    // Http初始化
    client_counts_++; //用户数量增加

    HttpClear();
}

//清空http读写缓存及状态
void HttpConn::HttpClear()
{

    //清除读缓存及状态
    memset(read_buffers_, '\0', kREAD_BUFFER_SIZE); //清零读数组
    read_index_ = 0;                                //读索引，记录已读数据数

    //清除写缓存及状态
    memset(write_buffers_, '\0', kWRITE_BUFFER_SIZE); //清零写数组
    write_index_ = 0;                                 //写索引，记录已写数据（不含文件）
    all_bytes_have_send_ = 0;                         //所有已写数据（包含文件）
    all_bytes_to_send_ = 0;                           //所有待写数据（包含文件）

    //状态标志清除
    completed_flag_ = 0; //用于reactor模式下，提示主线程，已经完成http缓存的读取/写入
    http_state_ = 0;     // http读写状态 0读 1写
    timerout_flag_ = 0;  // http处理超时连接

    // http处理状态
    check_state_ = CHECK_STATE_REQUESTLINE; //初始化到请求处理行状态
    method_ = GET;                          //默认处理get请求
    cgi_ = 0;                               //用户登录提交提醒
    checked_idx_ = 0;                       //已处理http字节数
    start_line_ = 0;                        //待处理的下一行的头地址
    memset(real_file, '\0', kFILENAME_LEN); //返回文件路径地址

    // http信息
    socket_linger_opt_ = false; //长连接选项
    url_ = NULL;                //请求网络文件路径
    version_ = NULL;            // http版本号
    content_length_ = 0;        //消息体长度
    host_ = NULL;               //主机信息

    // mysql
    mysql_ = NULL; //数据库连接
}

//读取内核接收缓存到http缓存处，
//此次不一定会读完整个http
//如果线程处理后发现不是完整http，那么会重新激发读。
bool HttpConn::ReadOnce()
{
    //超过最大接收则发生错误
    if (read_index_ >= kREAD_BUFFER_SIZE)
    {
        return false;
    }

    //
    int read_num = 0;

    // LT模式读取数据
    if (LT_MODE == socket_trigger_mode_)
    {
        //更新
        read_num = recv(sockfd_, read_buffers_ + read_index_, kREAD_BUFFER_SIZE - read_index_, 0);
        read_index_ += read_num;

        //读取错误
        if (read_num <= 0)
        {
            return false;
        }

        return true;
    }
    // ET模式读数据
    else
    {
        while (true)
        {
            read_num = recv(sockfd_, read_buffers_ + read_index_, kREAD_BUFFER_SIZE - read_index_, 0);
            if (read_num == -1)
            {
                //如果设置socket为非阻塞，当出现EAGAIN时，意思是缓存区已经读完了
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                //不是EAGAIN则错误
                return false;
            }
            //有读事件，但读取数为0
            else if (read_num == 0)
            {
                return false;
            }

            //更新已读字节数
            read_index_ += read_num;
        }
        return true;
    }
}

//写入http缓存到内核发送缓存
bool HttpConn::Write()
{
    //已发送数据
    int send_num = 0;

    //如果不用回应，则继续监听该连接请求
    if (all_bytes_to_send_ == 0)
    {
        EpollModFd(epoll_fd_, sockfd_, EPOLLIN, socket_trigger_mode_);
        HttpClear();
        return true;
    }

    while (1)
    {
        send_num = writev(sockfd_, m_iv_, iv_count_);

        //要么是错误，要么是EAGAIN表示当前待发送缓冲区已经写满了，需要等待（可以是另外的进程来处理）
        if (send_num < 0)
        {
            if (errno == EAGAIN)
            {
                EpollModFd(epoll_fd_, sockfd_, EPOLLOUT, socket_trigger_mode_);
                return true;
            }
            // unmap();取消文件映射
            return false;
        }

        //更新已写数据
        all_bytes_have_send_ += send_num;
        all_bytes_to_send_ -= send_num;

        //更新miv
        //发送文件部分
        if (all_bytes_have_send_ >= m_iv_[0].iov_len)
        {
            m_iv_[0].iov_len = 0;
            m_iv_[1].iov_base = file_address_ + (all_bytes_have_send_ - write_index_);
            m_iv_[1].iov_len = all_bytes_to_send_;
        }
        //发送网页部分
        else
        {
            m_iv_[0].iov_base = write_buffers_ + all_bytes_have_send_;
            m_iv_[0].iov_len = m_iv_[0].iov_len - all_bytes_have_send_;
        }

        //数据发完了，如果linger长连接，则继续保持连接，否则返回false断掉连接。
        if (all_bytes_to_send_ <= 0)
        {
            // unmap();取消文件映射
            EpollModFd(epoll_fd_, sockfd_, EPOLLIN, socket_trigger_mode_);

            if (socket_linger_opt_)
            {
                HttpClear();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

//关闭该已连接用户
void HttpConn::CloseConn(bool close_opt)
{
    if (close_opt && (sockfd_ != -1))
    {
        printf("close %d\n", sockfd_);
        EpollRemoveFd(epoll_fd_, sockfd_);
        sockfd_ = -1;
        client_counts_--;
    }
}

// http处理用户请求函数
void HttpConn::HttpProcess()
{
    //处理http请求，并将文件写入缓存，并返回http回应状态
    HttpStatus read_ret = ProcessRead();
    if (read_ret == NO_REQUEST)
    {
        //因为还没有读取完整http报文，继续监听
        EpollModFd(epoll_fd_, sockfd_, EPOLLIN, socket_trigger_mode_);
        return;
    }

    //根据回应状态，将http内容写入缓存/请求文件映射mmap，确定发送数据地址和大小
    bool write_ret = ProcessWrite(read_ret);
    if (!write_ret)
    {
        CloseConn(sockfd_);
        std::cout << "写入错误\n";
    }
    //注册写事件
    EpollModFd(epoll_fd_, sockfd_, EPOLLOUT, socket_trigger_mode_);
}

//读取http内容，处理请求（主状态机）
HttpConn::HttpStatus HttpConn::ProcessRead()
{

    LineStatus line_status = LINE_OK;
    HttpStatus ret = NO_REQUEST;
    char *text = 0;

    while ((check_state_ == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = GetLine();
        start_line_ = checked_idx_;
        // LOG_INFO("%s", text);
        switch (check_state_)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = ParseRequestLine(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = ParseHeaders(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return DoRequest();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = ParseContent(text);
            if (ret == GET_REQUEST)
                return DoRequest();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

//根据回应状态，将http内容写入缓存/请求文件映射mmap，确定发送数据地址和大小
bool HttpConn::ProcessWrite(HttpStatus ret)
{
    switch (ret)
    {

    case INTERNAL_ERROR:
    {
        AddStatusLine(500, kERROR_500_TITLE);
        AddHeaders(strlen(kERROR_500_FORM));
        if (!AddContent(kERROR_500_FORM))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        AddStatusLine(404, kERROR_404_TITLE);
        AddHeaders(strlen(kERROR_404_FORM));
        if (!AddContent(kERROR_404_FORM))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        AddStatusLine(403, kERROR_403_TITLE);
        AddHeaders(strlen(kERROR_403_FORM));
        if (!AddContent(kERROR_403_FORM))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        AddStatusLine(200, kOK_200_TITLE);
        if (file_stat_.st_size != 0)
        {
            AddHeaders(file_stat_.st_size);
            m_iv_[0].iov_base = write_buffers_;
            m_iv_[0].iov_len = write_index_;
            m_iv_[1].iov_base = file_address_;
            m_iv_[1].iov_len = file_stat_.st_size;
            iv_count_ = 2;
            all_bytes_to_send_ = write_index_ + file_stat_.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            AddHeaders(strlen(ok_string));
            if (!AddContent(ok_string))
                return false;
        }
    }
    default:
        return false;
    }

    //只写http内容，没有发送文件
    m_iv_[0].iov_base = write_buffers_;
    m_iv_[0].iov_len = write_index_;
    iv_count_ = 1;
    all_bytes_to_send_ = write_index_;
    return true;
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
HttpConn::LineStatus HttpConn::parse_line()
{
    char temp;
    for (; checked_idx_ < read_index_; ++checked_idx_)
    {
        temp = read_buffers_[checked_idx_];
        if (temp == '\r')
        {
            if ((checked_idx_ + 1) == read_index_)
                return LINE_OPEN;
            else if (read_buffers_[checked_idx_ + 1] == '\n')
            {
                read_buffers_[checked_idx_++] = '\0';
                read_buffers_[checked_idx_++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (checked_idx_ > 1 && read_buffers_[checked_idx_ - 1] == '\r')
            {
                read_buffers_[checked_idx_ - 1] = '\0';
                read_buffers_[checked_idx_++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//把格式化输入的命令内容到缓存write_buffer里
bool HttpConn::AddResponse(const char *format, ...)
{
    if (write_index_ >= kWRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);

    //将格式化字符串写入，并返回写入字数
    int len = vsnprintf(write_buffers_ + write_index_, kWRITE_BUFFER_SIZE - 1 - write_index_, format, arg_list);

    if (len >= (kWRITE_BUFFER_SIZE - 1 - write_index_))
    {
        va_end(arg_list);
        return false;
    }
    write_index_ += len;
    va_end(arg_list);

    // LOG_INFO("request:%s", write_buffers_);

    return true;
}

// http内容编写函数
//解析请求行
HttpConn::HttpStatus HttpConn::ParseRequestLine(char *text)
{
    url_ = strpbrk(text, " \t");
    if (!url_)
    {
        return BAD_REQUEST;
    }
    *url_++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        method_ = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        method_ = POST;
        cgi_ = 1;
    }
    else
        return BAD_REQUEST;
    url_ += strspn(url_, " \t");
    version_ = strpbrk(url_, " \t");
    if (!version_)
        return BAD_REQUEST;
    *version_++ = '\0';
    version_ += strspn(version_, " \t");
    if (strcasecmp(version_, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(url_, "http://", 7) == 0)
    {
        url_ += 7;
        url_ = strchr(url_, '/');
    }

    if (strncasecmp(url_, "https://", 8) == 0)
    {
        url_ += 8;
        url_ = strchr(url_, '/');
    }

    if (!url_ || url_[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(url_) == 1)
        strcat(url_, "judge.html");
    check_state_ = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
HttpConn::HttpStatus HttpConn::ParseHeaders(char *text)
{
    if (text[0] == '\0')
    {
        if (content_length_ != 0)
        {
            check_state_ = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            socket_linger_opt_ = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        content_length_ = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        host_ = text;
    }
    else
    {
        // LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//解析http请求消息体
HttpConn::HttpStatus HttpConn::ParseContent(char *text)
{
    if (read_index_ >= (content_length_ + checked_idx_))
    {
        text[content_length_] = '\0';
        // POST请求中最后为输入的用户名和密码
        heard_string_ = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//验证cgi登录
//判断要写入哪些内容到http，并确定文件映射mmap地址
HttpConn::HttpStatus HttpConn::DoRequest()
{
    strcpy(real_file, root_path_);
    int len = strlen(root_path_);
    // printf("url_:%s\n", url_);
    const char *p = strrchr(url_, '/');

    //处理cgi
    if (cgi_ == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = url_[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, url_ + 2);
        strncpy(real_file + len, m_url_real, kFILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        // user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; heard_string_[i] != '&'; ++i)
            name[i - 5] = heard_string_[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; heard_string_[i] != '\0'; ++i, ++j)
            password[j] = heard_string_[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (cgi_users_.find(name) == cgi_users_.end())
            {
                sql_lock_.Lock();
                int res = mysql_query(mysql_, sql_insert);
                cgi_users_.insert(pair<string, string>(name, password));
                sql_lock_.Unlock();

                if (!res)
                    strcpy(url_, "/log.html");
                else
                    strcpy(url_, "/registerError.html");
            }
            else
                strcpy(url_, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (cgi_users_.find(name) != cgi_users_.end() && cgi_users_[name] == password)
                strcpy(url_, "/welcome.html");
            else
                strcpy(url_, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(real_file + len, url_, kFILENAME_LEN - len - 1);

    if (stat(real_file, &file_stat_) < 0)
        return NO_RESOURCE;

    if (!(file_stat_.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(file_stat_.st_mode))
        return BAD_REQUEST;

    int fd = open(real_file, O_RDONLY);
    file_address_ = (char *)mmap(0, file_stat_.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void HttpConn::Unmap()
{
    if (file_address_)
    {
        munmap(file_address_, file_stat_.st_size);
        file_address_ = 0;
    }
}

//状态行编写
bool HttpConn::AddStatusLine(int status, const char *title)
{
    return AddResponse("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//添加头
bool HttpConn::AddHeaders(int content_len)
{
    return AddContentLength(content_len) && AddLingerOpt() &&
           AddBlankLine();
}

//添加内容
bool HttpConn::AddContent(const char *content)
{
    return AddResponse("%s", content);
}

//添加内容长度
bool HttpConn::AddContentLength(int content_len)
{
    return AddResponse("Content-Length:%d\r\n", content_len);
}

//添加长连接linger选项
bool HttpConn::AddLingerOpt()
{
    return AddResponse("Connection:%s\r\n", (socket_linger_opt_ == true) ? "keep-alive" : "close");
}

//添加空白行
bool HttpConn::AddBlankLine()
{
    return AddResponse("%s", "\r\n");
}
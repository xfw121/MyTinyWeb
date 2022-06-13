#include "http_conn.h"
#include <iostream>

//静态变量
int HttpConn::client_counts_ = 0;                     //用户数量
int HttpConn::epoll_fd_ = -1;                         // epoll事件表fd
TriggerMode HttpConn::socket_trigger_mode_ = LT_MODE; //用户socket模式
char *HttpConn::root_path_ = NULL;                    //根目录路径
int HttpConn::log_switch_ = 0;                        //日志默认0关闭

//数据库相关
map<string, string> users_cache_; //用户登录信息缓存
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
    close_conn_flag_ = 0;  // http处理超时连接

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
    //若要发送的数据长度为0
    //表示响应报文为空，一般不会出现这种情况
    //如果不用回应，则继续监听该连接请求
    if (all_bytes_to_send_ == 0)
    {
        EpollModFd(epoll_fd_, sockfd_, EPOLLIN, socket_trigger_mode_);
        HttpClear(); //清空缓存，接收新的数据
        return true;
    }

    while (1)
    {
        //将响应报文发送给浏览器端
        send_num = writev(sockfd_, m_iv_, iv_count_);

        //如果是错误，要么是EAGAIN表示当前待发送缓冲区已经写满了，需要等待（可以是另外的进程来处理）
        if (send_num < 0)
        {
            if (errno == EAGAIN)
            {
                EpollModFd(epoll_fd_, sockfd_, EPOLLOUT, socket_trigger_mode_);
                return true;
            }
            //如果是发送错误则取消文件映射
            Unmap(); //取消文件映射
            return false; //返回false，取消用户连接
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
        //发送http写缓存部分
        else
        {
            m_iv_[0].iov_base = write_buffers_ + all_bytes_have_send_;
            m_iv_[0].iov_len = m_iv_[0].iov_len - all_bytes_have_send_;
        }
        //数据发完了，如果linger长连接，则继续保持连接，否则返回false断掉连接。
        if (all_bytes_to_send_ <= 0)
        {
            Unmap(); //取消文件映射
            EpollModFd(epoll_fd_, sockfd_, EPOLLIN, socket_trigger_mode_);

            if (socket_linger_opt_)
            {
                HttpClear();  //依然保持连接，但需要清空缓存，以便再接收新的数据
                return true;
            }
            else
            { 
                return false; //返回false，关闭连接
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

//先把已存储用户和密码预读取出来到uses_中
void HttpConn::MysqlResultInit(ConnectionPool *connPool)
{

    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    ConnectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        printf("数据库读取错误");
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users_cache_[temp1] = temp2;
    }

}

// http处理用户请求函数
void HttpConn::Process()
{
    //解析http报文，并返回http回应状态和返回文件的映射
    HttpStatus read_ret = ProcessRead();
    if (read_ret == NO_REQUEST)
    {
        //因为还没有读取完整http报文，继续监听
        EpollModFd(epoll_fd_, sockfd_, EPOLLIN, socket_trigger_mode_);
        return;
    }

    //根据回应状态，将http内容写入缓存，设置非连续缓存读写writev的参数m_iv_（包含返回文件映射）
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

    while ((check_state_ == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = ParseLine()) == LINE_OK))
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

    case INTERNAL_ERROR: //服务器内部错误
    {
        AddStatusLine(500, kERROR_500_TITLE);
        AddHeaders(strlen(kERROR_500_FORM));
        if (!AddContent(kERROR_500_FORM))
            return false;
        break;
    }
    case BAD_REQUEST://请求语法错误
    {
        AddStatusLine(404, kERROR_404_TITLE);
        AddHeaders(strlen(kERROR_404_FORM));
        if (!AddContent(kERROR_404_FORM))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST: //禁止访问
    {
        AddStatusLine(403, kERROR_403_TITLE);
        AddHeaders(strlen(kERROR_403_FORM));
        if (!AddContent(kERROR_403_FORM))
            return false;
        break;
    }
    case FILE_REQUEST: //文件请求
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
//每次checked_idx_都指向待处理的下一行首位
HttpConn::LineStatus HttpConn::ParseLine()
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
    //如果写入内容超出m_write_buf大小则报错
    if (write_index_ >= kWRITE_BUFFER_SIZE)
        return false;
    //定义可变参数列表     
    va_list arg_list;
    va_start(arg_list, format);  //将变量arg_list初始化为传入参数

    //将数据format从可变参数列表格式化写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(write_buffers_ + write_index_, kWRITE_BUFFER_SIZE - 1 - write_index_, format, arg_list);

    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (kWRITE_BUFFER_SIZE - 1 - write_index_))
    {
        va_end(arg_list);
        return false;
    }
    //更新m_write_idx位置
    write_index_ += len;
    //清空可变参列表
    va_end(arg_list);
    // LOG_INFO("request:%s", write_buffers_);
    return true;
}

// http内容编写函数
//解析请求行
HttpConn::HttpStatus HttpConn::ParseRequestLine(char *text)
{
    url_ = strpbrk(text, " \t"); //返回第一个空格或者\t位置
    if (!url_)
    {
        return BAD_REQUEST;
    }
    *url_++ = '\0';      //将该位置改为\0，用于将前面数据取出
    char *method = text; //取出方法
    if (strcasecmp(method, "GET") == 0)
        method_ = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        method_ = POST;
        cgi_ = 1;
    }
    else
        return BAD_REQUEST;
    // m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    url_ += strspn(url_, " \t");
    version_ = strpbrk(url_, " \t"); //返回第一个空格或者\t位置 （找url的末尾）
    if (!version_)
        return BAD_REQUEST;
    *version_++ = '\0';
    version_ += strspn(version_, " \t"); //取出版本
    if (strcasecmp(version_, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    //此时到了URI，跳过前面的协议内容
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

    if (!url_ || url_[0] != '/') // URI不应该为空，默认为/,其他也是以/开头
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(url_) == 1)
        strcat(url_, "judge.html"); //如果是首次访问网站，跳转到judge.html，此时url内容为"/judge.html"
    check_state_ = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
HttpConn::HttpStatus HttpConn::ParseHeaders(char *text)
{
    if (text[0] == '\0') //说明当前行为空行
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
        heard_string_ = text; //具体格式为user=xxx&password=xxx
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

    //登录注册检测
    if (cgi_ == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        // char flag = url_[1];

        // char *m_url_real = (char *)malloc(sizeof(char) * 200);
        // strcpy(m_url_real, "/");        //复制 "/"到m_url_real
        // strcat(m_url_real, url_ + 2);   //追加
        // strncpy(real_file + len, m_url_real, kFILENAME_LEN - len - 1);
        // free(m_url_real);
        // printf("url_:%s\n", url_);

        //将用户名和密码提取出来
        // 具体格式为user=xxx&password=xxx
        char name[100], password[100];
        int i;
        for (i = 5; heard_string_[i] != '&'; ++i)
            name[i - 5] = heard_string_[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; heard_string_[i] != '\0'; ++i, ++j)
            password[j] = heard_string_[i];
        password[j] = '\0';

        if (*(p + 1) == '3') //注册
        {
            //设置sql插入语句
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES("); //赋值语句
            strcat(sql_insert, "'");                                          //添加语句
            strcat(sql_insert, name);                                         //添加语句
            strcat(sql_insert, "', '");                                       //添加语句
            strcat(sql_insert, password);                                     //添加语句
            strcat(sql_insert, "')");                                         //添加语句

            //如果是注册，先检测数据库中是否有重名的，没有重名的，进行增加数据
            if (users_cache_.find(name) == users_cache_.end())
            {
                sql_lock_.Lock();
                int res = mysql_query(mysql_, sql_insert);
                if (!res)
                {
                    strcpy(url_, "/log.html");
                    users_cache_.insert(pair<string, string>(name, password));
                }
                else
                    strcpy(url_, "/registerError.html");
                sql_lock_.Unlock();
            }
            else
                strcpy(url_, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users_cache_.find(name) != users_cache_.end() && users_cache_[name] == password)
                strcpy(url_, "/welcome.html"); //返回登录成功界面
            else
                strcpy(url_, "/logError.html");
        }
    }

    if (*(p + 1) == '0') //返回登录界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1') //返回注册界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5') //返回图片（只有登录成功界面，才有响应表单）
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6') //返回视频
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7') //返回公众号界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(real_file + len, url_, kFILENAME_LEN - len - 1);

    if (stat(real_file, &file_stat_) < 0) //stat函数用于取得指定文件的文件属性，并将文件属性存储在结构体stat里
        return NO_RESOURCE;                

    if (!(file_stat_.st_mode & S_IROTH))//没有可读权限
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(file_stat_.st_mode))   //该路径为目录，语法错误
        return BAD_REQUEST;

    int fd = open(real_file, O_RDONLY);         //打开文件描述符
    file_address_ = (char *)mmap(0, file_stat_.st_size, PROT_READ, MAP_PRIVATE, fd, 0); //建立映射
    close(fd);            //注意关闭文件描述符
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
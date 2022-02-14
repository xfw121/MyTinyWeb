#include "http_conn.h"
#include <iostream>

int http_conn::client_counts_ = 0; //用户数量
int http_conn::client_epoll_fd_ = -1;    //epoll事件表fd

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

//初始化函数 _t表示临时变量
void http_conn::Init(int client_sockfd_t, const sockaddr_in &client_address_t, TriggerMode trigger_mode_t)
{
    //socket相关配置
    client_sockfd_= client_sockfd_t;
    client_address_  = client_address_t;
    http_socket_trigger_mode_ = trigger_mode_t;

    //开始监听已连接用户请求的事件 因为后面还要进行数据处理，可能需要花点时间，所以尽量早点监听
    //因为在主循环后才处理新的事件，所以可以提前开启监听
    //关于oneshot模式，除非重新设置该sockfd，否则只会触发一次，这样每次只在线程处理完后，再重新设置。
    EpollAddFd(client_epoll_fd_, client_sockfd_, false, http_socket_trigger_mode_);

    //Http初始化
    HttpInit();
}

//Http初始化
void http_conn::HttpInit()
{

    //读缓存
    memset(read_buffers_, '\0', kREAD_BUFFER_SIZE); //读数组
    read_index_ = 0;                               //读索引，记录已读数据数

    //写缓存
    memset(write_buffers_, '\0', kWRITE_BUFFER_SIZE); //写数组
    write_index_ = 0;                                //写索引，记录已写数据（不含文件）

    all_bytes_have_send_ = 0; //所有已写数据（包含文件）
    all_bytes_to_send_ = 0;   //所有待写数据（包含文件）

    //用于reactor模式下，提示主线程，已经完成http缓存的读取/写入
    improv_   = 0;

    //http读写状态
    http_state_ = read_state;
}

//读取内核接收缓存到http缓存处，
//此次不一定会读完整个http
//如果线程处理后发现不是完整http，那么会重新激发读。
bool http_conn::ReadOnce()
{
    //超过最大接收则发生错误
    if (read_index_ >= kREAD_BUFFER_SIZE)
    {
        return false;
    }

    //
    int read_num = 0;

    //LT模式读取数据
    if (LT_MODE == http_socket_trigger_mode_)
    {
        //更新
        read_num = recv(client_sockfd_, read_buffers_ + read_index_, kREAD_BUFFER_SIZE - read_index_, 0);
        read_index_ += read_num;

        //读取错误
        if (read_num <= 0)
        {
            return false;
        }

        return true;
    }
    //ET模式读数据
    else
    {
        while (true)
        {
            read_num = recv(client_sockfd_, read_buffers_ + read_index_, kREAD_BUFFER_SIZE - read_index_, 0);
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
bool http_conn::Write()
{
    //已发送数据
    int send_num = 0;

    //如果不用回应，则继续监听该连接请求
    if (all_bytes_to_send_ == 0)
    {
        EpollModFd(client_epoll_fd_, client_sockfd_, EPOLLIN, http_socket_trigger_mode_);
        HttpInit();
        return true;
    }

    while (1)
    {
        send_num = writev(client_sockfd_, m_iv_, m_iv_count_);

        //要么是错误，要么是EAGAIN表示当前待发送缓冲区已经写满了，需要等待（可以是另外的进程来处理）
        if (send_num < 0)
        {
            if (errno == EAGAIN)
            {
                EpollModFd(client_epoll_fd_, client_sockfd_, EPOLLOUT, http_socket_trigger_mode_);
                return true;
            }
            //unmap();取消文件映射
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
            //unmap();取消文件映射
            EpollModFd(client_epoll_fd_, client_sockfd_, EPOLLIN, http_socket_trigger_mode_);

            if (http_socket_linger_opt_)
            {
                HttpInit();
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
void http_conn::CloseConn(bool close_opt)
{
    if (close_opt && (client_sockfd_!= -1))
    {
        printf("close %d\n", client_sockfd_);
        EpollRemoveFd(client_epoll_fd_, client_sockfd_);
        client_sockfd_= -1;
        client_counts_--;
    }
}

//http处理用户请求函数
void http_conn::HttpProcess()
{

    //处理http请求，并将文件写入缓存，并返回http回应状态
    HttpCode read_ret = HttpRead();
    // if (read_ret == NO_REQUEST)
    // {
    //     modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
    //     return;
    // }

    //根据回应状态，将http内容写入缓存/请求文件映射mmap，确定发送数据地址和大小
    bool write_ret = HttpWrite(read_ret);
    if (!write_ret)
    {
        CloseConn(client_sockfd_);
        std::cout<<"写入错误\n";
    }
    EpollModFd(client_epoll_fd_, client_sockfd_, EPOLLOUT, http_socket_trigger_mode_);
}

//读取http内容，处理请求
http_conn::HttpCode http_conn::HttpRead()
{

    //判断要写入哪些内容到http，并确定文件映射mmap地址
    // return DoRequest();

    //测试返回404
    
    return BAD_REQUEST;
}

//根据回应状态，将http内容写入缓存/请求文件映射mmap，确定发送数据地址和大小
bool http_conn::HttpWrite(HttpCode ret)
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
        // AddStatusLine(200, kOK_200_TITLE);
        // if (m_file_stat.st_size != 0)
        // {
        //     AddHeaders(m_file_stat.st_size);
        //     m_iv_[0].iov_base = m_write_buf;
        //     m_iv_[0].iov_len = m_write_idx;
        //     m_iv_[1].iov_base = m_file_address;
        //     m_iv_[1].iov_len = m_file_stat.st_size;
        //     m_iv_count_ = 2;
        //     bytes_to_send = m_write_idx + m_file_stat.st_size;
        //     return true;
        // }
        // else
        // {
        //     const char *ok_string = "<html><body></body></html>";
        //     AddHeaders(strlen(ok_string));
        //     if (!AddContent(ok_string))
        //         return false;
        // }
    }
    default:
        return false;
    }

    //只写http内容，没有发送文件
    m_iv_[0].iov_base = write_buffers_;
    m_iv_[0].iov_len = write_index_;
    m_iv_count_ = 1;
    all_bytes_to_send_ = write_index_;

    return true;
}

//验证cgi登录
//判断要写入哪些内容到http，并确定文件映射mmap地址
http_conn::HttpCode http_conn::DoRequest()
{

    //判断要写入哪些文件到http，并确定文件映射mmap地址

    return FILE_REQUEST;
}

//把格式化输入的命令内容到缓存write_buffer里
bool http_conn::AddResponse(const char *format, ...)
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

//http内容编写函数

//状态行编写
bool http_conn::AddStatusLine(int status, const char *title)
{
    return AddResponse("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//添加头
bool http_conn::AddHeaders(int content_len)
{
    return AddContentLength(content_len) && AddLinger() &&
           AddBlankLine();
}

//添加内容
bool http_conn::AddContent(const char *content)
{
    return AddResponse("%s", content);
}

//添加内容长度
bool http_conn::AddContentLength(int content_len)
{
    return AddResponse("Content-Length:%d\r\n", content_len);
}

//添加长连接linger选项
bool http_conn::AddLinger()
{
    return AddResponse("Connection:%s\r\n", (http_socket_linger_opt_ == true) ? "keep-alive" : "close");
}

//添加空白行
bool http_conn::AddBlankLine()
{
    return AddResponse("%s", "\r\n");
}
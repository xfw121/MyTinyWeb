# 1 MyTynyWeb简介
## 技术栈 

MyTynyWeb是Linux下C++轻量级Web服务器，实现技术栈如下：

+ **并发模型**

  - 使用 **半同步并发模式（主线程监听、线程池处理业务逻辑） + 非阻塞socket + epoll监听事件(ET和LT均实现) + 事件处理模式(Reactor和模拟Proactor均实现)** 的并发模型

+ **http报文服务**

  使用**状态机**解析HTTP请求报文，支持解析**GET和POST**请求

+ **数据库**

  访问服务器数据库实现web端用户**注册、登录**功能，可以请求服务器**图片和视频文件**

+ **服务器优化：定时器**

  处理非活动连接

+ **压力测试**

  经Webbench压力测试可以实现**上万的并发连接**数据交换

## 总体框架图

![image](https://img2022.cnblogs.com/blog/2197914/202206/2197914-20220620154316448-722170853.png)


## 服务器概述

### 基本定义

一个Web Server就是一个服务器软件（程序），或者是运行这个服务器软件的硬件（计算机）。其主要功能是通过HTTP协议与客户端（通常是浏览器（Browser））进行通信，来接收，存储，处理来自客户端的HTTP请求，并对其请求做出HTTP响应，返回给客户端其请求的内容（文件、网页等）或返回一个Error信息。

### 客户端访问浏览器过程

1. 向浏览器输入网址
2. DNS解析过程（将URL转化为IP地址）
   + 缓存检查，浏览器缓存->操作系统缓存->磁盘hosts 文件
   + 就会查询路由缓存，路由缓存不存在就去查找本地DNS服务器
   + 如果本地DNS服务器还没找到就会向根服务器发出请求。
   + 再找域服务器。
   + 服务器响应完客户端请求之后，四次挥手解除TCP连接建立TCP连接。

3. 浏览器依据HTTP协议生成针对目标Web服务器的HTTP请求报文，通过TCP、IP等协议发送到目标Web服务器上。
4. 服务器处理请求并返回HTTP报文。
5. 浏览器解析渲染页面。
6. 服务器响应完客户端请求之后，四次挥手解除TCP连接。

### 服务器端主要处理过程

Web服务器端主要需要完成两个功能，**1**通过socket监听来自用户的连接请求和请求报文，**2**处理相应报文，并返回必要报文给用户。

+ 功能1

  + 当有多个用户和连接请求，需要服务器通过**epoll这种I/O复用技术**（还有select和poll）来监听，对和服务器ip地址绑定的soket进行监听来接收新的用户连接请求，对和用户ip地址绑定的已连接socket监听来处理读取和发送事件（对应对用户到达数据缓存的读取和发送响应用户的缓存数据）。

  + 事件监听模式模式可划分为**ET**和**LT**触发模式，

    + 当处于**LT模式**，epoll_wait函数复制内核事件表已发送的事件状态到用户events结构体,用户层根据events结构体调用相应内核读写接口进行处理，内核如果检测到事件被处理完，会自动把对应事件状态清空（如用户调用读接口把对应读事件的文件描述符的缓存全部读完，那么内核会自动把该读事件状态清空，否则一直保持读事件状态）。
    + 当处于**ET模式**，epoll_wait函数复制内核事件表已发送的事件状态到用户events结构体，并会清除内核事件状态。所以用户如果已经用epoll_wait函数读取而没有处理，那么内核事件表清空，而不会再通知。

  + ET模式下的非阻塞IO读写注意事项。

    + 读

      ET模式是当读缓存区开始有数据才触发事件，所以每次要把读缓存数据都读完。在读取过程中，可能到达新数据，所以应该反复读取，直到缓存区没有数据即（errno == **EAGAIN** || errno == **EWOULDBLOCK**）状态，这两个状态一致，下次到达数据ET事件才可能触发。

    + 写

      ET模式是当写缓存区开始可写时才触发事件，按理说应该把响应报文都写入内核缓存区，循环写入，直到缓存区满了，那么就不再这里继续等待浪费时间，直接注册该写事件，交给另外的工作线程来继续完成。

  + oneshot模式

    用户连接的读写事件可能被触发多次，所以消息队列可能存在多个请求（哪怕是ET模式，到达用户数据发送变化也会触发），那么就可能发送同一个连接被多个线程读写，数据就乱连。因此 这里关于用户连接的读写事件注册，都是使用oneshot模式，即事件注册之后，只会触发一次事件，要重新注册，才会再次触发。

  + 按照读写事件处理的单位的不同可划分为**Reactor**模式和**Proactor**模式。Reactor模式是工作线程同步处理完成读写事件，Proactor模式是工作线程异步处理事件，交给主线程/内核完成。

+ 功能2

  - 为了保证每个用户的体验，需要通过**并发来处理**用户的请求。该项目使用**线程池（半同步半反应堆模式）**并发处理用户请求，主线程负责监听（**Proactor**模式下需要负责读写），工作线程（线程池中的线程）负责处理逻辑（HTTP请求报文的解析等等）（**Reactor**模式下要负责读写）。

+ 主要流程

  + 主线程监听绑定网卡ip地址的**listenfd**到达的新用户连接，处理新的socket文件描述符**connfd**（建立连接的用户以后都是与这个soket通信），并注册到内核事件表中，监听已建立连接用户的到达报文的读请求和工作线程处理后待发送响应报文的写请求。

  + 主线程监听到已连接socket的读请求，说明该用户有请求报文到达，将任务对象（程序中该socket对应的http对象的指针）插入线程池的请求队列中，并更改该对象的读写状态以告知待会要处理的工作线程。

  + 线程池中的工作线程依靠锁机制以及信号量机制来实现线程同步，保证操作的原子性。

    + **Reactor**模式
      + 读状态，先调用系统接口读取用户到达缓存到对应http对象，然后进行process处理（包括对请求报文的处理、返回报文的处理和注册写入）
      + 写状态，将对应socket的http报文对象写入内核缓存区。

    + **Proactor**模式
      + 仅有读状态（主线程仅会在完成读取到达的用户内核缓存到http对象后，再插入对象到请求队列），工作线程直接进行process处理。

# 2 并发模型

## 线程同步封装类

当多线程对消息队列获取待处理http对象任务时，为了防止多个线程对消息队列中同一对象进行操作以及对消息队列发生竞态更改，采用信号量和锁来进行对消息队列这一公共资源的保护，锁来保障对消息队列当前的操作权限，信号量通知当前队列任务数量。

+ 为什么要用RAII机制？

  其实锁也是占用资源，利用RAII机制（在构造函数中申请分配资源，在析构函数中释放资源），实现资源和状态的安全管理。

## 线程池实现

线程池的设计模式为半同步/半反应堆，Reactor和Proactor事件处理模式都已实现。

### 线程池事件处理模式

+ Reactor模式

  ![image-20220530151632733](https://img2022.cnblogs.com/blog/2197914/202206/2197914-20220606112002085-939102397.png)

  

+ Proactor模式

  ![image-20220530151659685](https://img2022.cnblogs.com/blog/2197914/202206/2197914-20220606112002492-1838168844.png)

​			项目采用同步io来模拟Proactor模式，与Reactor模式区别，主线程监听到已连接的读写事件后，先完成读写，只会把读请求对应的http对象指针放入到队列中，然后工作线程竞争获取，直接进行http报文逻辑处理，并将响应报文写入http对象写缓存，注册写事件，让主线程来完成。

### 线程池类

```c++
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
```

+ 主要成员变量

  + 线程池描述符数组，用来存储线程描述符id（在线程创建时被赋值）
  + 请求队列链表，存储待处理的http对象指针。
  + 请求队列的信号量和互斥锁。
  + 事件处理模式状态

+ 主要成员函数

  + 线程池初始化

    + 对消息队列和线程池的参数进行检查
    + 创建线程描述符数组，并检查是否成功创建
    + 创建线程，并设置子线程运行结束后，自动回收。

  + 线程池类析构函数，析构线程描述符

  + 消息队列添加函数

    通过list容器创建请求队列，向队列中添加时，通过**互斥锁**保证线程安全，添加完成后通过**信号量**提醒有任务要处理，最后注意线程同步。

  + 线程工作函数Worker

    这里之所以要加上Worker这个静态函数，主要是为了满足线程创建pthread_create函数对输入参数的要求，因为成员函数隐式传入的第一个参数为对象this指针。

  + 线程实际运行内容函数 Run

    + 通过信号量判断消息队列是否有消息，没有则阻塞

    + 加锁

    + 取出消息（先要判断是否为空，有可能先被其他线程处理掉了）

    + 解锁

    + 进行处理

      + **Reactor**模式
        + 读状态，先调用系统接口读取用户到达缓存到对应http对象，然后进行process处理（包括对请求报文的处理、返回报文的处理和注册写事件）
        + 写状态，将对应socket的http报文对象写入内核缓存区。

        + **Proactor**模式
          + 仅有读状态（主线程仅会在完成读取到达的用户内核缓存到http对象后，再插入对象到请求队列），工作线程直接进行process处理。

# 3 http报文处理

## http处理流程

### 总体流程

+ 浏览器端发出http连接请求，将接收到的连接数据读入对应http对象读buffer，并将插入该**对象指针**插入任务队列，工作线程从任务队列中取出一个任务进行处理
+ 工作线程取出任务后，调用**process_read函数**，通过**状态机**对请求报文进行解析，完整报文解析后跳转到do_request函数，确定响应报文状态和返回文件的映射。
+ 通过process_write写入http对象缓存buffer，随后发送http缓存报文给返回给浏览器端。

### http解析框图

![image-20220601091335394](https://img2022.cnblogs.com/blog/2197914/202206/2197914-20220606112002621-98157758.png)

+ **HTTP_CODE含义**

  表示HTTP请求的处理结果，在头文件中初始化了八种情形，在报文解析与响应中只用到了七种。

  - NO_REQUEST

  - - 请求不完整，需要继续读取请求报文数据
    - 跳转主线程继续监测读事件

  - GET_REQUEST

  - - 获得了完整的HTTP请求
    - 调用do_request完成请求资源映射

  - NO_RESOURCE

  - - 请求资源不存在
    - 跳转process_write完成响应报文

  - BAD_REQUEST

  - - HTTP请求报文有语法错误或请求资源为目录
    - 跳转process_write完成响应报文

  - FORBIDDEN_REQUEST

  - - 请求资源禁止访问，没有读取权限
    - 跳转process_write完成响应报文

  - FILE_REQUEST

  - - 请求资源可以正常访问
    - 跳转process_write完成响应报文

  - INTERNAL_ERROR

  - - 服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发

### 主要逻辑代码

+ 线程池从消息队列中取出http对象，将对应接收数据读入对应buffer（如果是Proactor模式，那么主线程已经读取好了），另外要从数据库连接池获取一个连接到mysql，通过调用Process函数来完成报文的处理。

```c++
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
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    //对请求处理
                    request->Process();
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
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->Process();
        }
    }
}
```

+ 各子线程通过process函数对任务进行处理，process函数调用process_read函数和process_write函数分别完成报文解析与报文响应两个任务，如果ProcessRead返回状态为NO_REQUEST，说明还没有接收到完整报文，继续监听。

```c++
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
```

## 注册登录逻辑处理（DoRequest函数）

### 流程图

![image-20220601153356766](https://img2022.cnblogs.com/blog/2197914/202206/2197914-20220606112003102-948321713.png)

### 主要代码

+ 从消息体提取用户名和密码

  ```c++
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
  ```

+ 注册

  + 先在用户信息缓存cgi_uses_(数据结构为map)查找是否有重复名
  + 然后数据库上锁
  + 调用插入sql语句
  + 同时插入cgi_uses_中
  + 解锁,设置返回url

```c++
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
            if (cgi_users_.find(name) == cgi_users_.end())
            {
                sql_lock_.Lock();
                int res = mysql_query(mysql_, sql_insert);
                if (!res)
                {
                    strcpy(url_, "/log.html");
                    cgi_users_.insert(pair<string, string>(name, password));
                }
                else
                    strcpy(url_, "/registerError.html");
                sql_lock_.Unlock();
            }
            else
                strcpy(url_, "/registerError.html");
        }
```

+ 登录
  + 进行密码验证
  + 然后设置返回url_

```c++
        else if (*(p + 1) == '2')
        {
            if (cgi_users_.find(name) != cgi_users_.end() && cgi_users_[name] == password)
                strcpy(url_, "/welcome.html"); //返回登录成功界面
            else
                strcpy(url_, "/logError.html");
        }
```

+ 完成文件映射和确定响应报文状态

  ```c++
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
  ```

## 响应报文生成（ProcessWrite）

### AddResponse函数（格式化写入http缓存）

根据`do_request`的返回状态，服务器子线程调用`process_write`向`m_write_buf`中写入响应报文。

- add_status_line函数，添加状态行：http/1.1 状态码 状态消息

- add_headers函数添加消息报头，内部调用add_content_length和add_linger函数

- - content-length记录响应报文长度，用于浏览器端判断服务器是否发送完数据
  - connection记录连接状态，用于告诉浏览器端保持长连接

- add_blank_line添加空行

- 上述涉及的5个函数，均是内部调用`**AddResponse**`函数格式化写入缓存write_buffers_，并更新write_index_指针，实现如下。

  ```c++
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
  ```

  

### 主要逻辑代码

+ 其他响应状态，添加响应报文到写缓存
+ 文件申请状态FILE_REQUEST，新添加非连续缓存读写结构体m_iv_的更新

```c++
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
```

## 响应报文发送（Write）

服务器子线程调用`ProcessWrite`完成响应报文写入http缓存，随后注册`epollout`事件。服务器主线程检测写事件，并调用`http_conn::Write`函数将响应报文发送给浏览器端。该函数具体逻辑如下：

在生成响应报文时初始化byte_to_send，包括头部信息和文件数据大小。通过writev函数循环发送响应报文数据，根据返回值更新byte_have_send和iovec结构体的指针和长度，并判断响应报文整体是否发送成功。

- 若writev单次发送成功，更新byte_to_send和byte_have_send的大小，若响应报文整体发送成功,则取消mmap映射,并判断是否是长连接.

- - 长连接重置http类实例，注册读事件，不关闭连接，
  - 短连接直接关闭连接

- 若writev单次发送不成功，判断是否是写缓冲区满了。

- - 若不是因为缓冲区满了而失败，取消mmap映射，关闭连接

  - 若eagain则满了，更新iovec结构体的指针和长度，并注册写事件，等待下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout），因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性。

    ```c++
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
                    return false;  //返回false，关闭连接
                }
            }
        }
    }
    ```


# 4 数据库连接池

**为什么要创建连接池？**

若系统需要频繁访问数据库，则需要频繁创建和断开数据库连接，而创建数据库连接是一个很耗时的操作，也容易对数据库造成安全隐患。

**整体概括**

+ 主线程的服务器初始化中，对数据库连接池进行初始化。
+ 工作线程在进行报文处理前，从连接池中获取一个数据库连接。
+ 处理后，释放数据库连接到连接池。

## 主线程完成连接池初始化

### 线程池初始化

+ 单例模式创建连接池，保证了连接池类对象有且仅有一个。
+ 数据库连接池初始化
+ 读取数据库中所有用户信息，进行存储在users_cache_中，以便查询

### 单例模式具体实现

```c++
ConnectionPool *ConnectionPool::GetInstance()
{
	static ConnectionPool connPool;
	return &connPool;
}
```

### 连接池初始化

```c++
//构造初始化
//输入url为”localhost“链接的是本地的计算机
//User为登陆数据库用户名
//PassWord为登录数据库密码
//DBName为数据库名
//Port为数据库的TCP监听端口（一般默认为3306）
//MaxConn为最大连接数
void ConnectionPool::Init(string url, string User, string PassWord, string DBName, int Port, int MaxConn)
{
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con);
		if (con == NULL)
		{
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);
		if (con == NULL)
		{
			exit(1);
		}
		connList.push_back(con);
	}
	reserve = Sem(MaxConn);  //reserve为数据库连接池信号量，表明存在连接数
}
```

## RAII机制获取和释放数据库连接

### 工作线程获取连接（以proactor模式为例） 

```c++
        // PROACTOR模式
        else if (PROACTOR_MODEL == concurrency_model_)
        {
            //从数据库连接池获取一个连接到mysql返回给request->mysql_
            ConnectionRAII mysqlcon(&request->mysql_, sql_conn_pool_);
            request->Process();
            //在退出函数体后，mysqlcon执行析构，来释放连接到连接池
        }
```

### RAII机制具体实现

+ RAII机制获取连接类

```c++
//RAII机制获取连接类
class ConnectionRAII{

public:
	ConnectionRAII(MYSQL **con, ConnectionPool *connPool);
	~ConnectionRAII();
	
private:
	MYSQL *conRAII;           	//要释放的数据库连接
	ConnectionPool *poolRAII;	//要释放的数据库连接所在的连接池
};
```

+ 构造函数和析构函数

  ```c++
  ConnectionRAII::ConnectionRAII(MYSQL **SQL, ConnectionPool *connPool){
  	*SQL = connPool->GetConnection();  //获取一个连接返回给*SQL
  	
  	conRAII = *SQL;
  	poolRAII = connPool;
  }
  
  ConnectionRAII::~ConnectionRAII(){
  	poolRAII->ReleaseConnection(conRAII);
  }
  ```

## 连接池代码实现

```c++
//数据库连接池
class ConnectionPool
{
public:
	MYSQL *GetConnection();				 //获取数据库连接
	bool ReleaseConnection(MYSQL *conn); //释放连接
	void DestroyPool();					 //销毁所有连接
	//单例模式（连接池对象作为函数的静态局部变量）
	static ConnectionPool *GetInstance();
	//对线程池的初始化
	void Init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn); 
private:
	ConnectionPool();
	~ConnectionPool();
	Locker lock;    //保障对连接池的修改
	list<MYSQL *> connList; //连接池
	Sem reserve;            //指示连接池可用连接数量
};
```

# 5定时器处理非活跃连接

+ **参考资料**

+ 为什么要处理非活跃连接？

  客户端（这里是浏览器）与服务器端建立连接后，长时间不交换数据，一直占用服务器端的文件描述符，导致连接资源的浪费，可以定时来处理非活跃连接。

## 定时器处理逻辑

具体的，利用`alarm`函数周期性地触发`SIGALRM`信号，信号处理函数利用管道通知主循环，主循环接收到该信号后对升序链表上所有定时器进行处理，若该段时间内没有交换数据，则将该连接关闭，释放所占用的资源。

### 定时器初始化

+ 初始化管道

  + 使用socketpair函数能够创建一对套接字进行通信

+ 初始化定时器链表（主要是完成参数传递）

+ 添加信号，开启定时

  ```c++
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
      Addsig(SIGPIPE, SIG_IGN);//忽略SIGCHLD信号，这常用于并发服务器的性能的一个技巧，因为并发服务器常常fork很多子进程，
                              //子进程终结之后需要服务器进程去wait清理资源。如果将此信号的处理方式设为忽略，
                              //可让内核把僵尸子进程转交给init进程去处理，省去了大量僵尸进程占用系统资源
      Addsig(SIGALRM, timer_list_.SigHandler, false); //定时信号
      Addsig(SIGTERM, timer_list_.SigHandler, false); //进程终止信号
      alarm(kTIMESLOT); //开启定时
  }
  ```

### 主线程逻辑（仅包含定时器部分代码）

+ 主线程监听来自管道的读事件，通过DealWithSigna函数，判断是SIGALRM信号还是SIGTERM信号（终止信号ctrl+c），并对应设置clock_flag和stop_server_flag。
+ 如果stop_server_flag=1，停止服务器运转
+ 如果clock_flag=1，调用定时器链表timer_list_的定时事件处理函数Tick进行处理。

```c++
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
        for (int i = 0; i < epoll_N; i++)
        {
            int sockfd = events_[i].data.fd; //获取事件sockfd
            //处理定时器发送的信号
            if ((sockfd == pipefd_[0]) && (events_[i].events & EPOLLIN))
            {
                bool flag = DealWithSigna(clock_flag, stop_server_flag);
                if (false == flag)
                    cout<<"管道接收失败"<<endl;
            }
        }
        //处理超时的不活跃连接
        if (clock_flag)
        {
            timer_list_.Tick();   //定时事件处理
            cout << "\n时钟信号到了\n";
            clock_flag = false;
        }
    }
}
```

+ 其中Tick函数主要包含两个功能

  + 去除超时定时器，调用绑定的定时事件函数（释放资源）
  + 再次启动定时

  ```c++
  //去除超时定时器，及调用绑定的定时事件函数（释放资源），并再次启动定时
  void SortTimerList::Tick()
  {
      if (!head)
      {
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
  ```

+ 另外定时信号处理函数实现如下（通过管道发送信号值给主线程）

  ```c++
  //定时信号中断函数
  void SortTimerList::SigHandler(int sig)
  {
      //为保证函数的可重入性，保留原来的errno
      int save_errno = errno;
      int msg = sig;
      send(pipefd_[1], (char *)&msg, 1, 0); //将信号值通过管道发给主线程
      errno = save_errno;
  }
  ```

## 基于升序链表的定时器设计

将定时事件类对象与连接资源类对象互相绑定，并将定时器对象以升序链表结构进行封装为升序定时器链表。

+ 定时器类

  主要包含了定时器过期时间，定时器事件函数指针，用户连接资源指针（用来与资源结构体互相绑定），以及链表结构

  ```c++
  //定时器结构体 （与资源结构体互相绑定）
  class Timer
  {
  public:
      Timer() : prev(NULL), next(NULL) {}
  
  public:
      time_t expire;                   //过期时间
      
      void (* cb_func)(ClientResource *); //这是定时事件函数指针
      ClientResource *user_data;    //加入资源指针，互相绑定
      Timer *prev;
      Timer *next;
  };
  ```

+ 资源结构体类

  主要包括就是，sockfd资源，以及通过定时器指针来与对应定时器互相绑定

  ```c++
  //资源结构体 （与定时器互相绑定）
  struct ClientResource
  {
      sockaddr_in address;  //网络ip地址
      int sockfd;           //文件描述符
      Timer *timer;         //加入定时器指针，互相绑定
  };
  ```

+ 升序定时器链表

  + 通过AddTimer，AdjustTimer，RemoveTimer来维持定时器链表
  + 利用`alarm`函数周期性地触发`SIGALRM`信号，信号中断函数SigHandler()利用管道通知主循环，主循环通过Tick()函数来完成处理超时定时器以及绑定的用户资源。

  ```c++
  //升序链表
  class SortTimerList
  {
  public:
      SortTimerList();
      ~SortTimerList();
      void Init( int epoll_fd_t,int *pipefd_t,int timeslot_t); //对时隙变量进行初始化
      void AddTimer(Timer *timer);        //添加定时器
      void AdjustTimer(Timer *timer);     //调整定时器
      void RemoveTimer(Timer *timer);     //去除定时器
      //信号中断函数 （必须定义静态成员函数，不然多了this参数，不能使用信号设置函数）（向管道发送数据）
      static void SigHandler(int sig);
      //定时事件处理函数
      void Tick();                       //去除超时定时器，及调用绑定的定时事件函数（释放资源），并再次启动定时
      static int *pipefd_;       //与主线程通信管道     （初始化） 
      static int epollfd_;       //事件表epollfd         （初始化）
      int TIMESLOT;            //时隙                 （初始化）
  private:
      void AddTimer(Timer *timer, Timer *lst_head);
      Timer *head;
      Timer *tail;
  };
  ```

  + 释放资源主要包括监听事件的删除和sockfd的关闭

    ```c++
    //释放资源函数
    void ReleseResource(ClientResource *user_data)  //释放用户资源
    {
        epoll_ctl(SortTimerList::epollfd_, EPOLL_CTL_DEL, user_data->sockfd, 0);   //监听事件的删除
        assert(user_data);  
        close(user_data->sockfd);                                                 //sockfd的关闭
        HttpConn::client_counts_--;
    }
    ```

## 断开连接处理（主线程）

除了定时处理非活跃连接，主线程还会监听本端断开连接，对端断开连接以及连接错误事件，来处理断开连接资源。

```c++
            //服务器端关闭连接，移除对应的定时器
            //EPOLLRDHUP对端关闭时
            //EPOLLHUP 本端描述符产生一个挂断事件，默认监测事件
            //EPOLLERR 描述符产生错误时触发，默认检测事件
            else if (events_[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //移除对应的定时器
                Timer *timer = client_resource_[sockfd].timer;
                DelTimer(timer, sockfd);
                cout << "\n断开连接事件\n";
            }
```
# 6并发测试

回环网络

+ reactor模式

  qps=277272/60=4621 （个/s）

  ![image-20220606193631063](https://img2022.cnblogs.com/blog/2197914/202206/2197914-20220606195944470-2024072661.png)

+ proactor模式

  qps=574380/60=9573 (个/s)

  ![image-20220606193814390](https://img2022.cnblogs.com/blog/2197914/202206/2197914-20220606195944661-1582949714.png)

非回环网络

测试ping

![image-20220606194513322](https://img2022.cnblogs.com/blog/2197914/202206/2197914-20220606195944709-633434049.png)

+ reactor模式

  qps=27276/60=454 （个/s）

  ![image-20220606194243967](https://img2022.cnblogs.com/blog/2197914/202206/2197914-20220606195944936-1227933763.png)

+ proactor模式

  qps=38340/60=639 （个/s）

  ![image-20220606195642443](https://img2022.cnblogs.com/blog/2197914/202206/2197914-20220606195944710-784089004.png)

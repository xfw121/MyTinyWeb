#include "webserver.h"

int main(int argc, char **argv)
{

    //需要修改的数据库信息,登录名,密码,库名

    //创建服务器
    WebServer webserver;

    //Web参数配置
    webserver.ParameterSet(argc, argv);

    //优先把Web本地功能先初始化
    //日志（都要用）
    //数据库
    //线程池初始化
    webserver.ThreadPoolInit();
    
    //初始化网络连接相关功能
    webserver.SocketInit();    //监听socket初始化,并开始监听
    webserver.ListTimerInit(); //定时器初始化，并启动定时器

    //主线程（将监听socketfd加入epoll监听事件表，并开始循环处理事件）
    webserver.EventLoop();

    return 0;
}
#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

ConnectionPool::ConnectionPool()
{
	
}

//单例模式实现连接池对象只有一个
ConnectionPool *ConnectionPool::GetInstance()
{
	static ConnectionPool connPool;  
	return &connPool;
}

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


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *ConnectionPool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.Wait();  //等待有可用连接
	
	lock.Lock();

	con = connList.front();
	connList.pop_front();

	lock.Unlock();
	return con;
}

//释放当前使用的连接
bool ConnectionPool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.Lock();

	connList.push_back(con);

	lock.Unlock();

	reserve.Post();
	return true;
}

//销毁数据库连接池
void ConnectionPool::DestroyPool()
{

	lock.Lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		connList.clear();
	}

	lock.Unlock();
}

ConnectionPool::~ConnectionPool()
{
	DestroyPool();
}

ConnectionRAII::ConnectionRAII(MYSQL **SQL, ConnectionPool *connPool){
	*SQL = connPool->GetConnection();  //获取一个连接返回给*SQL
	
	conRAII = *SQL;
	poolRAII = connPool;
}

ConnectionRAII::~ConnectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}
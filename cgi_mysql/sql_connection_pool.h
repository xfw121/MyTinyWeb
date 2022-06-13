#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"

using namespace std;

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

//RAII机制获取连接类
class ConnectionRAII{

public:
	ConnectionRAII(MYSQL **con, ConnectionPool *connPool);
	~ConnectionRAII();
	
private:
	MYSQL *conRAII;           	//要释放的数据库连接
	ConnectionPool *poolRAII;	//要释放的数据库连接所在的连接池
};

#endif

CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g -std=c++11
else
    CXXFLAGS += -O2

endif

server: main.cpp  webserver.cpp 	\
		./epoll/epoll_function.cpp 	\
		./cgi_mysql/sql_connection_pool.cpp \
		./http/http_conn.cpp				\
		./lock/locker.h		\
		./threadpool/threadpool.h
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient
	
clean:
	rm  -r server

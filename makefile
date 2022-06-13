CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g -std=c++11
else
    CXXFLAGS += -O2

endif

server: main.cpp  webserver.cpp 	\
		./util/util.cpp 	\
		./cgi_mysql/sql_connection_pool.cpp \
		./http/http_conn.cpp				\
		./lock/locker.h		\
		./threadpool/threadpool.h \
		./timer/list_timer.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient
	
clean:
	rm  -r server

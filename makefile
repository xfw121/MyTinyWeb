CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g -std=c++11
else
    CXXFLAGS += -O2

endif

server: main.cpp  webserver.cpp 	\
		./epoll/epoll_function.cpp 	\
		./http/http_conn.cpp		
	$(CXX) -o server  $^ $(CXXFLAGS) 
	
clean:
	rm  -r server

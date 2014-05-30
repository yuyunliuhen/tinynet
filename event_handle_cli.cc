
#include <stdio.h>
#include <stdlib.h>
//	for c++	11
#include <thread>
#include <functional>

#ifndef __LINUX
#include <WinSock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <strings.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h> 		//	gethostname
#include <netdb.h>			//	gethostbyname
#endif //__LINUX_

#include "event_handle_cli.h"
#include "reactor_impl_select.h"
#include "reactor.h"

Event_Handle_Cli::Event_Handle_Cli(Reactor* __reactor,const char* __host,unsigned int __port) : Event_Handle(__reactor),host_(__host),port_(__port)
{
	_init();
	reactor()->reactor_impl()->register_handle(this,get_handle(),kMaskConnect);
};

int Event_Handle_Cli::handle_input(int __fd)
{
	if(1)
	{
			//	just transform data
			char __buf[64*1024] = {0};
			int __recv_size = recv(__fd,__buf,64*1024,0);
			if(-1 != __recv_size)
			{
				if(0 == __recv_size)
				{
					reactor()->reactor_impl()->handle_close(__fd);
				}
				on_read(__fd,__buf,__recv_size);
			}		
	}
	else
	{
		//	read head first
		unsigned long __usable_size = 0;
		int __length = 0;
		int __recv_size = 0;
		_get_usable(__fd,__usable_size);
		if(__usable_size >= sizeof(int))
		{
			__recv_size = recv(__fd,(char*)&__length,4,0);
			if(sizeof(int) != __recv_size)
			{
				printf("error: __recv_size = %d",__recv_size);  
				return 0;
			}
		}
		_get_usable(__fd,__usable_size);

		if(__usable_size >= __length)
		{
			char __buf[8192] = {0};
			int __recv_size = recv(__fd,__buf,__length,0);
			if(0 == __recv_size)
			{
				return 0;
			}
			else if (-1 == __recv_size)
			{
				return -1;
			}
			on_read(__fd,__buf,__recv_size);
		}
	}

	return -1;
}

int Event_Handle_Cli::handle_output(int __fd)
{
	printf("handle_outputd\n");
#if 0
	static int __data = 0;
	++__data;
	int __send_size = send(__fd,(char*)&__data,sizeof(int),0);
	if( 0 == __send_size )
	{
		perror("error at send");  
		return -1;
	}
#endif
	reactor()->reactor_impl()->register_handle(this,__fd,kMaskRead);
	return -1;
}

int Event_Handle_Cli::handle_exception(int __fd)
{
	printf("handle_exception\n");
	return -1;
}

int Event_Handle_Cli::handle_close(int __fd)
{
	printf("handle_close\n");
	return -1;
}

int Event_Handle_Cli::handle_timeout(int __fd)
{
	printf("handle_timeout\n");
	return -1;
}

void Event_Handle_Cli::_init(unsigned int __port)
{
#ifndef __LINUX
	WORD __version_requested = MAKEWORD(2,2);
	WSADATA __data;
	if (0 != WSAStartup( __version_requested, &__data))
	{
		//Tell the user that we could not find a usable WinSock DLL.
		return;
	}
	if ( LOBYTE( __data.wVersion ) != 2 ||
		HIBYTE( __data.wVersion ) != 2 )
	{
		// Tell the user that we could not find a usable WinSock DLL.
		WSACleanup();
		return;
	}
#endif //__LINUX
	fd_ = socket(AF_INET,SOCK_STREAM,0); 
	if ( -1 == fd_ )
	{
		perror("error at socket");
		exit(1);
	}
	struct sockaddr_in __clientaddr;  
	memset(&__clientaddr,0,sizeof(sockaddr_in));  
	__clientaddr.sin_family = AF_INET;  
	__clientaddr.sin_port = htons(port_);  
	__clientaddr.sin_addr.s_addr = inet_addr(host_.c_str());
	int __res = connect(fd_,(sockaddr*)&__clientaddr,sizeof(sockaddr_in));
	if(-1 == __res)
	{
		perror("error at connect");
#ifndef __LINUX
		DWORD __last_error = ::GetLastError();
#endif // __LINUX
		exit(1);
	}
}

void Event_Handle_Cli::_set_noblock(int __fd)
{
#ifndef __LINUX
	unsigned long __non_block = 1;
	if (SOCKET_ERROR == ioctlsocket(__fd, FIONBIO, &__non_block))
	{
		printf("_set_noblock() error at ioctlsocket,error code = %d\n", WSAGetLastError());
	}
#else
	int __opts = fcntl(__fd,F_GETFL);  
	if(0 > __opts)  
    	{  
      		perror("error at fcntl(sock,F_GETFL)");  
       		exit(1);  
    	}  
	 __opts = __opts | O_NONBLOCK;  
	if( 0 > fcntl(__fd,F_SETFL,__opts) )  
	{  
       		perror("error at fcntl(sock,F_SETFL)");  
       		exit(1);  
   	}  
#endif //__LINUX
}

void Event_Handle_Cli::write( const char* __data,unsigned int __length )
{
	int __send_bytes = send(fd_,__data,__length,0);
	if(-1 == __send_bytes)
	{
#ifndef __LINUX
		DWORD __last_error = ::GetLastError();
		if(WSAEWOULDBLOCK  == __last_error)
		{
			//	disconnect from server
			handle_close(fd_);
			return;
		}
#else
		//error happend but EAGAIN and EWOULDBLOCK meams that peer socket have been close
		//EWOULDBLOCK means messages are available at the socket and O_NONBLOCK  is set on the socket's file descriptor
		if(EAGAIN == errno && EWOULDBLOCK == errno)
		{
			//	disconnect from server
			handle_close(fd_);
			return;
		}
#endif // __LINUX
		perror("error at send");  
	}
}

void Event_Handle_Cli::_work_thread()
{
	reactor()->event_loop(1);
}

void Event_Handle_Cli::_set_reuse_addr( int __fd )
{
	int __option_name = 1;
	if(setsockopt(__fd, SOL_SOCKET, SO_REUSEADDR, (char*)&__option_name, sizeof(int)) == -1)  
	{  
		perror("setsockopt SO_REUSEADDR ");  
		exit(1);  
	}  
}

void Event_Handle_Cli::_set_no_delay( int __fd )
{
	//	The Nagle algorithm is disabled if the TCP_NODELAY option is enabled 
	int __no_delay = TRUE;
	if(SOCKET_ERROR == setsockopt( __fd, IPPROTO_TCP, TCP_NODELAY, (char*)&__no_delay, sizeof(int)))
	{
		perror("setsockopt TCP_NODELAY");  
		exit(1);  
	}
}

void Event_Handle_Cli::_get_usable( int __fd, unsigned long& __usable_size)
{
#ifndef __LINUX
	if(SOCKET_ERROR == ioctlsocket(__fd, FIONREAD, &__usable_size))
	{
		printf("ioctlsocket failed with error %d\n", WSAGetLastError());
	}
#else
	if(ioctl(__fd,FIONREAD,__usable_size))
	{
		perror("ioctl FIONREAD");
	}
#endif //__LINUX
}

int Event_Handle_Cli::read( int __fd,char* __buf, int __length )
{
	int __recv_size = recv(__fd,__buf,__length,0);
	if(0 == __recv_size)
	{
		handle_close(__fd);
	}
	else if (-1 == __recv_size)
	{
#ifndef __LINUX
		DWORD __last_error = ::GetLastError();
		if(WSAEWOULDBLOCK  == __last_error)
		{
			//	close peer socket
			handle_close(__fd);
		}
#else
		if(EAGAIN == errno || EWOULDBLOCK == errno)
		{
			handle_close(__fd);
		}
#endif //__LINUX
	}
	return __recv_size;
}

void Event_Handle_Cli::star_work_thread()
{
	//	start work thread
	auto __thread = std::thread(CC_CALLBACK_0(Event_Handle_Cli::_work_thread,this));
	__thread.detach();
}





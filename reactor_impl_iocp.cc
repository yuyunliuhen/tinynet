#include <process.h>
#include <stdio.h>
#include "reactor_impl_iocp.h"
#include "event_handle.h"

#define TIME_OVERTIME					60*1000
#define PRE_POST_RECV_NUM				2
#define PRE_POST_ACCEPT_NUM				1
#define MAX_FREE_OVERLAPPED_PLUS_NUM	5000
#define MAX_FREE_CLIENT_CONTEXT_NUM		5000
#define MAX_CONNECT_NUM					5000
#define DEFAULT_SEND_BUF_SIZE			8192
#define DEFAULT_RECV_BUF_SIZE			8192

enum emErrorCode
{
	ERROR_READ_BUFFER_ERROR	= -1,
	ERROR_CONNECION_CLOSE = 10000,		//	more than 8192
	ERROR_IO_WRITE_PENDING,
};

struct   Overlapped_Puls
{
	//	the overlapped struct 
	OVERLAPPED	overLapped_;

	//	a global WSABUF data, 
	WSABUF		wsa_buf_;

	//	the buffer's start address
	char*		buffer_;

	//	the used buffer size
	int			used_size_;

	//	total size of buffer context
	int			buffer_length_;

	//	the sequence of buffer,include post send and post recv sequence.
	LONG		sequence_num_;

	//	socket of client
	SOCKET		sock_client_;

	//	operator type 
	kOPType		op_type_;

	struct Overlapped_Puls*	next_;
public:
	//packet data interface
	//add data to buffer
	BOOL add_data(char* __data,int __length)
	{
		if (used_size_ + __length > DATA_BUFSIZE)
		{
			return FALSE;
		}
		memcpy_s(buffer_ + used_size_,__length,__data,__length);
		used_size_ += __length;
		return TRUE;
	}

	//add a bool to buffer
	BOOL add_data(bool __data)
	{
		return add_data((char*)&__data,sizeof(bool));
	}

	BOOL add_data( unsigned char __data )
	{
		return add_data((char*)&__data,sizeof(unsigned char));
	}

	BOOL add_data( short __data )
	{
		return add_data((char*)&__data,sizeof(short));
	}

	BOOL add_data( int __data )
	{
		return add_data((char*)&__data,sizeof(int));
	}

	void read_data( char* __data,const int __bytes )
	{
		memcpy_s(__data,__bytes,buffer_ + used_size_,__bytes);
	}

	void read_int( int& __val )
	{
		read_data((char*)&__val,sizeof(int));
	}

	BOOL flush_buffer( Overlapped_Puls* __next_overlap_puls,int __flush_size )
	{
		if (!__next_overlap_puls)
		{
			return FALSE;
		}
		int __not_read_bytes = buffer_length_ - used_size_;
		if (__next_overlap_puls->buffer_length_ < __flush_size)
		{
			//	fix #2
			//	to the contrary, add this->buffer_ to __next_overlap_puls->buffer_
			//	first, copy __next_overlap_puls->buffer_ to this->buffer_
			memcpy_s(buffer_,__not_read_bytes,buffer_ + used_size_,__not_read_bytes);
			memcpy_s(buffer_ + __not_read_bytes,__next_overlap_puls->buffer_length_,__next_overlap_puls->buffer_,__next_overlap_puls->buffer_length_);
			buffer_length_ = __not_read_bytes + __next_overlap_puls->buffer_length_;
			used_size_ = 0;
			memset(buffer_ + buffer_length_,0,DATA_BUFSIZE - buffer_length_);

			//	second, copy  this->buffer_ to __next_overlap_puls->buffer_
			memcpy_s(__next_overlap_puls->buffer_,this->buffer_length_,this->buffer_,this->buffer_length_);
			__next_overlap_puls->buffer_length_ = this->buffer_length_;
			return FALSE;
		}
		
		memcpy_s(buffer_,__not_read_bytes,buffer_ + used_size_,__not_read_bytes);
		memcpy_s(buffer_ + __not_read_bytes,__flush_size,__next_overlap_puls->buffer_,__flush_size);
		buffer_length_ = __not_read_bytes + __flush_size;
		used_size_ = 0;
		memset(buffer_ + buffer_length_,0,DATA_BUFSIZE - buffer_length_);
		__next_overlap_puls->setp_used_size(__flush_size);
		return TRUE;
	}

	BOOL is_enough( const int __read_bytes )
	{
		int __not_read_bytes = buffer_length_ - used_size_;
		if (__not_read_bytes < __read_bytes)
		{
			return FALSE;
		}
		return TRUE;
	}

	void setp_used_size( const int __step_bytes )
	{
		if (used_size_ + __step_bytes <= buffer_length_)
		{
			used_size_ += __step_bytes;
		}
		else
		{
			//	error, the next packet is not enough for a complete packet.
			int i = 0;
		}
	}

	int	left_size()
	{
		return buffer_length_ - used_size_;
	}
};

//desc : implement SOCKET pool instead of PER_HANDLE_DATA
struct Client_Context
{
	//	accept socket
	SOCKET						socket_;

	//	the sockaddr_in to store client address
	sockaddr_in					sockaddr_client_;

	//	if socket close or not
	BOOL						close_;

	//	number of WSARecv posted
	LONG						num_post_recv_;

	//	number of WSASend posted
	LONG						num_post_send_;

	//	the inc dequeue in the Client_Context,if a read or read overlapped operator occur in the client socket ,read_sequence_ increase by one,
	//	that is the value is the total number of post recv.
	LONG						read_sequence_;

	//	current sequence to read,if lReadSequence decrease by by one,get rid of a  Overlapped_Puls from pOutOrderReads
	LONG						cur_read_sequence_;

	//	the inc dequeue of waiting send buffer
	LONG						write_sequence_;

	//	current sequence to write data
	LONG						cur_write_sequence_;

	//	lock to protect the struct
	CRITICAL_SECTION			lock_;

	//	record the current Overlapped_Puls I/O pending but we do not known if completion or not
	Overlapped_Puls*			cur_pending_send_;

	//	current waiting send number
	LONG						waiting_send_count_;

	//	record the OVERLAPPED_PLUS which waiting send
	Overlapped_Puls*			waiting_send_;

	//	record the OVERLAPPED_PLUS which out of order  
	Overlapped_Puls*			out_order_reads_;

	struct Client_Context*		next_;
};

Reactor_Impl_Iocp::Reactor_Impl_Iocp()
{
	handle_ = NULL;
	fd_ = -1;
	memset(event_array_,0,sizeof(WSAEVENT)*WSA_MAXIMUM_WAIT_EVENTS);
	event_total_ = 0;
	pre_post_accept_num_ = PRE_POST_ACCEPT_NUM;
	pre_post_recv_num_ = PRE_POST_RECV_NUM;
	free_overlap_puls_ = NULL;
	free_overlap_puls_count_ = 0;
	max_free_overlap_puls_count_ = MAX_FREE_OVERLAPPED_PLUS_NUM;
	free_client_context_ = NULL;
	free_cleint_context_count_ = 0;
	max_free_client_context_count_ = MAX_FREE_CLIENT_CONTEXT_NUM;
	active_cleint_context_ = NULL;
	cur_connection_ = 0;
	max_connection_ = MAX_CONNECT_NUM;
	penging_accept_overlap_puls_ = NULL;
	pending_accept_count_ = NULL;
	work_thread_cur_ = 0;
}

int Reactor_Impl_Iocp::register_handle(Event_Handle* __handle,int __fd,int __mask,int __connect)
{
	if(kMaskAccept ==__mask)
	{
		fd_ = __fd;
		handle_ = __handle;
		_ready();
	}
	else
	{
		if(1 == __connect)
		{
			
		}
	}
	return -1;
}

int Reactor_Impl_Iocp::remove_handle(Event_Handle* __handle,int __mask)
{
	return -1;
}

int Reactor_Impl_Iocp::handle_event(unsigned long __millisecond)
{
	return -1;
}

int Reactor_Impl_Iocp::event_loop(unsigned long __millisecond)
{
	while(true)
	{

	}
	return -1;
}

void Reactor_Impl_Iocp::_create_completeion_port()
{
	//	if the NumberOfConcurrentThreads is 0,that means io completion port will use default value,the number of cpu ' thread.
	completeion_port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if(NULL == completeion_port_)
	{
		printf("_create_completeion_port failed with error: %d\n", GetLastError());
	}
}

void Reactor_Impl_Iocp::_associate_completeion_port(HANDLE __completion_port,HANDLE __device,ULONG_PTR __completion_key)
{
	HANDLE __completeion_port = CreateIoCompletionPort(__device, completeion_port_, __completion_key, 0);
	if(NULL == __completeion_port)
	{
		printf("_associate_completeion_port failed with error: %d\n", GetLastError());
	}
}

void Reactor_Impl_Iocp::_ready()
{
	_create_completeion_port();
	//	but when use sleep,Waitforsingelobject,waitformultinobjects,singleobjectandwait and so on,
	//	it will be make thread unusable,so the moment create another thread for use

	//	Create worker threads to service the overlapped I/O requests.  
	//	The decision to create 2 worker threads per CPU in the system is a heuristic.  
	//	Also,note that thread handles are closed right away, because we will not need them and the worker threads will continue to execute.

	int __number_work_thread = _get_cpu_number();
	work_thread_cur_ = /*__number_work_thread*/1;
	for(int __i = 0; __i < work_thread_cur_; ++__i)
	{
		_begin_thread(&work_thread_function,this);
	}

	if ((event_array_[event_total_] = WSACreateEvent()) == WSA_INVALID_EVENT)
	{
		printf("WSACreateEvent() failed with error %d\n", WSAGetLastError());
		exit(1);
	}
	++event_total_;
	if (WSAEventSelect(fd_, event_array_[event_total_ - 1], FD_ACCEPT) == SOCKET_ERROR)
	{
		printf("WSAEventSelect() failed with error %d\n", WSAGetLastError());
		exit(1);
	}
	_associate_completeion_port(completeion_port_,(HANDLE)fd_,(DWORD)0);

	// Load the AcceptEx function into memory using WSAIoctl.
	DWORD __bytes = 0;
	GUID __guid_accept_ex = WSAID_ACCEPTEX;
	if(SOCKET_ERROR  == WSAIoctl(fd_, 
		SIO_GET_EXTENSION_FUNCTION_POINTER, 
		&__guid_accept_ex, 
		sizeof(__guid_accept_ex),
		&lpfn_acceptex_, 
		sizeof(lpfn_acceptex_), 
		&__bytes, 
		NULL, 
		NULL))
	{
		printf("WSAIoctl() failed with error %d\n", WSAGetLastError());
		exit(1);
	}

	// Load the GetAcceptExSockaddrs function into memory using WSAIoctl.
	GUID __guid_get_acceptex_sockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
	WSAIoctl(fd_,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&__guid_get_acceptex_sockaddrs,
		sizeof(__guid_get_acceptex_sockaddrs),
		&lpfn_get_acceptex_sockaddrs_,
		sizeof(lpfn_get_acceptex_sockaddrs_),
		&__bytes,
		NULL,
		NULL
		);
	//	start listen thread
	_begin_thread(&listen_thread,this);	

	set_sock_opt();
}

int Reactor_Impl_Iocp::_get_cpu_number()
{
	SYSTEM_INFO __systemInfo;
	ZeroMemory(&__systemInfo,sizeof(__systemInfo));
	//	determine how many processors are on the system.
	GetSystemInfo(&__systemInfo);

	return __systemInfo.dwNumberOfProcessors;
}

void Reactor_Impl_Iocp::_begin_thread(unsigned (__stdcall * __start_address) (void *),void* __pv)
{
	unsigned int __thread_id = 0;
	uintptr_t __res = _beginthreadex( NULL, 0, __start_address, __pv, 0, &__thread_id );
	if (0 == __res)
	{
		printf("_beginthreadex exception!");
		return;
	}
	HANDLE __work_thread = (HANDLE)__res;
	try
	{ 
		if (__work_thread) 
		{ 
			CloseHandle(__work_thread);
			(__work_thread) = NULL; 
		} 
	} 
	catch(...) 
	{
		printf("CloseHandle error\n");
	}
}

unsigned int __stdcall Reactor_Impl_Iocp::work_thread_function( void* __pv )
{
	Reactor_Impl_Iocp* __this = (Reactor_Impl_Iocp*)__pv;
	DWORD __bytes_transferred = 0;
	DWORD __per_handle = 0;
	LPOVERLAPPED __overlapped = NULL;
	Overlapped_Puls* __overlapped_puls = NULL;
	while (true)
	{
		BOOL __res = GetQueuedCompletionStatus(__this->completeion_port_, &__bytes_transferred,(LPDWORD)&__per_handle,(LPOVERLAPPED*)&__overlapped, INFINITE);
		DWORD __io_error = ::WSAGetLastError();
		if(!__res && __io_error == WAIT_TIMEOUT)
		{
			//	there is not much for server to do,and this thread can die even if it still outstanding I/O request
		}
		//thread exit,thought call post PostQueuedCompletionStatus and set dwCompletionKey = -1
		if(-1 == __per_handle)
		{
			_endthreadex(0);
		}
		__overlapped_puls = CONTAINING_RECORD(__overlapped, Overlapped_Puls, overLapped_);
		if(__overlapped_puls)
		{
			__io_error = NO_ERROR;
			DWORD __flags = 0;
			if (!__res)
			{
				//specify the socket for WSAGetOverlappedResult
				SOCKET __sock = INVALID_SOCKET;
				if(__overlapped_puls->op_type_ == OP_ACCEPT)
				{
					__sock = __this->fd_;
				}
				else
				{
					if(0 == __per_handle)
					{
						break;
					}
					__sock = ((Client_Context*)__per_handle)->socket_;
				}
				DWORD dwFlags = 0;
				if(!::WSAGetOverlappedResult(__sock, &__overlapped_puls->overLapped_, &__bytes_transferred, FALSE, &dwFlags))
				{
					__io_error = WSAGetLastError();
				}
			}
			__this->_process_io(__per_handle,__overlapped_puls,__bytes_transferred,__io_error);
		}
	}
	return 0;
}

int Reactor_Impl_Iocp::handle_close( int __fd )
{
	return -1;
}

unsigned int __stdcall Reactor_Impl_Iocp::listen_thread( void* __pv )
{
	Overlapped_Puls* __overlapped_puls = NULL;
	int __error = 0;
	Reactor_Impl_Iocp* __this = (Reactor_Impl_Iocp*)__pv;
	while(TRUE)
	{
		DWORD __events = 0;
		// Wait for one of the sockets to receive I/O notification and 
		if (((__events = WSAWaitForMultipleEvents(__this->event_total_, __this->event_array_, FALSE,
			/*WSA_INFINITE*/TIME_OVERTIME, FALSE)) == WSA_WAIT_FAILED))
		{
			printf("WSAWaitForMultipleEvents failed with error %d\n", WSAGetLastError());
			__this->destoryt_net();
			return 0;
		}
		if(WSA_WAIT_TIMEOUT == __events)
		{
			__this->check_all_connection_timeout();
			//if the client connect server for a long time but not recv or send any data,disconnect it
			__overlapped_puls = __this->penging_accept_overlap_puls_;
			while(NULL != __overlapped_puls)
			{
				int __seconds = 0;
				int __bytes = sizeof(__seconds);
				//check all AcceptEx is timeout
				__error = getsockopt(__overlapped_puls->sock_client_, SOL_SOCKET, SO_CONNECT_TIME,(char*)&__seconds, (int*)&__bytes );
				if ( NO_ERROR != (__error = WSAGetLastError())) 
				{
					printf("getsockopt(SO_CONNECT_TIME) failed: %ld\n", __error);
				}
				if(-1 != __seconds && __seconds >= TIME_OVERTIME/1000)
				{
					closesocket(__overlapped_puls->sock_client_);
					__overlapped_puls->sock_client_ = INVALID_SOCKET;
				}
				__overlapped_puls = __overlapped_puls->next_;
			}
		}
		else
		{
			WSANETWORKEVENTS __network_events;
			__events = __events - WAIT_OBJECT_0;
			if (WSAEnumNetworkEvents(__this->fd_, __this->event_array_[__events - WSA_WAIT_EVENT_0], &__network_events) == SOCKET_ERROR)
			{
				printf("WSAEnumNetworkEvents failed with error %d\n", WSAGetLastError());
				return 0;
			}
			if (__network_events.lNetworkEvents & FD_ACCEPT)
			{
				if (__network_events.iErrorCode[FD_ACCEPT_BIT] != 0)
				{
					printf("FD_ACCEPT failed with error %d\n", __network_events.iErrorCode[FD_ACCEPT_BIT]);
					break;
				}
				for(int i = 0; i < __this->pre_post_accept_num_; ++i)
				{
					__overlapped_puls = __this->allocate_overlapped_puls(DATA_BUFSIZE);
					if(NULL != __overlapped_puls)
					{
						__this->post_accept(__overlapped_puls);
						__this->insert_pending_accept(__overlapped_puls);
					}
				}
				if (__this->event_total_ > WSA_MAXIMUM_WAIT_EVENTS)
				{
					printf("Too many connections - closing socket.\n");
					__this->_close_socket(__this->fd_);
					break;
				}
			}
		}
	}
	return 0;
}

void Reactor_Impl_Iocp::post_accept(Overlapped_Puls* __overlapped_plus)
{
	int __error_code = 0;
	DWORD __bytes = 0;
	// Create per I/O socket information structure to associate with the WSARecv call below.
	if(SOCKET_ERROR == (__overlapped_plus->sock_client_ = WSASocket(AF_INET,SOCK_STREAM,IPPROTO_TCP,NULL, 0,WSA_FLAG_OVERLAPPED)))
	{
		printf("WSASocket() failed with error %d\n", WSAGetLastError());
		return ;
	}
	// Empty our overlapped structure and accept connections.
	memset(&__overlapped_plus->overLapped_,0,sizeof(OVERLAPPED));
	__overlapped_plus->op_type_ = OP_ACCEPT;
	if(!lpfn_acceptex_(fd_, 
		__overlapped_plus->sock_client_,
		__overlapped_plus->buffer_, 
		__overlapped_plus->buffer_length_- ((sizeof(sockaddr_in) + 16) * 2),
		sizeof(sockaddr_in) + 16, 
		sizeof(sockaddr_in) + 16, 
		&__bytes,
		&__overlapped_plus->overLapped_))
	{
		if(ERROR_IO_PENDING != (__error_code = WSAGetLastError()))
		{
			printf("AcceptEx() failed with error %d\n", __error_code);
			return ;
		}
	}
}

Overlapped_Puls* Reactor_Impl_Iocp::allocate_overlapped_puls( int __buffer_len )
{
	if(__buffer_len > DATA_BUFSIZE)
	{
		return NULL;
	}
	Overlapped_Puls* __overlapped_plus = NULL;
	overlap_puls_lock_.acquire_lock();
	//if free overlappuls list is NULL,new a buffer,else get a block from overlappuls list
	if(NULL == free_overlap_puls_)
	{
		if ((__overlapped_plus = (Overlapped_Puls*)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY, sizeof(Overlapped_Puls) + DATA_BUFSIZE)) == NULL)
		{
			printf("HeapAlloc() failed with error %d\n", GetLastError());
		}
	}
	else
	{
		__overlapped_plus = free_overlap_puls_;
		free_overlap_puls_ = free_overlap_puls_->next_;
		__overlapped_plus->next_ = NULL;
		InterlockedDecrement(&free_overlap_puls_count_);
	}

	if(NULL != __overlapped_plus)
	{
		__overlapped_plus->buffer_ = (char*)(__overlapped_plus + 1);
		__overlapped_plus->buffer_length_ = __buffer_len;
	}
	overlap_puls_lock_.release_lock();
	return __overlapped_plus;
}

Client_Context* Reactor_Impl_Iocp::allocate_client_context(SOCKET __sock)
{
	if(INVALID_SOCKET == __sock)
	{
		return NULL;
	}
	Client_Context* __client_context = NULL;
	client_context_lock_.acquire_lock();
	if(NULL == free_client_context_)
	{
		if ((__client_context = (Client_Context*) HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY, sizeof(Client_Context))) == NULL)
		{
			printf("HeapAlloc() failed with error %d\n", GetLastError());
		}
		else
		{
			InterlockedIncrement(&cur_connection_);
			::InitializeCriticalSection(&__client_context->lock_);
		}
	}
	else
	{
		__client_context = free_client_context_;
		free_client_context_ = free_client_context_->next_;
		__client_context->next_ = NULL;
		InterlockedIncrement(&cur_connection_);
		InterlockedDecrement(&free_cleint_context_count_);
	}
	if(NULL != __client_context)
	{
		__client_context->socket_ = __sock;
		//	fix the bug #1
		__client_context->read_sequence_ = 1;
	}
	client_context_lock_.release_lock();
	return __client_context;
}

void Reactor_Impl_Iocp::release_client_context( Client_Context* __client_context )
{
	if (!__client_context)
	{
		return;
	}
	if(INVALID_SOCKET != __client_context->socket_)
	{
		_close_socket(__client_context->socket_);
	}

	client_context_lock_.acquire_lock();
	if(0 == __client_context->socket_)
	{
		client_context_lock_.release_lock();
		return ;
	}
	if(free_client_context_ != __client_context)
	{
		//first release the overlappuls in which the socket have not read yet
		Overlapped_Puls* __next_overlap_plus = NULL;
		while(NULL != __client_context->out_order_reads_)
		{
			__next_overlap_plus = __client_context->out_order_reads_->next_;
			release_overlapped_puls(__client_context->out_order_reads_);
			__client_context->out_order_reads_ = __next_overlap_plus;
		}
		__next_overlap_plus = NULL;
		while(NULL != __client_context->waiting_send_)
		{
			__next_overlap_plus = __client_context->waiting_send_->next_;
			release_overlapped_puls(__client_context->waiting_send_);
			__client_context->waiting_send_ = __next_overlap_plus;
		}
		if(free_cleint_context_count_ < max_free_client_context_count_)
		{
			__client_context->next_ = free_client_context_;
			__client_context->socket_ = INVALID_SOCKET;
			memset( &__client_context->sockaddr_client_, 0, sizeof(sockaddr_in) );
			__client_context->num_post_recv_ = 0;
			__client_context->num_post_send_ = 0;
			__client_context->read_sequence_ = 0;
			__client_context->cur_read_sequence_ = 0;
			__client_context->write_sequence_ = 0;
			__client_context->cur_write_sequence_ = 0;
			__client_context->waiting_send_count_ = 0;
			__client_context->close_ = TRUE;
			__client_context->waiting_send_ = NULL;
			__client_context->cur_pending_send_ = NULL;
			__client_context->out_order_reads_ = NULL;
			__client_context->next_ = NULL;
			free_client_context_ = __client_context;
			InterlockedDecrement(&cur_connection_);
			InterlockedIncrement(&free_overlap_puls_count_);
		}
		else
		{
			::DeleteCriticalSection(&__client_context->lock_);
			HeapFree(GetProcessHeap(),0,__client_context);
			__client_context = NULL;
			InterlockedDecrement(&cur_connection_);
		}
	}
	client_context_lock_.release_lock();
	//	to be continue ...
}

void Reactor_Impl_Iocp::_process_io( DWORD __per_handle,Overlapped_Puls* __overlapped_puls,DWORD __bytes_transferred,int __error )
{
	//	check if __bytes_transferred is 0.if so, a new client connection is coming,sub a pening acceptex count first
	Client_Context* __client_context = (Client_Context*)__per_handle;
	if(NULL != __client_context)
	{
		if( TRUE == __client_context->close_ )
		{
			//no use, it will lead to client not release corrrctly.i do not known the reason! 2011-06-16
			//return ;
		}
		::EnterCriticalSection(&__client_context->lock_);
		if(OP_READ == __overlapped_puls->op_type_)
		{
			//client socket overlapped recv count sub by one
			InterlockedDecrement(&__client_context->num_post_recv_);
		}
		else if(OP_WRITE == __overlapped_puls->op_type_)
		{
			//client socket overlapped send count sub by one
			InterlockedDecrement(&__client_context->num_post_send_);
		}
		::LeaveCriticalSection(&__client_context->lock_);
		//check the client close or not
		if( TRUE == __client_context->close_ )
		{
			if(0 == __client_context->num_post_recv_ && 0 == __client_context->num_post_send_)
			{
				release_client_context(__client_context);
			}
			release_overlapped_puls(__overlapped_puls);
			return ;
		}
	}
	else
	{
		//	remove pending accept after get a accept status from queue
		remove_pending_accept(__overlapped_puls);
	}
	if(NO_ERROR != __error)
	{
		//do with errors
		//...
		if(__overlapped_puls->op_type_ != OP_ACCEPT)
		{
			//call virtual function-----------------------------------------
			on_connection_error(__client_context, __overlapped_puls, __error);
			//call virtual function-----------------------------------------
			close_connection(__client_context);
			if(0 == __client_context->num_post_recv_ && 0 == __client_context->num_post_send_)
			{
				release_client_context(__client_context);
			}
		}
		else
		{
			if(INVALID_SOCKET != __overlapped_puls->sock_client_)
			{
				_close_socket(__overlapped_puls->sock_client_);
			}
		}
		release_overlapped_puls(__overlapped_puls);
		return ;
	}
	switch(__overlapped_puls->op_type_)
	{
	case OP_ACCEPT:
		{
			on_accept_completed( __overlapped_puls, __bytes_transferred );
		}
		break;
	case OP_READ:
		{
			on_read_completed( __client_context, __overlapped_puls, __bytes_transferred );
		}
		break;
	case OP_ZERO_READ:
		{
			on_zero_read_completed( __client_context, __overlapped_puls, __bytes_transferred );
		}
		break;
	case OP_WRITE:
		{
			on_write_completed( __client_context, __overlapped_puls, __bytes_transferred );
		}
		break;
	}
}

void Reactor_Impl_Iocp::release_overlapped_puls( Overlapped_Puls* __overlapped_puls )
{
	if(!__overlapped_puls)
	{
		return ;
	}
	overlap_puls_lock_.acquire_lock();
	if(__overlapped_puls != free_overlap_puls_)
	{
		if(free_overlap_puls_count_ < max_free_overlap_puls_count_)
		{
			memset(__overlapped_puls,0,sizeof(Overlapped_Puls) + DATA_BUFSIZE);
			__overlapped_puls->next_ = free_overlap_puls_;
			free_overlap_puls_ = __overlapped_puls;
			InterlockedIncrement(&free_overlap_puls_count_);
		}
		else
		{
			HeapFree(GetProcessHeap(),0,__overlapped_puls);
		}
	}
	overlap_puls_lock_.release_lock();
}

BOOL Reactor_Impl_Iocp::remove_pending_accept( Overlapped_Puls* __overlapped_puls )
{
	BOOL __res = FALSE;
	pending_accept_lock_.acquire_lock();
	Overlapped_Puls* __temp_overLap_plus = penging_accept_overlap_puls_;
	//if the next overlappplus just we want to find
	if(__overlapped_puls == __temp_overLap_plus)
	{
		penging_accept_overlap_puls_ = __overlapped_puls->next_;
		__res = TRUE;
	}
	else
	{
		//travel all element until find the des
		while(NULL != __temp_overLap_plus && __overlapped_puls != __temp_overLap_plus->next_)
		{
			__temp_overLap_plus = __temp_overLap_plus->next_;
		}
		//find it
		if(NULL != __temp_overLap_plus)
		{
			__temp_overLap_plus->next_ = __overlapped_puls->next_;
			__res = TRUE;
		}
	}
	if(__res)
	{
		InterlockedDecrement(&pending_accept_count_);
	}
	pending_accept_lock_.release_lock();
	return FALSE;
}

void Reactor_Impl_Iocp::on_connection_error( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls, int __error )
{

}

void Reactor_Impl_Iocp::on_accept_completed( Overlapped_Puls* __overlapped_puls,DWORD __bytes_transferred )
{
	if(0 == __bytes_transferred)
	{
		if(INVALID_SOCKET != __overlapped_puls->sock_client_)
		{
			_close_socket(__overlapped_puls->sock_client_);
		}
	}
	int __local_len = 0;
	int __rmote_len = 0;
	LPSOCKADDR __localaddr, __remoteaddr;
	Client_Context* __client_context = allocate_client_context(__overlapped_puls->sock_client_);
	if(NULL != __client_context)
	{
		if(add_connection(__client_context))
		{
			lpfn_get_acceptex_sockaddrs_(
				__overlapped_puls->buffer_,
				__overlapped_puls->buffer_length_ - ((sizeof(sockaddr_in) + 16) * 2),
				sizeof(sockaddr_in) + 16,
				sizeof(sockaddr_in) + 16,
				(SOCKADDR **)&__localaddr,
				&__local_len,
				(SOCKADDR **)&__remoteaddr,
				&__rmote_len);	
			memcpy(&__client_context->sockaddr_client_, __localaddr, __local_len);
			__client_context->close_ = FALSE;
			// Associate the accept socket with the completion port
			_associate_completeion_port(completeion_port_,(HANDLE)__client_context->socket_,(DWORD)__client_context);
			__overlapped_puls->buffer_length_ = __bytes_transferred;
			//call virtual function-----------------------------------------
			on_connection_established(__client_context,__overlapped_puls);
			//call virtual function-----------------------------------------
			//post a few WSARecv quest
			for(int i = 0; i < PRE_POST_RECV_NUM; ++i)
			{
				Overlapped_Puls* __temp_overLap_plus = allocate_overlapped_puls(DATA_BUFSIZE);
				if(NULL != __temp_overLap_plus)
				{
					if(!post_recv(__client_context,__temp_overLap_plus))
					{
						close_connection(__client_context);
						release_client_context(__client_context);
						break;
					}
				}
			}
		}
		else
		{	
			close_connection(__client_context);
			release_client_context(__client_context);
		}
	}
	else
	{
		_close_socket(__overlapped_puls->sock_client_);
	}
	//	fix the bug #1
	process_packet(__client_context,__overlapped_puls);
}

void Reactor_Impl_Iocp::on_read_completed( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls,DWORD __bytes_transferred )
{
	//check to see if an error has occured on the socket and if so then close the socket and cleanup the SOCKET_INFORMATION structure
	//associated with the socket.
	if(0 == __bytes_transferred)
	{
		__overlapped_puls->buffer_length_ = 0;
		on_connection_closing(__client_context,__overlapped_puls);
		//call virtual function-----------------------------------------
		close_connection(__client_context);
		//call virtual function-----------------------------------------
		if(0 == __client_context->num_post_recv_ && 0 == __client_context->num_post_send_)
		{
			release_client_context(__client_context);
		}
		release_overlapped_puls(__overlapped_puls);
	}
	else
	{
		__overlapped_puls->buffer_length_ = __bytes_transferred;
		process_packet(__client_context,__overlapped_puls);
		if(TRUE == __client_context->close_)
		{
			return;
		}
		int __posr_recv_left = PRE_POST_RECV_NUM - __client_context->num_post_recv_;
		for(int i = 0; i < __posr_recv_left; ++i)
		{
			Overlapped_Puls* __temp_overLap_puls = allocate_overlapped_puls(DATA_BUFSIZE);
			if(NULL != __temp_overLap_puls)
			{
				if(!post_recv(__client_context,__temp_overLap_puls))
				{
					close_connection(__client_context);
					//this part will come out memory leak, how to work it out?
					//add by Lee 2011-04-08
					if(0 == __client_context->num_post_recv_ && 0 == __client_context->num_post_send_)
					{
						release_client_context(__client_context);
					}
					break;
				}
			}
		}		
	}
}

void Reactor_Impl_Iocp::on_zero_read_completed( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls,DWORD __bytes_transferred )
{

}

void Reactor_Impl_Iocp::on_write_completed( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls,DWORD __bytes_transferred )
{
	if(0 == __bytes_transferred)
	{
		__overlapped_puls->buffer_length_ = 0;
		on_connection_closing(__client_context, __overlapped_puls);	
		//call virtual function-----------------------------------------
		close_connection(__client_context);
		//call virtual function-----------------------------------------
		if(0 == __client_context->num_post_recv_ && 0 == __client_context->num_post_send_)
		{
			release_client_context(__client_context);
		}
	}
	else
	{
		__overlapped_puls->buffer_length_ = __bytes_transferred;
		if ( __client_context->cur_pending_send_ == __overlapped_puls )
		{
			__client_context->cur_pending_send_ = NULL;
		}
		//update 2011-06-22
		//if there any buffer waiting send
		if ( __client_context->waiting_send_ )
		{
			send_pending_send( __client_context );
		}
	}
	::InterlockedIncrement(&__client_context->cur_write_sequence_);
	release_overlapped_puls(__overlapped_puls);
}

void Reactor_Impl_Iocp::close_connection( Client_Context* __client_context )
{
	active_clienk_context_lock_.acquire_lock();
	if(0 == __client_context->socket_)
	{
		active_clienk_context_lock_.release_lock();
		return ;
	}
	Client_Context*  __temp_client_context = active_cleint_context_;
	if(__temp_client_context == __client_context)
	{
		active_cleint_context_ = __client_context->next_;
	}
	else
	{
		while(NULL != __temp_client_context && __temp_client_context->next_ != __client_context)
		{
			__temp_client_context = __temp_client_context->next_;
		}
		if(NULL != __temp_client_context)
		{
			//update 2011-04-05 by Lee
			__temp_client_context->next_ = __client_context->next_;
		}
	}
	//close socket
	::EnterCriticalSection(&__client_context->lock_);
	if(INVALID_SOCKET != __client_context->socket_)
	{
		//add 2011-04-13 by Lee
		//force the subsequent closesocket to be abortative.
		LINGER  lingerStruct;
		lingerStruct.l_onoff = 1;
		lingerStruct.l_linger = 0;
		setsockopt(__client_context->socket_, SOL_SOCKET, SO_LINGER,(char*)&lingerStruct, sizeof(lingerStruct));
		//add 2011-04-21 by Lee
		//now close the socket handle.this will do an abortive or graceful close, as requested.  
		CancelIo((HANDLE)__client_context->socket_);
		_close_socket(__client_context->socket_);
	}
	__client_context->close_ = TRUE;
	::LeaveCriticalSection(&__client_context->lock_);
	active_clienk_context_lock_.release_lock();
}

void Reactor_Impl_Iocp::_close_socket( SOCKET __socket )
{
	closesocket(__socket);
	__socket = INVALID_SOCKET;
}

BOOL Reactor_Impl_Iocp::add_connection( Client_Context* __client_context )
{
	active_clienk_context_lock_.acquire_lock();
	if(cur_connection_ < max_connection_)
	{
		__client_context->next_ = active_cleint_context_;
		active_cleint_context_ = __client_context;
		active_clienk_context_lock_.release_lock();
		return TRUE;
	}
	active_clienk_context_lock_.release_lock();
	return FALSE;
}

void Reactor_Impl_Iocp::on_connection_established( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls )
{

}

BOOL Reactor_Impl_Iocp::post_recv( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls )
{
	::EnterCriticalSection(&__client_context->lock_);
	__overlapped_puls->sequence_num_ = __client_context->read_sequence_;
	int __error = 0;
	DWORD __bytes = 0;
	DWORD __flags = 0;
	__overlapped_puls->op_type_ = OP_READ;
	__overlapped_puls->wsa_buf_.buf = __overlapped_puls->buffer_;
	__overlapped_puls->wsa_buf_.len = __overlapped_puls->buffer_length_;
	int __res = WSARecv(__client_context->socket_, &__overlapped_puls->wsa_buf_, 1, &__bytes, &__flags, &__overlapped_puls->overLapped_, NULL);
	if ((__res == SOCKET_ERROR) && (WSA_IO_PENDING != (__error = WSAGetLastError()))) 
	{
		printf("WSARecv failed: %d\n", __error);
		::LeaveCriticalSection(&__client_context->lock_);
		return FALSE;
	} 
	InterlockedIncrement(&__client_context->read_sequence_);
	InterlockedIncrement(&__client_context->num_post_recv_);
	::LeaveCriticalSection(&__client_context->lock_);
	return TRUE;
}

void Reactor_Impl_Iocp::process_packet( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls )
{
	out_read_overlap_puls_lock_.acquire_lock();
	Overlapped_Puls* __overlapped_puls_read = get_next_read_overlap_puls(__client_context,__overlapped_puls);
	while(NULL != __overlapped_puls_read)
	{
		//read packet completed,the buffer of pOverLapPlus' data just one or multi complete packet;
		int __read_less_size = read_packet(__client_context,__overlapped_puls_read);
		if( ERROR_READ_BUFFER_ERROR == __read_less_size )
		{
			//if ERROR_READ_BUFFER_ERROR happed, that means data error.
			//just release memory,this a temp solution!!!
			::InterlockedIncrement(&__client_context->cur_read_sequence_);
			release_overlapped_puls(__overlapped_puls_read);
			break;
		}
		else if( ERROR_CONNECION_CLOSE == __read_less_size )
		{
			out_read_overlap_puls_lock_.release_lock();
			return ;
		}
		if (0 == __read_less_size)
		{
			//add the sequence of the data to read
			::InterlockedIncrement(&__client_context->cur_read_sequence_);
			release_overlapped_puls(__overlapped_puls_read);
			__overlapped_puls_read = get_next_read_overlap_puls(__client_context,NULL);
		}
		//read not completed,read the other part from next buffer
		else
		{
			//make sure the sequence is increment one, if not, the data will not read correctly.
			::InterlockedIncrement(&__client_context->cur_read_sequence_);
			Overlapped_Puls* __temp_next_overlap_puls_read = get_next_read_overlap_puls(__client_context,NULL);
			if (!__temp_next_overlap_puls_read)
			{
				//add by Lee 2011-05-28
				//if the next buffer is not exist, add current buffer to the out order buffer,and clear the used data,
				//then continue process io
				//to be continue...
				::InterlockedDecrement(&__client_context->cur_read_sequence_);
				::InterlockedDecrement(&__client_context->cur_read_sequence_);
				get_next_read_overlap_puls(__client_context,__overlapped_puls_read);
				::InterlockedIncrement(&__client_context->cur_read_sequence_);
				break;
			}
			else
			{
				//flush buffer,remove nReadLeftSize from pTempOverLapPulsRead and add to current pOverLapPulsRead
				if(__overlapped_puls_read->flush_buffer(__temp_next_overlap_puls_read,__read_less_size))
				{
					::InterlockedDecrement(&__client_context->cur_read_sequence_);
					//add the __temp_next_overlap_puls_read to out order buffer
					get_next_read_overlap_puls(__client_context,__temp_next_overlap_puls_read);
				}
				else
				{
					__overlapped_puls_read = __overlapped_puls_read->next_;
				}
			}
		}
	}
	out_read_overlap_puls_lock_.release_lock();
}

void Reactor_Impl_Iocp::on_connection_closing( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls )
{

}

BOOL Reactor_Impl_Iocp::post_send( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls )
{
	::EnterCriticalSection(&__client_context->lock_);
	DWORD __bytes = 0;
	DWORD __flags = 0;
	__overlapped_puls->op_type_ = OP_WRITE;
	__overlapped_puls->wsa_buf_.buf = __overlapped_puls->buffer_;
	__overlapped_puls->wsa_buf_.len = __overlapped_puls->buffer_length_;
	int __res = WSASend(__client_context->socket_, &__overlapped_puls->wsa_buf_, 1, &__bytes, __flags, &__overlapped_puls->overLapped_, NULL);
	if ( __res == SOCKET_ERROR ) 
	{
		if( WSA_IO_PENDING == WSAGetLastError() )
		{
			__client_context->cur_pending_send_ = __overlapped_puls;
		}
		else
		{
			printf("WSASend failed: %d\n", WSAGetLastError());
			::LeaveCriticalSection(&__client_context->lock_);
			//close the socket and release client context,maybe we should close socket safety,how to do this,use HasOverlappedIoCompleted?
			//PostRecv not use this way
			close_connection(__client_context);
			release_client_context(__client_context);
			return FALSE;
		}
	}
	InterlockedIncrement(&__client_context->num_post_send_);
	::LeaveCriticalSection(&__client_context->lock_);
	return TRUE;
}

BOOL Reactor_Impl_Iocp::write( Client_Context* __client_context,const char* __data, int __length )
{

	return FALSE;
}

void Reactor_Impl_Iocp::send_pending_send( Client_Context* __client_context )
{
	waiting_sendt_lock_.acquire_lock();
	Overlapped_Puls* __waiting_send_buffer = get_penging_send( __client_context );
	while( __waiting_send_buffer )
	{
		BOOL __res = send_2_client( __client_context, __waiting_send_buffer );
		if ( __res )
		{
			remove_pending_send( __client_context, __waiting_send_buffer );
			__waiting_send_buffer = get_penging_send( __client_context );
		}
		else
		{
			break;
		}
	}
	waiting_sendt_lock_.release_lock();
}

void Reactor_Impl_Iocp::broadcast( int __fd,const char* __data,unsigned int __length )
{

}

Reactor_Impl_Iocp::~Reactor_Impl_Iocp()
{
	::closesocket(fd_);
}

void Reactor_Impl_Iocp::set_sock_opt()
{
	int __dont_linger = true;
	int __size = sizeof(int);
	int __ret = getsockopt( fd_,SOL_SOCKET,SO_DONTLINGER,(char *)&__dont_linger, &__size );
	if(__ret == SOCKET_ERROR)
	{
		printf("getsockopt SO_DONTLINGER failed with error: %d\n", WSAGetLastError() );
		return ;
	}

	//get the size of send buffer,default size is 8192(windows 7)
	int __send_buf_size = 0;
	__ret = getsockopt(fd_,SOL_SOCKET,SO_SNDBUF,(char*)&__send_buf_size,&__size);
	if(__ret == SOCKET_ERROR)
	{
		printf("getsockopt SO_SNDBUF failed with error: %d\n", WSAGetLastError());
		return ;
	}
	/*
		Disable send buffering on the socket.  Setting SO_SNDBUFto 0 causes winsock to stop buffering sends 
		and perform sends directly from our buffers, thereby reducing CPU usage.

		However, this does prevent the socket from ever filling the send pipeline. 
		This can lead to packets being sent that are not full (i.e. the overhead of the IP and TCP headers is 
		great compared to the amount of data being carried).

		Disabling the send buffer has less serious repercussions than disabling the receive buffer.
	*/
	int __zero = __send_buf_size;
	__ret = setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, (char *)&__zero, sizeof(__zero));
	if(__ret == SOCKET_ERROR)
	{
		printf("setsockopt SO_SNDBUF failed with error: %d\n", WSAGetLastError());
		return ;
	}

	//Don't disable receive buffering. This will cause poor network
	//performance since if no receive is posted and no receive buffers,
	//the TCP stack will set the window size to zero and the peer will
	//no longer be allowed to send data.
	//get the size of recv buffer,default size is 8192(windows 7)
	int __recv_buf_size = 0;
	__ret = getsockopt(fd_,SOL_SOCKET,SO_RCVBUF,(char*)&__recv_buf_size,&__size);
	if(__ret == SOCKET_ERROR)
	{
		printf("getsockopt SO_RCVBUF failed with error: %d\n", WSAGetLastError());
		return ;
	}
	//set the size of recv buffer
	__ret = setsockopt(fd_,SOL_SOCKET,SO_RCVBUF,(char*)&__recv_buf_size,sizeof(int));
	if(__ret == SOCKET_ERROR)
	{
		printf("setsockopt SO_RCVBUF failed with error: %d\n", WSAGetLastError());
		return ;
	}

	//!!!!!!
	//Do not set a linger value...especially don't set it to an abortive
	//close. If you set abortive close and there happens to be a bit of
	//data remaining to be transfered (or data that has not been 
	//acknowledged by the peer), the connection will be forcefully reset
	//and will lead to a loss of data (i.e. the peer won't get the last
	//bit of data). This is BAD. If you are worried about malicious
	//clients connecting and then not sending or receiving, the server
	//should maintain a timer on each connection. If after some point,
	//the server deems a connection is "stale" it can then set linger
	//to be abortive and close the connection.
	//
	/*
	struct linger ling;
	ling.l_onoff = 1;
	ling.l_linger = 0;
	//if ling.l_linger is 0,close socket at once,else waiting all data is recv/send or timeout.
	__ret = setsockopt( fd_, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));
	if(__ret == SOCKET_ERROR)
	{
		printf("setsockopt SO_LINGER failed with error: %d\n", WSAGetLastError());
		return ;
	}
	*/

	int __keep_alive = 1;
	__ret = setsockopt( fd_, SOL_SOCKET, SO_KEEPALIVE, (char*)&__keep_alive, sizeof(int));
	if(__ret == SOCKET_ERROR)
	{
		printf("setsockopt SO_KEEPALIVE failed with error: %d\n", WSAGetLastError());
		return ;
	}

	//The Nagle algorithm is disabled if the TCP_NODELAY option is enabled 
	/*
	int _no_delay = TRUE;
	__ret = setsockopt( fd_, IPPROTO_TCP, TCP_NODELAY, (char*)&_no_delay, sizeof(int));
	if(__ret == SOCKET_ERROR)
	{
		printf("setsockopt IPPROTO_TCP failed with error: %d\n", WSAGetLastError());
		return ;
	}
	*/
}

void Reactor_Impl_Iocp::destoryt_net()
{
	//close all client connection
	close_all_connection();
	_close_socket(fd_);
	//all thread exit
	for(int i = 0; i < work_thread_cur_; ++i)
	{
		::PostQueuedCompletionStatus(completeion_port_, 0, -1, NULL);
	}
	if(completeion_port_)
	{
		CloseHandle(completeion_port_);
		completeion_port_ = NULL;
	}
	free_all_client_context();
	free_all_overlap_puls();
	_endthreadex(0);
}

void Reactor_Impl_Iocp::close_all_connection()
{
	active_clienk_context_lock_.acquire_lock();
	Client_Context* __client_context = active_cleint_context_;
	while(NULL != __client_context)
	{
		::EnterCriticalSection(&__client_context->lock_);
		if(INVALID_SOCKET != __client_context->socket_)
		{
			_close_socket(__client_context->socket_);
		}
		__client_context->close_ = TRUE;
		::LeaveCriticalSection(&__client_context->lock_);
		__client_context = __client_context->next_;
	}
	active_clienk_context_lock_.release_lock();
}

void Reactor_Impl_Iocp::free_all_client_context()
{
	client_context_lock_.acquire_lock();
	//add 2011-04-14
	//first release all active client context if the list is not empty
	Client_Context* __active_client_context = active_cleint_context_;
	Client_Context* __next_active_client_context = NULL;
	while(NULL != __active_client_context)
	{
		__next_active_client_context = __active_client_context->next_;
		HeapFree(GetProcessHeap(),0,__active_client_context);
		__active_client_context = __next_active_client_context;
		InterlockedDecrement(&cur_connection_);
	}
	__active_client_context = NULL;
	cur_connection_ = 0;
	//and the free all free client context;
	Client_Context* __free_client_context = free_client_context_;
	Client_Context* __next_free_client_context = NULL;
	while(NULL != __next_free_client_context)
	{
		//update 2011-04-14
		__next_free_client_context = __next_free_client_context->next_;
		HeapFree(GetProcessHeap(),0,__next_free_client_context);
		__next_free_client_context = __next_free_client_context;
		InterlockedDecrement(&free_cleint_context_count_);
	}
	free_client_context_ = NULL;
	free_cleint_context_count_ = 0;
	client_context_lock_.release_lock();
}

void Reactor_Impl_Iocp::free_all_overlap_puls()
{
	overlap_puls_lock_.acquire_lock();
	Overlapped_Puls* __free_overlap_plus = free_overlap_puls_;
	Overlapped_Puls* __next_free_overlap_plus = NULL;
	while(NULL != __free_overlap_plus)
	{
		__next_free_overlap_plus = __free_overlap_plus->next_;
		HeapFree(GetProcessHeap(),0,__free_overlap_plus);
		__free_overlap_plus = __next_free_overlap_plus;
	}
	__free_overlap_plus = NULL;
	free_overlap_puls_count_ = 0;
	overlap_puls_lock_.release_lock();
}

void Reactor_Impl_Iocp::check_all_connection_timeout()
{

}

void Reactor_Impl_Iocp::insert_pending_accept( Overlapped_Puls* __overlapped_puls )
{
	pending_accept_lock_.acquire_lock();
	if(NULL == penging_accept_overlap_puls_)
	{
		penging_accept_overlap_puls_ = __overlapped_puls;
	}
	else 
	{
		__overlapped_puls->next_ = penging_accept_overlap_puls_;
		penging_accept_overlap_puls_ = __overlapped_puls;
	}
	InterlockedIncrement(&pending_accept_count_);
	pending_accept_lock_.release_lock();
}

void Reactor_Impl_Iocp::insert_pending_send( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls )
{
	waiting_sendt_lock_.acquire_lock();
	if(NULL != __overlapped_puls)
	{
		__overlapped_puls->next_ = NULL;
		Overlapped_Puls* __temp_overlap_plus = __client_context->waiting_send_;
		Overlapped_Puls* __pre_overlap_plus = NULL;
		while(NULL != __temp_overlap_plus)
		{
			if(__overlapped_puls->sequence_num_ < __temp_overlap_plus->sequence_num_)
			{
				break;
			}
			__pre_overlap_plus = __temp_overlap_plus;
			__temp_overlap_plus = __temp_overlap_plus->next_;
		}
		//insert the head of list
		if(NULL == __pre_overlap_plus)
		{
			__overlapped_puls->next_ = __client_context->waiting_send_;
			__client_context->waiting_send_ = __overlapped_puls;
		}
		//insert the mid of list
		else
		{
			__overlapped_puls->next_ = __pre_overlap_plus->next_;
			__pre_overlap_plus->next_ = __overlapped_puls;
		}
	}
	InterlockedIncrement(&__client_context->waiting_send_count_);
	waiting_sendt_lock_.release_lock();
}

Overlapped_Puls* Reactor_Impl_Iocp::get_next_read_overlap_puls( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls )
{
	if(NULL != __overlapped_puls)
	{
		//if client current read sequence is equal current overlappuls' s sequence,the overlappuls is the just we want to read
		if(__client_context->cur_read_sequence_ == __overlapped_puls->sequence_num_)
		{
			return __overlapped_puls;
		}
		__overlapped_puls->next_ = NULL;
		Overlapped_Puls* __temp_overlap_plus = __client_context->out_order_reads_;
		Overlapped_Puls* __pre_overlap_plus = NULL;
		//traverse all client order reads until the end,and record the last overlappuls.
		//and make sure the pOutOrderReads in the order
		while(NULL != __temp_overlap_plus)
		{
			if(__overlapped_puls->sequence_num_ < __temp_overlap_plus->sequence_num_)
			{
				break;
			}
			__pre_overlap_plus = __temp_overlap_plus;
			__temp_overlap_plus = __temp_overlap_plus->next_;
		}
		//insert the head of list
		if(NULL == __pre_overlap_plus)
		{
			__overlapped_puls->next_ = __client_context->out_order_reads_;
			__client_context->out_order_reads_ = __overlapped_puls;
		}
		//insert the mid of list
		else
		{
			__overlapped_puls->next_ = __pre_overlap_plus->next_;
			__pre_overlap_plus->next_ = __overlapped_puls;
		}
	}
	Overlapped_Puls* __temp_overlap_plus = __client_context->out_order_reads_;
	if(NULL != __temp_overlap_plus)
	{
		if(__client_context->cur_read_sequence_ == __temp_overlap_plus->sequence_num_)
		{
			__client_context->out_order_reads_ = __temp_overlap_plus->next_;
			return __temp_overlap_plus;
		}
	}
	return NULL;
}

int Reactor_Impl_Iocp::read_packet( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls )
{
	const int __head_size = 12;
	unsigned char __packet_head[__head_size] = {};
	int __head = 0;
	int __packet_length = 0;
	int __log_level = 0;
	int __frame_number = 0;
	unsigned int __guid = 0;
	BOOL __enough = __overlapped_puls->is_enough(__head_size);
	if(__enough)
	{
		__overlapped_puls->read_data((char*)__packet_head,__head_size);
		memcpy(&__packet_length,__packet_head,4);
		memcpy(&__head,__packet_head + 4,4);
		memcpy(&__guid,__packet_head + 8,4);
	}
	else
	{
		int __left_size = __overlapped_puls->left_size();
		if(0 == __left_size)
		{
			//read completed
			return 0;
		}
		return __head_size - __left_size;
	}
	if (0 == __packet_length)
	{
		//read completed
		return 0;
	}
	if ( 0 > __packet_length || DEFAULT_RECV_BUF_SIZE < __packet_length )
	{
		//usually, the packet is not completed
		//when one thread process a buffer,but the buffer is not complete,so waiting the next buffer coming,
		//but the next buffer have been processing by another thread, this condition lead the problem.
		return ERROR_READ_BUFFER_ERROR;
	}
	__enough = __overlapped_puls->is_enough(__packet_length + __head_size);
	while (__enough)
	{
		BOOL __res = send_2_client(__client_context,__overlapped_puls->buffer_ + __overlapped_puls->used_size_,__packet_length + __head_size);
		if( FALSE == __res )
		{
			//add by Lee 2011-06-08
			//socket close or other error happen
			return ERROR_CONNECION_CLOSE;
		}
		else
		{
			__overlapped_puls->setp_used_size( __packet_length + __head_size );
		}
		memset(__packet_head,0,__head_size);
		__head = 0;
		__packet_length = 0;
		__log_level = 0;
		__frame_number = 0;
		__enough = __overlapped_puls->is_enough(__head_size);
		if(__enough)
		{
			__overlapped_puls->read_data((char*)__packet_head,__head_size);
			memcpy(&__packet_length,__packet_head,4);
			memcpy(&__head,__packet_head + 4,4);
			memcpy(&__guid,__packet_head + 8,4);
		}
		else
		{
			int __left_size = __overlapped_puls->left_size();
			if(0 == __left_size)
			{
				//read completed
				return 0;
			}
			return __head_size - __left_size;
		}
		if (0 == __packet_length)
		{
			//read completed
			return 0;
		}
		else if ( 0 > __packet_length || DEFAULT_RECV_BUF_SIZE < __packet_length )
		{
			return ERROR_READ_BUFFER_ERROR;
		}
		__enough = __overlapped_puls->is_enough(__packet_length + __head_size);
	}
	return __packet_length - __overlapped_puls->buffer_length_ + __overlapped_puls->used_size_ + __head_size;
}

void Reactor_Impl_Iocp::send_2_all_client( const char* __data, int __length )
{
	Overlapped_Puls* __overlap_plus = allocate_overlapped_puls(__length);
	if(NULL != __overlap_plus)
	{
		memcpy(__overlap_plus->buffer_,__data,__length);
		BOOL __res = TRUE;
		active_clienk_context_lock_.acquire_lock();
		Client_Context* __first_client_context = active_cleint_context_;
		post_send(__first_client_context,__overlap_plus);
		while(NULL != __first_client_context->next_ && TRUE == __res)
		{
			__res = post_send(__first_client_context,__overlap_plus);
			__first_client_context = __first_client_context->next_;
		}
		active_clienk_context_lock_.release_lock();
	}
	release_overlapped_puls(__overlap_plus);
}

void Reactor_Impl_Iocp::send_2_all_client( Overlapped_Puls* __overlapped_puls )
{
	if(NULL != __overlapped_puls)
	{
		BOOL __res = TRUE;
		active_clienk_context_lock_.acquire_lock();
		Client_Context* __first_client_context = active_cleint_context_;
		post_send(__first_client_context,__overlapped_puls);
		while(NULL != __first_client_context->next_ && TRUE == __res)
		{
			__res = post_send(__first_client_context,__overlapped_puls);
			__first_client_context = __first_client_context->next_;
		}
		active_clienk_context_lock_.release_lock();
	}
}

BOOL Reactor_Impl_Iocp::send_2_client( Client_Context* __client_context,const char* __data, int __length )
{
	Overlapped_Puls* __overlapped_puls = allocate_overlapped_puls(__length);
	if(NULL != __overlapped_puls)
	{
		__overlapped_puls->sequence_num_ = __client_context->write_sequence_;
		InterlockedIncrement(&__client_context->write_sequence_);
		memcpy(__overlapped_puls->buffer_,__data,__length);
		if ( __client_context->cur_pending_send_ )
		{
			//check the pending send if complete or not
			if( !HasOverlappedIoCompleted( &__client_context->cur_pending_send_->overLapped_ ))
			{
				insert_pending_send(__client_context,__overlapped_puls);
				return TRUE;
			}
		}
		if(__client_context)
		{
			//add by Lee 2011-06-20
			//if the overlapped have not finished, add the pending send to list
			//if the client send packet one packer per time, it is not need to check sequence!
			if ( __client_context->cur_write_sequence_ == __overlapped_puls->sequence_num_ )
			{
				return post_send(__client_context, __overlapped_puls);
			}
			else
			{
				insert_pending_send(__client_context,__overlapped_puls);
				send_pending_send( __client_context );
				return TRUE;
			}
		}
	}
	return FALSE;
}

BOOL Reactor_Impl_Iocp::send_2_client( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls )
{
	if(NULL != __overlapped_puls)
	{
		if ( __client_context->cur_pending_send_ )
		{
			//check the pending send if complete or not
			if( !HasOverlappedIoCompleted( &__client_context->cur_pending_send_->overLapped_ ))
			{
				//usually, the buffer is get from the list of waiting send,so return false but not add to list again
				return FALSE;
			}
		}
		else
		{
			return post_send(__client_context, __overlapped_puls);
		}
	}
	return FALSE;
}

Overlapped_Puls* Reactor_Impl_Iocp::get_penging_send( Client_Context* __client_context )
{
	if ( !__client_context->waiting_send_ )
	{
		return NULL;
	}
	if ( __client_context->cur_write_sequence_ == __client_context->waiting_send_->sequence_num_ )
	{
		return __client_context->waiting_send_;
	}
	return NULL;
}

BOOL Reactor_Impl_Iocp::remove_pending_send( Client_Context* __client_context,Overlapped_Puls* __overlapped_puls )
{
	BOOL __res = FALSE;
	Overlapped_Puls* __temp_overlap_plus = __client_context->waiting_send_;
	//if the next overlappplus just we want to find
	if(__overlapped_puls == __temp_overlap_plus)
	{
		__client_context->waiting_send_ = __overlapped_puls->next_;
		__res = TRUE;
	}
	else
	{
		//travel all element until find the des
		while(NULL != __temp_overlap_plus && __overlapped_puls != __temp_overlap_plus->next_)
		{
			__temp_overlap_plus = __temp_overlap_plus->next_;
		}
		//find it
		if(NULL != __temp_overlap_plus)
		{
			__temp_overlap_plus->next_ = __overlapped_puls->next_;
			__res = TRUE;
		}
	}
	return __res;
}

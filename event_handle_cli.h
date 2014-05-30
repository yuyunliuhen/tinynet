#ifndef event_handle_cli_h__
#define event_handle_cli_h__
#include <string>
#include "event_handle.h"

#define CC_CALLBACK_0(__selector__,__target__, ...) std::bind(&__selector__,__target__, ##__VA_ARGS__)

class Event_Handle_Cli : public  Event_Handle
{
public:
	Event_Handle_Cli(Reactor* __reactor,const char* __host,unsigned int __port);

	virtual int handle_input(int __fd);

	virtual int handle_output(int __fd);

	virtual int handle_exception(int __fd);

	virtual int handle_close(int __fd);

	virtual int handle_timeout(int __fd);

	virtual int get_handle() const { return fd_;}

	void write(const char* __data,unsigned int __length);

	//	read data from network cache
	int	read(int __fd,char* __buf, int __length); 

protected:
	void star_work_thread();

public:
	//	pure virtual function, subclass must define it.
	virtual void on_read(int __fd,const char* __data,unsigned int __length) = 0;

private:
	void 	_init(unsigned int __port = 9876);

	void 	_set_noblock(int __fd);

	void	_set_reuse_addr(int __fd);

	void	_set_no_delay(int __fd);

	void	_get_usable(int __fd,unsigned long& __usable_size);

	void	_work_thread();
private:
	int  	fd_;

	std::string		host_;

	unsigned		port_;
};

#endif // event_handle_cli_h__

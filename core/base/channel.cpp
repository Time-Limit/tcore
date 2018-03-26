#include "channel.h"
#include "exptype.h"
#include "neter.h"
#include "thread.h"
#include <arpa/inet.h>

Channel::Channel(int f)
	: fd(f)
	, ready_close(false)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

void Channel::Handle(const epoll_event *event)
{
	if(event->events & (EPOLLIN | EPOLLERR | EPOLLHUP))
	{
		Recv();
		OnRecv();
	}
	if(event->events & EPOLLOUT)
	{
		OnSend();
	}
	LOG_TRACE("Channel::Handle, fd=%d, events=0x%x", fd, event->events);
}

void Channel::Close()
{
	if(IsClose()) return;
	ready_close = true;
	Neter::GetInstance().ReadyClose(this);
}

void Exchanger::Send(const void * buf, size_t size)
{
	if(IsClose()) return ;
	static const size_t OBUFF_SIZE_LIMIT = 64*1024*1024; 
	MutexGuard guarder(olock);
	if(obuff.size() + size > OBUFF_SIZE_LIMIT)
	{
		LOG_ERROR("Exchanger::Send, size limit exceed, ip=%s, size=%lu", ip, obuff.size() + size);
		Close();
		return ;
	}
	obuff.insert(obuff.end(), buf, size);
	RegisterSendEvent();
}

void Exchanger::OnSend()
{
	if(IsClose()) return;

	MutexGuard guarder(olock);
	ready_send = false;
	int per_cnt = write(fd, ((char *)obuff.begin()) + cur_cursor, obuff.size() - cur_cursor);
	if(per_cnt < 0)
	{
		if(errno == EAGAIN || errno == EINTR)
		{
			RegisterSendEvent();
			return ;
		}
		LOG_ERROR("Exchanger::Send, errno=%d, info=%s", errno, strerror(errno));
		obuff.clear();
		cur_cursor = 0;
		Close();
		return ;
	}
	if((size_t)per_cnt == obuff.size() - cur_cursor)
	{
		cur_cursor = 0;
		obuff.clear();
		return ;
	}
	cur_cursor += per_cnt;
	RegisterSendEvent();
}

void Exchanger::Recv()
{
	if(IsClose()) return;
	int cnt = 0, per_cnt = 0;
	do
	{
		while((per_cnt = read(fd, tmp_buff, TMP_BUFF_SIZE)) > 0)
		{
			ibuff.insert(ibuff.end(), tmp_buff, per_cnt);
			cnt += per_cnt;
			if(per_cnt < TMP_BUFF_SIZE)
			{
				return ;
			}
		}
	}while(per_cnt < 0 && errno == EINTR);

	if(cnt && per_cnt < 0 && errno == EAGAIN)
	{
		return ;
	}
	
	Close();

	LOG_ERROR("Exchanger::Recv, cnt=%d, errno=%d, info=%s", cnt, errno, strerror(errno));
}

void Exchanger::OnRecv()
{
	if(IsClose()) return;
	session->DataIn(ibuff);
	ibuff.clear();
}

void Exchanger::InitPeerName()
{
	struct sockaddr_in peer;
	memset(ip, 0, sizeof(ip));
	int len = sizeof(sockaddr_in);
	if(getpeername(fd, (sockaddr *)&peer, (socklen_t *)&len) == 0)
	{
		inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
		LOG_TRACE("Exchanger::InitPeerName, ip=%s", ip);
	}
	else
	{
		Close();
		LOG_ERROR("Exchanger::InitPeerName, getpeername failed, errno=%s", strerror(errno));
	}
}

void Exchanger::RegisterSendEvent()
{
	if(ready_send == false)
	{
		ready_send = true;
		epoll_event event;
		event.events = EPOLLIN | EPOLLOUT | EPOLLET;
		event.data.ptr = this;
		Neter::GetInstance().Ctl(EPOLL_CTL_MOD, fd, &event);
	}
}

void Exchanger::RegisterRecvEvent()
{
	if(ready_send == false)
	{
		ready_send = true;
		epoll_event event;
		event.events = EPOLLIN | EPOLLOUT | EPOLLET;
		event.data.ptr = this;
		Neter::GetInstance().Ctl(EPOLL_CTL_MOD, fd, &event);
	}
}

void Acceptor::OnRecv()
{
	if(IsClose())
	{
		return ;
	}
	struct sockaddr_in accept_addr;
	int server_addr_len;
	int connect_fd;
	
	while((connect_fd = accept(fd, (struct sockaddr *)&accept_addr, (socklen_t *)&server_addr_len)) > 0 || errno == EINTR)
	{
		LOG_TRACE("Acceptor::OnRecv, connect_fd=%d", connect_fd);
		session_manager.OnConnect(connect_fd);
	}
}

bool Acceptor::Listen(const char * ip, int port, SessionManager &manager)
{
	if(!ip)
	{
		LOG_ERROR("Acceptor::Listen, invalid ip address.");
		return false;
	}
	int sockfd = 0;
	int optval = -1;
	struct sockaddr_in server;
	socklen_t socklen = sizeof(struct sockaddr_in);

	sockfd = socket(PF_INET, SOCK_STREAM, 0);

	if(sockfd < 0)
	{
		LOG_ERROR("Acceptor::Init, socket, error=%s", strerror(errno));
		return false;
	}

	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	memset(&server, 0, socklen);
	struct in_addr address;
	if(inet_pton(AF_INET, ip, &address) == -1)
	{
		LOG_ERROR("Acceptor::inet_pton, ip=%s, error=%s", ip, strerror(errno));
		return false;
	}
	server.sin_addr.s_addr = address.s_addr;
	server.sin_port = htons(port);
	server.sin_family = AF_INET;

	if(bind(sockfd, (struct sockaddr *)&server, socklen) < 0)
	{
		LOG_ERROR("Acceptor::Listen, bind, error=%s", strerror(errno));
		return false ;
	}

	listen(sockfd, LISTEN_QUEUE_SIZE);
	LOG_TRACE("Acceptor::Init, fd=%d", sockfd);	

	epoll_event ev;
	ev.events = EPOLLIN|EPOLLET|EPOLLET;
	Acceptor *acceptor = new Acceptor(sockfd, manager);
	ev.data.ptr = acceptor;
	Neter::GetInstance().Ctl(EPOLL_CTL_ADD, acceptor->ID(), &ev);

	return true;
}

void SecureExchanger::OnSend()
{

	if(IsClose()) return;

	if(!is_handshake_finish)
	{
		Handshake();
		return ;
	}

	MutexGuard guarder(olock);
	ready_send = false;
	int per_cnt = SSL_write(ssl, ((char *)obuff.begin()) + cur_cursor, obuff.size() - cur_cursor);
	if(per_cnt < 0)
	{
		int error = SSL_get_error(ssl, per_cnt);
		if(error == SSL_ERROR_WANT_WRITE)
		{
			RegisterRecvEvent();
			return ;
		}
		LOG_ERROR("SecureExchanger::OnSend, errno=%d, info=%s", errno, strerror(errno));
		obuff.clear();
		cur_cursor = 0;
		Close();
		return ;
	}
	if((size_t)per_cnt == obuff.size() - cur_cursor)
	{
		cur_cursor = 0;
		obuff.clear();
		return ;
	}
	cur_cursor += per_cnt;
	RegisterSendEvent();
}

bool SecureExchanger::Handshake()
{
	if(is_handshake_finish)
	{
		return true;
	}

	if(!ssl)
	{
		return false;
	}

	int r = 0;
	if(!is_init_ssl)
	{
		r = SSL_set_fd(ssl, fd);
		if(r != 1)
		{
			LOG_TRACE("SecureExchanger::Handshake, error=%d, info=%s", errno, strerror(errno));
			return false;
		}

		SSL_set_accept_state(ssl);

		is_init_ssl = true;
	}

	r = SSL_do_handshake(ssl);

	if(r != 1)
	{
		int error = SSL_get_error(ssl, r);
		if(error == SSL_ERROR_WANT_READ)
		{
			RegisterSendEvent();
			return true;
		}
		else if(error == SSL_ERROR_WANT_WRITE)
		{
			RegisterRecvEvent();
			return true;
		}

		LOG_TRACE("SecureExchanger::Handshake, error=%d, info=%s", errno, strerror(errno));
		return false;
	}

	is_handshake_finish = true;

	int error = SSL_get_error(ssl, r);

	LOG_TRACE("SecureExchanger::Handshake, finish, error=%d", error);

	return true;
}

/*
 
   177 void handleDataRead(Channel* ch) {
   178     char buf[4096];
   179     int rd = SSL_read(ch->ssl_, buf, sizeof buf);
   180     int ssle = SSL_get_error(ch->ssl_, rd);
   181     if (rd > 0) {
   182         const char* cont = "HTTP/1.1 200 OK\r\nConnection: Close\r\n\r\n";
   183         int len1 = strlen(cont);
   184         int wd = SSL_write(ch->ssl_, cont, len1);
   185         log("SSL_write %d bytes\n", wd);
   186         delete ch;
   187     }
   188     if (rd < 0 && ssle != SSL_ERROR_WANT_READ) {
   189         log("SSL_read return %d error %d errno %d msg %s", rd, ssle, errno, strerror(errno));
   190         delete ch;
   191         return;
   192     }
   193     if (rd == 0) {
   194         if (ssle == SSL_ERROR_ZERO_RETURN)
   195             log("SSL has been shutdown.\n");
   196         else
   197             log("Connection has been aborted.\n");
   198         delete ch;
   199     }
   200 }



 * */

void SecureExchanger::Recv()
{
	if(IsClose()) return;

	if(is_handshake_finish == false)
	{
		Handshake();
		return ;
	}

	int cnt = 0, per_cnt = 0, error = 0;
	do
	{
		while((per_cnt = SSL_read(ssl, tmp_buff, TMP_BUFF_SIZE)) > 0)
		{
			ibuff.insert(ibuff.end(), tmp_buff, per_cnt);
			cnt += per_cnt;
			if(per_cnt < TMP_BUFF_SIZE)
			{
				return ;
			}
		}
		error = SSL_get_error(ssl, per_cnt);
	}while(per_cnt < 0 && error == SSL_ERROR_WANT_READ);

	Close();

	LOG_ERROR("SecureExchanger::Recv, cnt=%d, error=%d, errno=%d, info=%s", cnt, error, errno, strerror(errno));
}


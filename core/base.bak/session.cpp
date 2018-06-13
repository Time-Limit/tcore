#include "session.h"
#include "channel.h"
#include "neter.h"
#include "thread.h"
#include "protocol.h"

using namespace TCORE;

Session::Session(int fd)
: sid(fd)
, exchanger(nullptr)
{
}

Session::~Session()
{
	exchanger = nullptr;
}

void Session::Close()
{
	manager->DelSession(this);
}

void Session::DataOut(const char *data, size_t size)
{
	exchanger->Send(data, size);
}

void Session::SetExchanger()
{
	exchanger = new Exchanger(ID(), this);
}

HttpSession::HttpSession(int fd)
: Session(fd)
{}

SessionManager::SessionManager(ProtocolHandler ph)
: protocol_handler(ph)
{}

SessionManager::SessionManager()
: SessionManager([](SessionManager *manager, session_id_t sid, Protocol& p) { p.Handle(manager, sid); })
{}

void SessionManager::DelSession(Session *session)
{
	if(session)
	{
		MutexGuard guarder(session_map_lock);
		SessionMap::iterator it = session_map.find(session->ID());

		if(it != session_map.end())
		{
			session_map.erase(it);
		}

		delete session;
	}
}

void SessionManager::Send(session_id_t sid, const char *data, size_t size)
{
	SessionMap::iterator it = session_map.find(sid);

	if(it == session_map.end())
	{
		return ;
	}

	it->second->DataOut(data, size);
}

void SessionManager::Send(session_id_t sid, const Protocol &p)
{
	OctetsStream os;
	os << p;
	Send(sid, (const char *)os.GetData().begin(), os.GetData().size());
}

void SessionManager::AddSession(Session *session)
{
	MutexGuard guard(session_map_lock);
	session->SetManager(this);
	session->SetExchanger();
	session_map.insert(std::make_pair(session->ID(), session));

	epoll_event ev;
	ev.events = EPOLLIN|EPOLLET;
	ev.data.ptr = session->GetExchanger();
	Log::Trace("SessionManager::Add, fd=", session->ID(), ", ip=", session->GetExchanger()->IP());
	Neter::GetInstance().Ctl(EPOLL_CTL_ADD, session->ID(), &ev);
}

SessionManager::~SessionManager()
{
	for(auto &it : session_map)
	{
		delete it.second;
		it.second = nullptr;
	}

	session_map.clear();
}

void HttpSession::OnDataIn()
{
	OctetsStream os(recv_data);
	HttpRequest *req = nullptr;
	try
	{
		for(;;)
		{
			req = new HttpRequest();
			os >> *req >> OctetsStream::COMMIT;
			ThreadPool::GetInstance().AddTask(new HandleNetProtocolTask(GetManager(), ID(), req));
			req = nullptr;
		}
	}
	catch(...)
	{
		os >> OctetsStream::REVERT;
		delete req;
	}
	recv_data = os.GetData();
}

void HttpSessionManager::OnConnect(int fd)
{
	HttpSession *session = new HttpSession(fd);
	AddSession(session);
}



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>   
#include <stdarg.h>    
#include <unistd.h>    
#include <sys/time.h>  
#include <time.h>      
#include <fcntl.h>     
#include <errno.h>     
#include <sys/ioctl.h> 
#include <arpa/inet.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"


CSocekt::CSocekt()
{
    m_worker_connections = 1;      
    m_ListenPortCount = 1;         
    m_RecyConnectionWaitTime = 60; 

    m_epollhandle = -1;          
    

    m_iLenPkgHeader = sizeof(COMM_PKG_HEADER);    
    m_iLenMsgHeader =  sizeof(STRUC_MSG_HEADER);  


    m_iSendMsgQueueCount     = 0;     
    m_totol_recyconnection_n = 0;     
    m_cur_size_              = 0;     
    m_timer_value_           = 0;     
    m_iDiscardSendPkgCount   = 0;     


    m_onlineUserCount        = 0;     
    m_lastprintTime          = 0;     
    return;
}


bool CSocekt::Initialize()
{
    ReadConf(); 
    if(ngx_open_listening_sockets() == false)  
        return false;
    return true;
}


bool CSocekt::Initialize_subproc()
{
    
    if(pthread_mutex_init(&m_sendMessageQueueMutex, NULL)  != 0)
    {
        ngx_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_mutex_init(&m_sendMessageQueueMutex)失败.");
        return false;
    }
    
    if(pthread_mutex_init(&m_connectionMutex, NULL)  != 0)
    {
        ngx_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_mutex_init(&m_connectionMutex)失败.");
        return false;
    }
 
    if(pthread_mutex_init(&m_recyconnqueueMutex, NULL)  != 0)
    {
        ngx_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_mutex_init(&m_recyconnqueueMutex)失败.");
        return false;
    }
   
    if(pthread_mutex_init(&m_timequeueMutex, NULL)  != 0)
    {
        ngx_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_mutex_init(&m_timequeueMutex)失败.");
        return false;
    }

    if(sem_init(&m_semEventSendQueue,0,0) == -1)
    {
        ngx_log_stderr(0,"CSocekt::Initialize_subproc()中sem_init(&m_semEventSendQueue,0,0)失败.");
        return false;
    }

    int err;
    ThreadItem *pSendQueue;   
    m_threadVector.push_back(pSendQueue = new ThreadItem(this));               
    err = pthread_create(&pSendQueue->_Handle, NULL, ServerSendQueueThread,pSendQueue); 
    if(err != 0)
    {
        ngx_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_create(ServerSendQueueThread)失败.");
        return false;
    }


    ThreadItem *pRecyconn;    
    m_threadVector.push_back(pRecyconn = new ThreadItem(this));
    err = pthread_create(&pRecyconn->_Handle, NULL, ServerRecyConnectionThread,pRecyconn);
    if(err != 0)
    {
        ngx_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_create(ServerRecyConnectionThread)失败.");
        return false;
    }

    if(m_ifkickTimeCount == 1)  
    {
        ThreadItem *pTimemonitor;  
        m_threadVector.push_back(pTimemonitor = new ThreadItem(this));
        err = pthread_create(&pTimemonitor->_Handle, NULL, ServerTimerQueueMonitorThread,pTimemonitor);
        if(err != 0)
        {
            ngx_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_create(ServerTimerQueueMonitorThread)失败.");
            return false;
        }
    }

    return true;
}


CSocekt::~CSocekt()
{

    std::vector<lpngx_listening_t>::iterator pos;
	for(pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos) 
	{
		delete (*pos); 
	}
	m_ListenSocketList.clear();
    return;
}

void CSocekt::Shutdown_subproc()
{
    if(sem_post(&m_semEventSendQueue)==-1)  
    {
         ngx_log_stderr(0,"CSocekt::Shutdown_subproc()中sem_post(&m_semEventSendQueue)失败.");
    }

    std::vector<ThreadItem*>::iterator iter;
	for(iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        pthread_join((*iter)->_Handle, NULL); 
    }

	for(iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
	{
		if(*iter)
			delete *iter;
	}
	m_threadVector.clear();

    
    clearMsgSendQueue();
    clearconnection();
    clearAllFromTimerQueue();


    pthread_mutex_destroy(&m_connectionMutex);          
    pthread_mutex_destroy(&m_sendMessageQueueMutex);    
    pthread_mutex_destroy(&m_recyconnqueueMutex);       
    pthread_mutex_destroy(&m_timequeueMutex);           
    sem_destroy(&m_semEventSendQueue);                  
}

void CSocekt::clearMsgSendQueue()
{
	char * sTmpMempoint;
	CMemory *p_memory = CMemory::GetInstance();

	while(!m_MsgSendQueue.empty())
	{
		sTmpMempoint = m_MsgSendQueue.front();
		m_MsgSendQueue.pop_front();
		p_memory->FreeMemory(sTmpMempoint);
	}
}

void CSocekt::ReadConf()
{
    CConfig *p_config = CConfig::GetInstance();
    m_worker_connections      = p_config->GetIntDefault("worker_connections",m_worker_connections);              
    m_ListenPortCount         = p_config->GetIntDefault("ListenPortCount",m_ListenPortCount);                    
    m_RecyConnectionWaitTime  = p_config->GetIntDefault("Sock_RecyConnectionWaitTime",m_RecyConnectionWaitTime); 

    m_ifkickTimeCount         = p_config->GetIntDefault("Sock_WaitTimeEnable",0);                                
	m_iWaitTime               = p_config->GetIntDefault("Sock_MaxWaitTime",m_iWaitTime);                         
	m_iWaitTime               = (m_iWaitTime > 5)?m_iWaitTime:5;                                                 
    m_ifTimeOutKick           = p_config->GetIntDefault("Sock_TimeOutKick",0);                                   用

    m_floodAkEnable          = p_config->GetIntDefault("Sock_FloodAttackKickEnable",0);                          
	m_floodTimeInterval      = p_config->GetIntDefault("Sock_FloodTimeInterval",100);                            
	m_floodKickCount         = p_config->GetIntDefault("Sock_FloodKickCounter",10);                              

    return;
}


bool CSocekt::ngx_open_listening_sockets()
{
    int                isock;                
    struct sockaddr_in serv_addr;            
    int                iport;                
    char               strinfo[100];         


    memset(&serv_addr,0,sizeof(serv_addr));  
    serv_addr.sin_family = AF_INET;          
  
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); 
    CConfig *p_config = CConfig::GetInstance();
    
    for(int i = 0; i < m_ListenPortCount; i++)
    {
        isock = socket(AF_INET,SOCK_STREAM,0);
        if(isock == -1)
        {
            ngx_log_stderr(errno,"CSocekt::Initialize()中socket()失败,i=%d.",i);
            return false;
        }

        int reuseaddr = 1;  
        if(setsockopt(isock, SOL_SOCKET, SO_REUSEADDR, (const void *) &reuseaddr, sizeof(reuseaddr)) == -1)
        {
            ngx_log_stderr(errno,"CSocekt::Initialize()中setsockopt(SO_REUSEADDR)失败,i=%d.",i);
            close(isock);

            return false;
        }
        int reuseport = 1;
        if (setsockopt(isock, SOL_SOCKET, SO_REUSEPORT, (const void *) &reuseport, sizeof(int))== -1) 
        {
            ngx_log_stderr(errno,"CSocekt::Initialize()中setsockopt(SO_REUSEPORT)失败",i);
        }

        if(setnonblocking(isock) == false)
        {
            ngx_log_stderr(errno,"CSocekt::Initialize()中setnonblocking()失败,i=%d.",i);
            close(isock);
            return false;
        }

        strinfo[0] = 0;
        sprintf(strinfo,"ListenPort%d", i);
        iport = p_config->GetIntDefault(strinfo,10000);
        
        serv_addr.sin_port = htons((in_port_t)iport);  

       
        if(bind(isock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
        {
            ngx_log_stderr(errno,"CSocekt::Initialize()中bind()失败,i=%d.",i);
            close(isock);
            return false;
        }

      
        if(listen(isock,NGX_LISTEN_BACKLOG) == -1)
        {
            ngx_log_stderr(errno,"CSocekt::Initialize()中listen()失败,i=%d.",i);
            close(isock);
            return false;
        }

       
        lpngx_listening_t p_listensocketitem = new ngx_listening_t;
        memset(p_listensocketitem,0,sizeof(ngx_listening_t));      
        p_listensocketitem->port = iport;                          
        p_listensocketitem->fd   = isock;                          

        ngx_log_error_core(NGX_LOG_INFO,0,"监听%d端口成功!",iport);
        m_ListenSocketList.push_back(p_listensocketitem);      
    } 
    if(m_ListenSocketList.size() <= 0)
        return false;
    return true;
}


bool CSocekt::setnonblocking(int sockfd)
{
    int nb=1; 
    if(ioctl(sockfd, FIONBIO, &nb) == -1) 
    {
        return false;
    }
    return true;
}

void CSocekt::ngx_close_listening_sockets()
{
    for(int i = 0; i < m_ListenPortCount; i++) 
    {
        close(m_ListenSocketList[i]->fd);
        ngx_log_error_core(NGX_LOG_INFO,0,"关闭监听端口%d!",m_ListenSocketList[i]->port);
    }
    return;
}

void CSocekt::msgSend(char *psendbuf) 
{
    CMemory *p_memory = CMemory::GetInstance();

    CLock lock(&m_sendMessageQueueMutex); 

  
    if(m_iSendMsgQueueCount > 50000)
    {
        m_iDiscardSendPkgCount++;
        p_memory->FreeMemory(psendbuf);
		return;
    }

    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)psendbuf;
	lpngx_connection_t p_Conn = pMsgHeader->pConn;
    if(p_Conn->iSendCount > 400)
    {
        ngx_log_stderr(0,"CSocekt::msgSend()中发现某用户%d积压了大量待发送数据包，切断与他的连接！",p_Conn->fd);
        m_iDiscardSendPkgCount++;
        p_memory->FreeMemory(psendbuf);
        zdClosesocketProc(p_Conn);
		return;
    }

    ++p_Conn->iSendCount;
    m_MsgSendQueue.push_back(psendbuf);
    ++m_iSendMsgQueueCount;   

 
    if(sem_post(&m_semEventSendQueue)==-1)  
    {
        ngx_log_stderr(0,"CSocekt::msgSend()中sem_post(&m_semEventSendQueue)失败.");
    }
    return;
}


void CSocekt::zdClosesocketProc(lpngx_connection_t p_Conn)
{
    if(m_ifkickTimeCount == 1)
    {
        DeleteFromTimerQueue(p_Conn); 
    }

    if(p_Conn->fd != -1)
    {
        close(p_Conn->fd);
        p_Conn->fd = -1;
    }

    if(p_Conn->iThrowsendCount > 0)
        --p_Conn->iThrowsendCount;   

    inRecyConnectQueue(p_Conn);
    return;
}

bool CSocekt::TestFlood(lpngx_connection_t pConn)
{
    struct  timeval sCurrTime;   
	uint64_t        iCurrTime;  
	bool  reco      = false;

	gettimeofday(&sCurrTime, NULL); 
    iCurrTime =  (sCurrTime.tv_sec * 1000 + sCurrTime.tv_usec / 1000);
	if((iCurrTime - pConn->FloodkickLastTime) < m_floodTimeInterval)  
	{
		pConn->FloodAttackCount++;
		pConn->FloodkickLastTime = iCurrTime;
	}
	else
	{
		pConn->FloodAttackCount = 0;
		pConn->FloodkickLastTime = iCurrTime;
	}
	if(pConn->FloodAttackCount >= m_floodKickCount)
	{
		reco = true;
	}
	return reco;
}

void CSocekt::printTDInfo()
{
    time_t currtime = time(NULL);
    if( (currtime - m_lastprintTime) > 10)
    {
        int tmprmqc = g_threadpool.getRecvMsgQueueCount();

        m_lastprintTime = currtime;
        int tmpoLUC = m_onlineUserCount;    
        int tmpsmqc = m_iSendMsgQueueCount; 
        ngx_log_stderr(0,"------------------------------------begin--------------------------------------");
        ngx_log_stderr(0,"当前在线人数/总人数(%d/%d)。",tmpoLUC,m_worker_connections);
        ngx_log_stderr(0,"连接池中空闲连接/总连接/要释放的连接(%d/%d/%d)。",m_freeconnectionList.size(),m_connectionList.size(),m_recyconnectionList.size());
        ngx_log_stderr(0,"当前时间队列大小(%d)。",m_timerQueuemap.size());
        ngx_log_stderr(0,"当前收消息队列/发消息队列大小分别为(%d/%d)，丢弃的待发送数据包数量为%d。",tmprmqc,tmpsmqc,m_iDiscardSendPkgCount);
        if( tmprmqc > 100000)
        {
            ngx_log_stderr(0,"接收队列条目数量过大(%d)，要考虑限速或者增加处理线程数量了！！！！！！",tmprmqc);
        }
        ngx_log_stderr(0,"-------------------------------------end---------------------------------------");
    }
    return;
}


int CSocekt::ngx_epoll_init()
{
    m_epollhandle = epoll_create(m_worker_connections);  
    if (m_epollhandle == -1)
    {
        ngx_log_stderr(errno,"CSocekt::ngx_epoll_init()中epoll_create()失败.");
        exit(2); 
    }
    initconnection();
   
    std::vector<lpngx_listening_t>::iterator pos;
	for(pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        lpngx_connection_t p_Conn = ngx_get_connection((*pos)->fd);
        if (p_Conn == NULL)
        {
            
            ngx_log_stderr(errno,"CSocekt::ngx_epoll_init()中ngx_get_connection()失败.");
            exit(2); 
        }
        
        p_Conn->listening = (*pos);   
        (*pos)->connection = p_Conn;  

        
        p_Conn->rhandler = &CSocekt::ngx_event_accept;
        if(ngx_epoll_oper_event(
                                (*pos)->fd,         
                                EPOLL_CTL_ADD,      
                                EPOLLIN|EPOLLRDHUP, 
                                0,                  
                                p_Conn              
                                ) == -1)
        {
            exit(2); 
        }
    } 
    return 1;
}

/*
//epoll增加事件，可能被ngx_epoll_init()等函数调用
//fd:句柄，一个socket
//readevent：表示是否是个读事件，0是，1不是
//writeevent：表示是否是个写事件，0是，1不是
//otherflag：其他需要额外补充的标记，弄到这里
//eventtype：事件类型  ，一般就是用系统的枚举值，增加，删除，修改等;
//c：对应的连接池中的连接的指针
//返回值：成功返回1，失败返回-1；
int CSocekt::ngx_epoll_add_event(int fd,
                                int readevent,int writeevent,
                                uint32_t otherflag,
                                uint32_t eventtype,
                                lpngx_connection_t pConn
                                )
{
    struct epoll_event ev;
    //int op;
    memset(&ev, 0, sizeof(ev));

    if(readevent==1)
    {
        //读事件，这里发现官方nginx没有使用EPOLLERR，因此我们也不用【有些范例中是使用EPOLLERR的】
        ev.events = EPOLLIN|EPOLLRDHUP; //EPOLLIN读事件，也就是read ready【客户端三次握手连接进来，也属于一种可读事件】   EPOLLRDHUP 客户端关闭连接，断连
                                          //似乎不用加EPOLLERR，只用EPOLLRDHUP即可，EPOLLERR/EPOLLRDHUP 实际上是通过触发读写事件进行读写操作recv write来检测连接异常

        //ev.events |= (ev.events | EPOLLET);  //只支持非阻塞socket的高速模式【ET：边缘触发】，就拿accetp来说，如果加这个EPOLLET，则客户端连入时，epoll_wait()只会返回一次该事件，
                    //如果用的是EPOLLLT【水平触发：低速模式】，则客户端连入时，epoll_wait()会被触发多次，一直到用accept()来处理；



        //https://blog.csdn.net/q576709166/article/details/8649911
        //找下EPOLLERR的一些说法：
        //a)对端正常关闭（程序里close()，shell下kill或ctr+c），触发EPOLLIN和EPOLLRDHUP，但是不触发EPOLLERR 和EPOLLHUP。
        //b)EPOLLRDHUP    这个好像有些系统检测不到，可以使用EPOLLIN，read返回0，删除掉事件，关闭close(fd);如果有EPOLLRDHUP，检测它就可以直到是对方关闭；否则就用上面方法。
        //c)client 端close()联接,server 会报某个sockfd可读，即epollin来临,然后recv一下 ， 如果返回0再掉用epoll_ctl 中的EPOLL_CTL_DEL , 同时close(sockfd)。
                //有些系统会收到一个EPOLLRDHUP，当然检测这个是最好不过了。只可惜是有些系统，上面的方法最保险；如果能加上对EPOLLRDHUP的处理那就是万能的了。
        //d)EPOLLERR      只有采取动作时，才能知道是否对方异常。即对方突然断掉，是不可能有此事件发生的。只有自己采取动作（当然自己此刻也不知道），read，write时，出EPOLLERR错，说明对方已经异常断开。
        //e)EPOLLERR 是服务器这边出错（自己出错当然能检测到，对方出错你咋能知道啊）
        //f)给已经关闭的socket写时，会发生EPOLLERR，也就是说，只有在采取行动（比如读一个已经关闭的socket，或者写一个已经关闭的socket）时候，才知道对方是否关闭了。
                //这个时候，如果对方异常关闭了，则会出现EPOLLERR，出现Error把对方DEL掉，close就可以了。
    }
    if(writeevent==1)
    {
        //写事件
        ev.events = EPOLLOUT;
        //保留以往的读事件，读事件是肯定要在【一直有的】，你不能说后续增加了一个写事件，就把原来的读事件覆盖掉了
        ev.events |= (EPOLLIN|EPOLLRDHUP); //把以往的读事件弄进来
    }

    if(otherflag != 0)
    {
        ev.events |= otherflag;
    }

    //以下这段代码抄自nginx官方,因为指针的最后一位【二进制位】肯定不是1，所以 和 c->instance做 |运算；到时候通过一些编码，既可以取得c的真实地址，又可以把此时此刻的c->instance值取到
    //比如c是个地址，可能的值是 0x00af0578，对应的二进制是‭101011110000010101111000‬，而 | 1后是0x00af0579
    //ev.data.ptr = (void *)( (uintptr_t)c | c->instance);   //把对象弄进去，后续来事件时，用epoll_wait()后，这个对象能取出来用
                                                             //但同时把一个 标志位【不是0就是1】弄进去

    if(eventtype == EPOLL_CTL_ADD)
    {
        //如果事件类型是
        ev.data.ptr = (void *)pConn;
    }

    if(epoll_ctl(m_epollhandle,eventtype,fd,&ev) == -1)
    {
        ngx_log_stderr(errno,"CSocekt::ngx_epoll_add_event()中epoll_ctl(%d,%d,%d,%ud,%ud)失败.",fd,readevent,writeevent,otherflag,eventtype);
        //exit(2); //这是致命问题了，直接退，资源由系统释放吧，这里不刻意释放了，比较麻烦，后来发现不能直接退；
        return -1;
    }
    return 1;
}
*/

//对epoll事件的具体操作  个人理解本质上就是使用epoll_ctl函数 想象下 这个函数肯定只有两个地方会被调用 监听套接字和通信套接字
//返回值：成功返回1，失败返回-1；
int CSocekt::ngx_epoll_oper_event(
                        int                fd,               //句柄，一个socket
                        uint32_t           eventtype,        //事件类型，一般是EPOLL_CTL_ADD，EPOLL_CTL_MOD，EPOLL_CTL_DEL ，说白了就是操作epoll红黑树的节点(增加，修改，删除)
                        uint32_t           flag,             //标志，具体含义取决于eventtype
                        int                bcaction,         //补充动作，用于补充flag标记的不足  :  0：增加   1：去掉 2：完全覆盖 ,eventtype是EPOLL_CTL_MOD时这个参数就有用
                        lpngx_connection_t pConn             //pConn：一个指针【其实是一个连接】，EPOLL_CTL_ADD时增加到红黑树中去，将来epoll_wait时能取出来用
                        )
//连接断开之后，系统会自动帮我们删EPOLL_CTL_DEL
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    if(eventtype == EPOLL_CTL_ADD) //往红黑树中增加节点；
    {
        //红黑树从无到有增加节点
        //ev.data.ptr = (void *)pConn;//p331因为这个地方会引起core dump 所以移动到下面去
        ev.events = flag;      //既然是增加节点，则不管原来是啥标记
        pConn->events = flag;  //这个连接本身也记录这个标记
    }
    else if(eventtype == EPOLL_CTL_MOD)//如果需要往外发的话 就要增加修改节点的操作  比如发送消息给客户端的时候
    {
        //节点已经在红黑树中，修改节点的事件信息
        ev.events = pConn->events;  //先把标记恢复回来
        if(bcaction == 0)//补充flag标记的不足  :  0：增加
        {
            //增加某个标记
            ev.events |= flag;
        }
        else if(bcaction == 1)
        {
            //去掉某个标记
            ev.events &= ~flag;
        }
        else
        {
            //完全覆盖某个标记
            ev.events = flag;      //完全覆盖
        }
        pConn->events = ev.events; //记录该标记
    }
    else
    {
        //删除红黑树中节点，目前没这个需求【socket关闭这项会自动从红黑树移除】，所以将来再扩展
        return  1;  //先直接返回1表示成功
    }

    //原来的理解中，绑定ptr这个事，只在EPOLL_CTL_ADD的时候做一次即可，但是发现EPOLL_CTL_MOD似乎会破坏掉.data.ptr，因此不管是EPOLL_CTL_ADD，还是EPOLL_CTL_MOD，都给进去
    //找了下内核源码SYSCALL_DEFINE4(epoll_ctl, int, epfd, int, op, int, fd,		struct epoll_event __user *, event)，感觉真的会覆盖掉：
       //copy_from_user(&epds, event, sizeof(struct epoll_event)))，感觉这个内核处理这个事情太粗暴了
    ev.data.ptr = (void *)pConn;// epoll_event的第二个参数 data 怎么用,ev.data其实是一个union类型 一般传入的就是监听套接字fd 也可以传入void* ptr  p331
    //将来来事件的时候 就可以通过ptr把这块内存拿到

    if(epoll_ctl(m_epollhandle,eventtype,fd,&ev) == -1)
    {
        ngx_log_stderr(errno,"CSocekt::ngx_epoll_oper_event()中epoll_ctl(%d,%ud,%ud,%d)失败.",fd,eventtype,flag,bcaction);
        return -1;
    }
    return 1;
}

//开始获取发生的事件消息
//参数unsigned int timer：epoll_wait()阻塞的时长，单位是毫秒；
//返回值，1：正常返回  ,0：有问题返回，一般不管是正常还是问题返回，都应该保持进程继续运行
//本函数被ngx_process_events_and_timers()调用，而ngx_process_events_and_timers()是在子进程的死循环!!中被反复调用  在worker函数中使用
int CSocekt::ngx_epoll_process_events(int timer) //代码中传的是-1 那就是一直阻塞
{
    //等待事件，事件会返回到m_events里，最多返回NGX_MAX_EVENTS个事件【因为我只提供了这些内存】；
    //如果两次调用epoll_wait()的事件间隔比较长，则可能在epoll的双向链表中，积累了多个事件，所以调用epoll_wait，可能取到多个事件
    //阻塞timer这么长时间除非：a)阻塞时间到达 b)阻塞期间收到事件【比如新用户连入】会立刻返回c)调用时有事件也会立刻返回d)如果来个信号，比如你用kill -1 pid测试 TODO
    //如果timer为-1则一直阻塞，如果timer为0则立即返回，即便没有任何事件
    //返回值：有错误发生返回-1，错误在errno中，比如你发个信号过来，就返回-1，错误信息是(4: Interrupted system call)
    //       如果你等待的是一段时间，并且超时了，则返回0；
    //       如果返回>0则表示成功捕获到这么多个事件【返回值里】
    int events = epoll_wait(m_epollhandle,m_events,NGX_MAX_EVENTS,timer);//NGX_MAX_EVENTS 这边定义为512
    //这里return不同的值感觉并没什么用 因为这个函数一直是死循环的  当这里为-1的时候 就直接返回了
    if(events == -1)
    {
        //有错误发生，发送某个信号给本进程就可以导致这个条件成立，而且错误码根据观察是4；
        //#define EINTR  4，EINTR错误的产生：当阻塞于某个慢系统调用的一个进程捕获某个信号且相应信号处理函数返回时，该系统调用可能返回一个EINTR错误。
               //例如：在socket服务器端，设置了信号捕获机制，有子进程，当在父进程阻塞于慢系统调用时由父进程捕获到了一个有效信号时，内核会致使accept返回一个EINTR错误(被中断的系统调用)。
        //peter可优化 可以提供代码健壮性
        //一般worker进程不会收到信号，除非master进程给worker进程发信号
        if(errno == EINTR) //  server_process.c 4_20文件夹中 //read函数在阻塞的时候 被中断 返回的时候就不是阻塞的了 而是返回-1 EINTR
        {
            //d)如果来个信号，比如你用kill -1 pid  就会使得epoll_wait不被阻塞 而且这种情况events == -1

            //信号所致，直接返回，一般认为这不是毛病，但还是打印下日志记录一下，因为一般也不会人为给worker进程发送消息
            ngx_log_error_core(NGX_LOG_INFO,errno,"CSocekt::ngx_epoll_process_events()中epoll_wait()失败!");
            return 1;  //正常返回 不处理
        }
        else
        {
            //这被认为应该是有问题，记录日志
            ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocekt::ngx_epoll_process_events()中epoll_wait()失败!");
            return 0;  //非正常返回
        }
    }
    //timer == -1  也会走这里面来？-->  无限等待-1 却来了一个超时的信息，说明这里就有问题
    if(events == 0) //超时，但没事件来
    {
        if(timer != -1)
        {
            //要求epoll_wait阻塞一定的时间而不是一直阻塞，这属于阻塞到时间了，则正常返回
            return 1;
        }
        //无限等待timer == -1【所以不存在超时】，但却没返回任何事件，这应该不正常有问题
        ngx_log_error_core(NGX_LOG_ALERT,0,"CSocekt::ngx_epoll_process_events()中epoll_wait()没超时却没返回任何事件!");
        return 0; //非正常返回
    }

    //会惊群，一个telnet上来，4个worker进程都会被惊动，都执行下边这个
    //ngx_log_stderr(0,"惊群测试:events=%d,进程id=%d",events,ngx_pid);   //产生惊群现象的打印 talent进来之后 会发现有四个打印  是来自不同的进程pid
    //ngx_log_stderr(0,"----------------------------------------");

    //走到这里，就是属于有事件收到了  上面其实都是异常处理
    lpngx_connection_t p_Conn;
    //uintptr_t          instance;
    uint32_t           revents;
    for(int i = 0; i < events; ++i)    //遍历本次epoll_wait返回的所有事件，注意events才是返回的实际事件数量 所以可以通过它来遍历！
    {
    // ev.data.ptr = (void *)pConn;
        p_Conn = (lpngx_connection_t)(m_events[i].data.ptr);           //ngx_epoll_add_event()给进去的，这里能取出来  , 把连接池中的内存地址取出

        /*
        instance = (uintptr_t) c & 1;                             //将地址的最后一位取出来，用instance变量标识, 见ngx_epoll_add_event，该值是当时随着连接池中的连接一起给进来的
                                                                  //取得的是你当时调用ngx_epoll_add_event()的时候，这个连接里边的instance变量的值；
        p_Conn = (lpngx_connection_t) ((uintptr_t)p_Conn & (uintptr_t) ~1); //最后1位干掉，得到真正的c地址

        //仔细分析一下官方nginx的这个判断
        //过滤过期事件的；
        if(c->fd == -1)  //一个套接字，当关联一个 连接池中的连接【对象】时，这个套接字值是要给到c->fd的，
                           //那什么时候这个c->fd会变成-1呢？关闭连接时这个fd会被设置为-1，哪行代码设置的-1再研究，但应该不是ngx_free_connection()函数设置的-1
        {
            //case1:
            //比如我们用epoll_wait取得三个事件，处理第一个事件时，因为业务需要，我们把这个连接关闭，那我们应该会把c->fd设置为-1； void CSocekt::ngx_close_listening_sockets()
            //第二个事件照常处理
            //第三个事件，假如这第三个事件，也跟第一个事件对应的是同一个连接，那这个条件就会成立；那么这种事件，属于过期事件，不该处理

            //第一个和第三个对应得是同一个通信套接字对应的事件，是这个意思吗?
            //这里的第一个事件是fd:EPOLLIN
            //这里的第三个事件是fd:EPOLLOUT
            


            //这里可以增加个日志，也可以不增加日志
            ngx_log_error_core(NGX_LOG_DEBUG,0,"CSocekt::ngx_epoll_process_events()中遇到了fd=-1的过期事件:%p.",c);
            continue; //这种事件就不处理即可
        }

        //过滤过期事件的； 90分钟左右 https://study.163.com/course/courseLearn.htm?courseId=1006470001&share=1&shareId=1444225233#/learn/video?lessonId=1278545656&courseId=1006470001
        if(c->instance != instance)//都会进行取反操作
        {
            //--------------------以下这些说法来自于资料--------------------------------------
            //什么时候这个条件成立呢？【换种问法：instance标志为什么可以判断事件是否过期呢？】
            //比如我们用epoll_wait取得三个事件，处理第一个事件时，因为业务需要，我们把这个连接关闭【麻烦就麻烦在这个连接被服务器关闭上了】，但是恰好第三个事件也跟这个连接有关；
            //因为第一个事件就把socket连接关闭了，显然第三个事件我们是不应该处理的【因为这是个过期事件】，若处理肯定会导致错误；
            //那我们上述把c->fd设置为-1，可以解决这个问题吗？ 能解决一部分问题，但另外一部分不能解决，不能解决的问题描述如下【这么离奇的情况应该极少遇到】：

            //case2:
            //a)处理第一个事件时，因为业务需要，我们把这个连接【假设套接字为50】关闭，同时设置c->fd = -1;并且调用ngx_free_connection将该连接归还给连接池；
            //b)处理第二个事件，恰好第二个事件是建立新连接事件，调用ngx_get_connection从连接池中取出的连接非常可能就是刚刚释放的第一个事件对应的连接池中的连接；
            //c)又因为a中套接字50被释放了，所以会被操作系统拿来复用，复用给了b)【一般这么快就被复用也是醉了】；
            //d)当处理第三个事件时，第三个事件其实是已经过期的，应该不处理，那怎么判断这第三个事件是过期的呢？ 【假设现在处理的是第三个事件，此时这个 连接池中的该连接 实际上已经被用作第二个事件对应的socket上了】；
                //依靠instance标志位能够解决这个问题，当调用ngx_get_connection从连接池中获取一个新连接时，我们把instance标志位置反，所以这个条件如果不成立，说明这个连接已经被挪作他用了；

            //--------------------我的个人思考--------------------------------------
            //如果收到了若干个事件，其中连接关闭也搞了多次，导致这个instance标志位被取反2次，那么，造成的结果就是：还是有可能遇到某些过期事件没有被发现【这里也就没有被continue】，照旧被当做没过期事件处理了；
                  //如果是这样，那就只能被照旧处理了。可能会造成偶尔某个连接被误关闭？但是整体服务器程序运行应该是平稳，问题不大的，这种漏网而被当成没过期来处理的的过期事件应该是极少发生的

            ngx_log_error_core(NGX_LOG_DEBUG,0,"CSocekt::ngx_epoll_process_events()中遇到了instance值改变的过期事件:%p.",c);
            continue; //这种事件就不处理即可
        }
        //存在一种可能性，过期事件没被过滤完整【非常极端】，走下来的； 连续两次取反就恢复了 就成了漏网之鱼
        */

        //能走到这里，我们认为这些事件都没过期，就正常开始处理
        revents = m_events[i].events;//取出事件类型

        /*
        if(revents & (EPOLLERR|EPOLLHUP)) //例如对方close掉套接字，这里会感应到【换句话说：如果发生了错误或者客户端断连】
        {
            //这加上读写标记，方便后续代码处理，至于怎么处理，后续再说，这里也是参照nginx官方代码引入的这段代码；
            //官方说法：if the error events were returned, add EPOLLIN and EPOLLOUT，to handle the events at least in one active handler
            //我认为官方也是经过反复思考才加上着东西的，先放这里放着吧；
            revents |= EPOLLIN|EPOLLOUT;   //EPOLLIN：表示对应的链接上有数据可以读出（TCP链接的远端主动关闭连接，也相当于可读事件，因为本服务器小处理发送来的FIN包）
                                           //EPOLLOUT：表示对应的连接上可以写入数据发送【写准备好】
        } */

        if(revents & EPOLLIN)  //如果是读事件 三次握手进来
        {
            //ngx_log_stderr(errno,"数据来了来了来了 ~~~~~~~~~~~~~.");
            //一个客户端新连入，这个会成立，
            //已连接发送数据来，这个也成立；
            //c->r_ready = 1;                         //标记可以读；【从连接池拿出一个连接时这个连接的所有成员都是0】
            //int CSocekt::ngx_epoll_init() 这个函数中有对变量rhandler的赋值   p_Conn->rhandler = &CSocekt::ngx_event_accept;
            (this->* (p_Conn->rhandler))(p_Conn);    //注意括号的运用来正确设置优先级，防止编译出错；【如果是个新客户连入
                                                      //如果新连接进入，这里执行的应该是CSocekt::ngx_event_accept(c)】
                                                      //如果是已经连入，发送数据到这里，则这里执行的应该是 CSocekt::ngx_read_request_handler()

        }
        //TODO 如果有写事件的话 这个地方应该怎么理解 TODO   (this->* (p_Conn->whandler) )(p_Conn);
        if(revents & EPOLLOUT) //如果是写事件【对方关闭连接也触发这个，再研究。。。。。。】，注意上边的 if(revents & (EPOLLERR|EPOLLHUP))  revents |= EPOLLIN|EPOLLOUT; 读写标记都给加上了
        {
            //ngx_log_stderr(errno,"22222222222222222222.");
            if(revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) //客户端关闭，如果服务器端挂着一个写通知事件，则这里个条件是可能成立的
            {
                //EPOLLERR：对应的连接发生错误                     8     = 1000
                //EPOLLHUP：对应的连接被挂起                       16    = 0001 0000
                //EPOLLRDHUP：表示TCP连接的远端关闭或者半关闭连接   8192   = 0010  0000   0000   0000
                //我想打印一下日志看一下是否会出现这种情况
                //8221 = ‭0010 0000 0001 1101‬  ：包括 EPOLLRDHUP ，EPOLLHUP， EPOLLERR
                //ngx_log_stderr(errno,"CSocekt::ngx_epoll_process_events()中revents&EPOLLOUT成立并且revents & (EPOLLERR|EPOLLHUP|EPOLLRDHUP)成立,event=%ud。",revents);

                //我们只有投递了 写事件，但对端断开时，程序流程才走到这里，投递了写事件意味着 iThrowsendCount标记肯定被+1了，这里我们减回
                --p_Conn->iThrowsendCount;
            }
            else
            {
                (this->* (p_Conn->whandler) )(p_Conn);   //如果有数据没有发送完毕，由系统驱动来发送，则这里执行的应该是 CSocekt::ngx_write_request_handler()
            }
        }
    } //end for(int i = 0; i < events; ++i)
    return 1;
}

//--------------------------------------------------------------------
//处理发送消息队列的线程  创建一个线程来统一处理负责数据发送   void CSocekt::msgSend(char *psendbuf) // m_MsgSendQueue.push_back(psendbuf);
//有点不明白 这个m_MsgSendQueue是这个类的成员变量 可以通过类的成员函数(线程入口函数)访问成员变量 达到访问这个线程可以访问成员变量的目的  TODO
void* CSocekt::ServerSendQueueThread(void* threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CSocekt *pSocketObj = pThread->_pThis;
    int err;
    std::list <char *>::iterator pos,pos2,posend;

    char *pMsgBuf;
    LPSTRUC_MSG_HEADER	pMsgHeader;
	LPCOMM_PKG_HEADER   pPkgHeader;
    lpngx_connection_t  p_Conn;
    unsigned short      itmp;
    ssize_t             sendsize;

    CMemory *p_memory = CMemory::GetInstance();

    while(g_stopEvent == 0) //不退出
    {
        //如果信号量值>0，则 -1(减1) 并走下去，否则卡这里卡着【为了让信号量值+1，可以在其他线程调用sem_post达到，实际上在CSocekt::msgSend()调用sem_post就达到了让这里sem_wait走下去的目的】
        //******如果被某个信号中断，sem_wait也可能过早的返回，错误为EINTR；
        //整个程序退出之前，也要sem_post()一下，确保如果本线程卡在sem_wait()，也能走下去从而让本线程成功返回
        if(sem_wait(&pSocketObj->m_semEventSendQueue) == -1)
        {
            //失败？及时报告，其他的也不好干啥
            if(errno != EINTR) //这个我就不算个错误了【当阻塞于某个慢系统调用的一个进程捕获某个信号且相应信号处理函数返回时，该系统调用可能返回一个EINTR错误。】
                ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()中sem_wait(&pSocketObj->m_semEventSendQueue)失败.");
        }

        //一般走到这里都表示需要处理数据收发了
        if(g_stopEvent != 0)  //要求整个进程退出
            break;

        if(pSocketObj->m_iSendMsgQueueCount > 0) //原子的  msgsend的时候 m_iSendMsgQueueCount+1了
        {
            err = pthread_mutex_lock(&pSocketObj->m_sendMessageQueueMutex); //因为我们要操作发送消息对列m_MsgSendQueue，所以这里要临界
            if(err != 0) ngx_log_stderr(err,"CSocekt::ServerSendQueueThread()中pthread_mutex_lock()失败，返回的错误码为%d!",err);

            pos    = pSocketObj->m_MsgSendQueue.begin();
			posend = pSocketObj->m_MsgSendQueue.end();

            while(pos != posend)//遍历一次队列 把当前消息队列中需要发的消息都处理了一遍  能发的就发出去，发不出去的就continue
            {
                pMsgBuf = (*pos);                          //拿到的每个消息都是 消息头+包头+包体【但要注意，我们是不发送消息头给客户端的】
                pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;  //指向消息头
                pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf+pSocketObj->m_iLenMsgHeader);	//指向包头
                p_Conn = pMsgHeader->pConn;

                //包过期，因为如果 这个连接被回收，比如在ngx_close_connection(),inRecyConnectQueue()中都会自增iCurrsequence
                     //而且这里有没必要针对 本连接 来用m_connectionMutex临界 ,只要下面条件成立，肯定是客户端连接已断，要发送的数据肯定不需要发送了
                if(p_Conn->iCurrsequence != pMsgHeader->iCurrsequence) //为啥要消息头 就是这里用于判断是否过期用的  很多地方用这个
                //全局搜这个 iCurrsequence ! 会发现很多地方调用了这个
                {
                    //本包中保存的序列号与p_Conn【连接池中连接】中实际的序列号已经不同，丢弃此消息，小心处理该消息的删除
                    //没必要再去发送
                    pos2=pos;//处理迭代器失效
                    pos++;
                    pSocketObj->m_MsgSendQueue.erase(pos2);//所以不用push_front
                    --pSocketObj->m_iSendMsgQueueCount; //发送消息队列容量少1
                    p_memory->FreeMemory(pMsgBuf);
                    continue;
                } //end if

                //一种特殊场景的过滤 当A发给服务器B时,B 正要回两条数据给A，但是A已经断线了，
                //那么当第二次想再发给A的时候  B发第一条就发不出去，缓冲区已满，但是p_Conn->iThrowsendCount已经++
                //那么当第二次想再发给A的时候 就应该ingnore
                if(p_Conn->iThrowsendCount > 0) //iThrowsendCount  刚开始这个条件为0  当大于0的时候 就说明了 就不能删除这一条 跳过处理下一条  这一条还在epoll wait里面
                {
                    //靠系统驱动来发送消息，所以这里不能再发送
                    //消息队列某一条没发完 还在epoll中
                    pos++;
                    continue;
                }

                --p_Conn->iSendCount;   //发送队列中有的数据条目数-1；

                //走到这里，可以发送消息，一些必须的信息记录，要发送的东西也要从发送队列里干掉  peter
                p_Conn->psendMemPointer = pMsgBuf;      //发送后释放用的，因为这段内存是new出来的
                pos2=pos;
				pos++;
                pSocketObj->m_MsgSendQueue.erase(pos2);
                --pSocketObj->m_iSendMsgQueueCount;      //发送消息队列容量少1
                p_Conn->psendbuf = (char *)pPkgHeader;   //要发送的数据的缓冲区指针，因为发送数据不一定全部都能发送出去，我们要记录数据发送到了哪里，需要知道下次数据从哪里开始发送
                itmp = ntohs(pPkgHeader->pkgLen);        //包头+包体 长度 ，打包时用了htons【本机序转网络序】，所以这里为了得到该数值，用了个ntohs【网络序转本机序】；
                p_Conn->isendlen = itmp;                 //要发送多少数据，因为发送数据不一定全部都能发送出去，我们需要知道剩余有多少数据还没发送

                //这里是重点，我们采用 epoll水平触发的策略，能走到这里的，都应该是还没有投递 写事件 到epoll中
                    //epoll水平触发发送数据的改进方案：
	                //开始不把socket写事件通知加入到epoll,当我需要写数据的时候，直接调用write/send发送数据；
	                //如果返回了EAGIN【发送缓冲区满了，需要等待可写事件才能继续往缓冲区里写数据】，此时，我再把写事件通知加入到epoll，
	                //此时，就变成了在epoll驱动下写数据，全部数据发送完毕后，再把写事件通知从epoll中干掉；
	                //优点：数据不多的时候，可以避免epoll的写事件的增加/删除，提高了程序的执行效率；
                //(1)直接调用write或者send发送数据
                //ngx_log_stderr(errno,"即将发送数据%ud。",p_Conn->isendlen);

                sendsize = pSocketObj->sendproc(p_Conn,p_Conn->psendbuf,p_Conn->isendlen); //注意参数  通过这个函数pSocketObj->sendproc 进行发送
                if(sendsize > 0)
                {
                    if(sendsize == p_Conn->isendlen) //成功发送出去了数据，一下就发送出去这很顺利
                    {
                        //成功发送的和要求发送的数据相等，说明全部发送成功了 发送缓冲区去了【数据全部发完】
                        p_memory->FreeMemory(p_Conn->psendMemPointer);  //释放内存
                        p_Conn->psendMemPointer = NULL;
                        p_Conn->iThrowsendCount = 0;  //这行其实可以没有，因此此时此刻这东西就是=0的
                        //ngx_log_stderr(0,"CSocekt::ServerSendQueueThread()中数据发送完毕，很好。"); //做个提示吧，商用时可以干掉
                    }
                    //但是这个地方为什么没有EAGAIN状态标志位的判断
                    //n < size 没发送完毕，那肯定是发送缓冲区满了，所以也不必要重试发送，直接返回吧
                    else  //没有全部发送完毕(EAGAIN)，数据只发出去了一部分！！，但肯定是因为 发送缓冲区满了,那么
                    {
                        //发送到了哪里，剩余多少，记录下来，方便下次sendproc()时使用
                        p_Conn->psendbuf = p_Conn->psendbuf + sendsize;
				        p_Conn->isendlen = p_Conn->isendlen - sendsize;
                        //因为发送缓冲区满了，所以 现在我要依赖系统通知来发送数据了
                        //原来是0
                        ++p_Conn->iThrowsendCount;             //标记发送缓冲区满了，需要通过epoll事件来驱动消息的继续发送【原子+1，且不可写成p_Conn->iThrowsendCount = p_Conn->iThrowsendCount +1 ，这种写法不是原子+1】
                        //投递此事件后，我们将依靠epoll驱动调用ngx_write_request_handler()函数发送数据
                        //一开始是主动发数据，但是当发送缓冲区满的时候 ，就要通过事件系统驱动我们继续发事件
                        // }
                        //当发送缓冲区满，往epoll投递写通知， 当把EPOLLOUT 就入到红黑树以后，那边就会增加对可写事件的通知  ---> 那么当可以继续发送时,epoll就会通知你 ---> epoll通知你的时候，就可以调用whandler
                        //TODO 如果有写事件的话 这个地方应该怎么理解 TODO   (this->* (p_Conn->whandler) )(p_Conn); if(revents & EPOLLOUT)
                        if(pSocketObj->ngx_epoll_oper_event(
                                p_Conn->fd,         //socket句柄
                                EPOLL_CTL_MOD,      //事件类型，这里是增加【因为我们准备增加个写通知】
                                EPOLLOUT,           //标志，这里代表要增加的标志,EPOLLOUT：可写【可写的时候通知我】
                                0,                  //对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数, 0：增加   1：去掉 2：完全覆盖
                                p_Conn              //连接池中的连接
                                ) == -1)

                        {
                            //有这情况发生？这可比较麻烦，不过先do nothing
                            ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()ngx_epoll_oper_event()失败.");
                        }

                        //ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()中数据没发送完毕【发送缓冲区满】，整个要发送%d，实际发送了%d。",p_Conn->isendlen,sendsize);

                    } //end if(sendsize > 0)
                    continue;  //继续处理其他消息
                }  //end if(sendsize > 0)

                //能走到这里，应该是有点问题的
                else if(sendsize == 0)//应该是socket断开了
                {
                    //发送0个字节，首先因为我发送的内容不是0个字节的；
                    //然后如果发送 缓冲区满则返回的应该是-1，而错误码应该是EAGAIN，所以我综合认为，这种情况我就把这个发送的包丢弃了【按对端关闭了socket处理】
                    //这个打印下日志，我还真想观察观察是否真有这种现象发生
                    //ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()中sendproc()居然返回0？"); //如果对方关闭连接出现send=0，那么这个日志可能会常出现，商用时就 应该干掉
                    //然后这个包干掉，不发送了

                    //发数据的时候，客户端socket断开是不会做任何事情， 我们只发数据
                    //收数据的时候，也就是监测可读事件的时候，客户端socket断开是会做处理的  (this->* (p_Conn->rhandler) )(p_Conn);

                    //不对socket做其他的额外处理 只是把内存释放掉
                    p_memory->FreeMemory(p_Conn->psendMemPointer);  //释放内存
                    p_Conn->psendMemPointer = NULL;
                    p_Conn->iThrowsendCount = 0;  //这行其实可以没有，因此此时此刻这东西就是=0的
                    continue;
                }

                //能走到这里，继续处理问题
                else if(sendsize == -1)
                {
                    //发送缓冲区已经满了【一个字节都没发出去，说明发送 缓冲区当前正好是满的】
                    ++p_Conn->iThrowsendCount; //标记发送缓冲区满了，需要通过epoll事件来驱动消息的继续发送
                    //投递此事件后，我们将依靠epoll驱动调用ngx_write_request_handler()函数发送数据
                    if(pSocketObj->ngx_epoll_oper_event(
                                p_Conn->fd,         //socket句柄
                                EPOLL_CTL_MOD,      //事件类型，这里是增加【因为我们准备增加个写通知】
                                EPOLLOUT,           //标志，这里代表要增加的标志,EPOLLOUT：可写【可写的时候通知我】
                                0,                  //对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数, 0：增加   1：去掉 2：完全覆盖
                                p_Conn              //连接池中的连接
                                ) == -1)
                    {
                        //有这情况发生？这可比较麻烦，不过先do nothing
                        ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()中ngx_epoll_add_event()_2失败.");
                    }
                    continue;
                }
                else
                {
                    //能走到这里的，应该就是返回值-2了，一般就认为对端断开了，等待recv()来做断开socket以及回收资源
                    p_memory->FreeMemory(p_Conn->psendMemPointer);  //释放内存
                    p_Conn->psendMemPointer = NULL;
                    p_Conn->iThrowsendCount = 0;  //这行其实可以没有，因此此时此刻这东西就是=0的
                    continue;
                }

            } //end while(pos != posend)

            err = pthread_mutex_unlock(&pSocketObj->m_sendMessageQueueMutex);//处理完之后 就释放这个资源
            if(err != 0)  ngx_log_stderr(err,"CSocekt::ServerSendQueueThread()pthread_mutex_unlock()失败，返回的错误码为%d!",err);

        } //if(pSocketObj->m_iSendMsgQueueCount > 0)
    } //end while

    return (void*)0;
}

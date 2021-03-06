


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


void CSocekt::ngx_event_accept(lpngx_connection_t oldc)
{
    struct sockaddr    mysockaddr;     
    socklen_t          socklen;
    int                err;
    int                level;
    int                s;
    static int         use_accept4 = 1;   
    lpngx_connection_t newc;              


    socklen = sizeof(mysockaddr);
    do  
    {
        if(use_accept4)
        {
            s = accept4(oldc->fd, &mysockaddr, &socklen, SOCK_NONBLOCK); 
        }
        else
        {
            s = accept(oldc->fd, &mysockaddr, &socklen);
        }

        if(s == -1)
        {
            err = errno;
            if(err == EAGAIN) 
            {
                return ;
            }
            level = NGX_LOG_ALERT;
            if (err == ECONNABORTED) 
            {
                level = NGX_LOG_ERR;
            }
            else if (err == EMFILE || err == ENFILE)                                                  
            {
                level = NGX_LOG_CRIT;
            }

            if(use_accept4 && err == ENOSYS) 
            {
                use_accept4 = 0;  
                continue;         
            }

            if (err == ECONNABORTED)  
            {

            }

            if (err == EMFILE || err == ENFILE)
            {

            }
            return;
        }  

        {
            ngx_log_stderr(0,"超出系统允许的最大连入用户数(最大允许连入数%d)，关闭连入请求(%d)。",m_worker_connections,s);
            close(s);
            return ;
        }

        if(m_connectionList.size() > (m_worker_connections * 5)) 
        {
          
            if(m_freeconnectionList.size() < m_worker_connections)
            {
                close(s);
                return ;
            }
        }

        newc = ngx_get_connection(s); 
        if(newc == NULL)
        {
            if(close(s) == -1)
            {
                ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocekt::ngx_event_accept()中close(%d)失败!",s);
            }
            return;
        }
      
        memcpy(&newc->s_sockaddr,&mysockaddr,socklen);  
        if(!use_accept4)
        {
            
            if(setnonblocking(s) == false)
            {
                ngx_close_connection(newc); 
                return; 
            }
        }

        newc->listening = oldc->listening;                    
        
        newc->rhandler = &CSocekt::ngx_read_request_handler;  
        newc->whandler = &CSocekt::ngx_write_request_handler; 

      
        if(ngx_epoll_oper_event(
                                s,                  
                                EPOLL_CTL_ADD,               
                                EPOLLIN|EPOLLRDHUP, 
                                0,                  
                                newc                
                                ) == -1)
        {   
            ngx_close_connection(newc);
            return; 
        }

        if(m_ifkickTimeCount == 1)
        {
            AddToTimerQueue(newc);
        }
        ++m_onlineUserCount;  
        break;
    } while (1);

    return;
}


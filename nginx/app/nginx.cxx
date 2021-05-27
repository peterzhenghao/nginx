

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/time.h>          

#include "ngx_macro.h"         
#include "ngx_func.h"          
#include "ngx_c_conf.h"        
#include "ngx_c_socket.h"      
#include "ngx_c_memory.h"      
#include "ngx_c_threadpool.h"  
#include "ngx_c_crc32.h"       
#include "ngx_c_slogic.h"      


static void freeresource();


size_t  g_argvneedmem=0;        
size_t  g_envneedmem=0;         
int     g_os_argc;              
char    **g_os_argv;            
char    *gp_envmem=NULL;        
int     g_daemonized=0;         


CLogicSocket   g_socket;       
CThreadPool    g_threadpool;   


pid_t   ngx_pid;                
pid_t   ngx_parent;             
int     ngx_process;            
int     g_stopEvent;            

sig_atomic_t  ngx_reap;         
                                


int main(int argc, char *const *argv)
{

    int exitcode = 0;           
    int i;                      


    g_stopEvent = 0;           
    ngx_pid    = getpid();      
    ngx_parent = getppid();     
  
    g_argvneedmem = 0;
    for(i = 0; i < argc; i++)  
    {
        g_argvneedmem += strlen(argv[i]) + 1; 
    }
    
    for(i = 0; environ[i]; i++)
    {
        g_envneedmem += strlen(environ[i]) + 1; 
    } 

    g_os_argc = argc;           
    g_os_argv = (char **) argv; 

  
    ngx_log.fd = -1;                  
    ngx_process = NGX_PROCESS_MASTER; 
    ngx_reap = 0;                     

 
    CConfig *p_config = CConfig::GetInstance(); 
    if(p_config->Load("nginx.conf") == false) 

        ngx_log_init();    
        ngx_log_stderr(0,"配置文件[%s]载入失败，退出!","nginx.conf");
       
        exitcode = 2; 
        goto lblexit;
    }
    
    CMemory::GetInstance();
    
    CCRC32::GetInstance();

   
    ngx_log_init();             

   
    if(ngx_init_signals() != 0) 
    {
        exitcode = 1;
        goto lblexit;
    }
   
    if(g_socket.Initialize() == false)
    {
        exitcode = 1;
        goto lblexit;
    }

  
    ngx_init_setproctitle();  

   
    if(p_config->GetIntDefault("Daemon",0) == 1) 
    {
       
        int cdaemonresult = ngx_daemon();

        if(cdaemonresult == -1)
        {
            exitcode = 1;  
            goto lblexit;
        }
        if(cdaemonresult == 1)
        {
            
            freeresource();   
                              
            exitcode = 0;
            return exitcode;  
        }
      
        g_daemonized = 1;  
    }

    
    ngx_master_process_cycle(); 

   
lblexit:
   
    ngx_log_stderr(0,"程序退出，再见了!");
    freeresource();  
    return exitcode;
}


void freeresource()
{
    if(gp_envmem)
    {
        delete []gp_envmem;
        gp_envmem = NULL;
    }

  
    if(ngx_log.fd != STDERR_FILENO && ngx_log.fd != -1)
    {
        close(ngx_log.fd);
        ngx_log.fd = -1; 
    }
}

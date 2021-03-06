
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

#include "ngx_global.h
#include "ngx_macro.h"
#include "ngx_func.h"
#include "ngx_c_conf.h"


static u_char err_levels[][20]  =
{
    {"stderr"},    
    {"emerg"},     
    {"alert"},     
    {"crit"},      
    {"error"},     
    {"warn"},      
    {"notice"},    
    {"info"},      
    {"debug"}      
};
ngx_log_t   ngx_log;



void ngx_log_stderr(int err, const char *fmt, ...)
{
    va_list args;                       
    u_char  errstr[NGX_MAX_ERROR_STR+1];
    u_char  *p,*last;

    memset(errstr,0,sizeof(errstr));     

    last = errstr + NGX_MAX_ERROR_STR;   
                                         
                                         

    p = ngx_cpymem(errstr, "nginx: ", 7);

    va_start(args, fmt); 
    p = ngx_vslprintf(p,last,fmt,args); /
    va_end(args);       
    if (err) 
    {
        p = ngx_log_errno(p, last, err);
    }

    if (p >= (last - 1))
    {
        p = (last - 1) - 1; 
                            
    }
    *p++ = '\n'; 

   
    write(STDERR_FILENO,errstr,p - errstr); 

    if(ngx_log.fd > STDERR_FILENO) 
    {
        err = 0;    
        p--;*p = 0; 
        ngx_log_error_core(NGX_LOG_STDERR,err,(const char *)errstr);
    }
    return;
}


u_char *ngx_log_errno(u_char *buf, u_char *last, int err)
{
    
    char *perrorinfo = strerror(err);
    size_t len = strlen(perrorinfo);

    char leftstr[10] = {0};
    sprintf(leftstr," (%d: ",err);
    size_t leftlen = strlen(leftstr);

    char rightstr[] = ") ";
    size_t rightlen = strlen(rightstr);

    size_t extralen = leftlen + rightlen; 
    if ((buf + len + extralen) < last)
    {
        buf = ngx_cpymem(buf, leftstr, leftlen);
        buf = ngx_cpymem(buf, perrorinfo, len);
        buf = ngx_cpymem(buf, rightstr, rightlen);
    }
    return buf;
}


void ngx_log_error_core(int level,  int err, const char *fmt, ...)
{
    u_char  *last;
    u_char  errstr[NGX_MAX_ERROR_STR+1];   

    memset(errstr,0,sizeof(errstr));
    last = errstr + NGX_MAX_ERROR_STR;

    struct timeval   tv;
    struct tm        tm;
    time_t           sec;   
    u_char           *p;    
    va_list          args;

    memset(&tv,0,sizeof(struct timeval));
    memset(&tm,0,sizeof(struct tm));

    gettimeofday(&tv, NULL);     

    sec = tv.tv_sec;             
    localtime_r(&sec, &tm);      
    tm.tm_mon++;                 
    tm.tm_year += 1900;          

    u_char strcurrtime[40]={0};  
    ngx_slprintf(strcurrtime,
                    (u_char *)-1,                       
                    "%4d/%02d/%02d %02d:%02d:%02d",     
                    tm.tm_year, tm.tm_mon,
                    tm.tm_mday, tm.tm_hour,
                    tm.tm_min, tm.tm_sec);
    p = ngx_cpymem(errstr,strcurrtime,strlen((const char *)strcurrtime));  
    p = ngx_slprintf(p, last, " [%s] ", err_levels[level]);                
    p = ngx_slprintf(p, last, "%P: ",ngx_pid);                             

    va_start(args, fmt);                     
    p = ngx_vslprintf(p, last, fmt, args);   
    va_end(args);                            

    if (err)  
    {
        p = ngx_log_errno(p, last, err);
    }
    if (p >= (last - 1))
    {
        p = (last - 1) - 1; 
    }
    *p++ = '\n'; 


    ssize_t   n;
    while(1)
    {
        if (level > ngx_log.log_level)
        {
          
            break;
        }
       
        n = write(ngx_log.fd,errstr,p - errstr);  
        if (n == -1)
        {
           
            if(errno == ENOSPC) 
            {

            }
            else
            {
               
                if(ngx_log.fd != STDERR_FILENO) 
                {
                    n = write(STDERR_FILENO,errstr,p - errstr);
                }
            }
        }
        break;
    } 
    return;
}


void ngx_log_init()
{
    u_char *plogname = NULL;
    size_t nlen;

   
    CConfig *p_config = CConfig::GetInstance();
    plogname = (u_char *)p_config->GetString("Log");
    if(plogname == NULL)
    {
        
        plogname = (u_char *) NGX_ERROR_LOG_PATH; 
    }

    ngx_log.log_level = p_config->GetIntDefault("LogLevel",NGX_LOG_NOTICE);

    ngx_log.fd = open((const char *)plogname,O_WRONLY|O_APPEND|O_CREAT,0644);
    if (ngx_log.fd == -1)  
    {
        ngx_log_stderr(errno,"[alert] could not open error log file: open() \"%s\" failed", plogname);
        ngx_log.fd = STDERR_FILENO;
    }
    return;
}

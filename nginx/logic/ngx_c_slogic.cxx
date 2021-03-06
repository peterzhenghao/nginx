

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
#include <pthread.h>   

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"

#include "ngx_c_memory.h"
#include "ngx_c_crc32.h"
#include "ngx_c_slogic.h"
#include "ngx_logiccomm.h"
#include "ngx_c_lockmutex.h"


typedef bool (CLogicSocket::*handler)(  lpngx_connection_t pConn,      
                                        LPSTRUC_MSG_HEADER pMsgHeader, 
                                        char *pPkgBody,                
                                        unsigned short iBodyLength);   


static const handler statusHandler[] =
{
   
    &CLogicSocket::_HandlePing,                             
    NULL,                                                   
    NULL,                                                   
    NULL,                                                   
    NULL,                                                   

  
    &CLogicSocket::_HandleRegister,                         
    &CLogicSocket::_HandleLogIn,                            

};
#define AUTH_TOTAL_COMMANDS sizeof(statusHandler)/sizeof(handler) 

CLogicSocket::CLogicSocket()
{

}

CLogicSocket::~CLogicSocket()
{

}


bool CLogicSocket::Initialize()
{
    bool bParentInit = CSocekt::Initialize(); 
    return bParentInit;
}

void CLogicSocket::threadRecvProcFunc(char *pMsgBuf)
{
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;                  
    LPCOMM_PKG_HEADER  pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf+m_iLenMsgHeader); 
    void  *pPkgBody;                                                              
    unsigned short pkglen = ntohs(pPkgHeader->pkgLen);                            

    if(m_iLenPkgHeader == pkglen)
    {
      
		if(pPkgHeader->crc32 != 0) 
		{
			return; 
		}
		pPkgBody = NULL;
    }
    else
	{
		pPkgHeader->crc32 = ntohl(pPkgHeader->crc32);		         
		pPkgBody = (void *)(pMsgBuf+m_iLenMsgHeader+m_iLenPkgHeader);


		int calccrc = CCRC32::GetInstance()->Get_CRC((unsigned char *)pPkgBody, pkglen-m_iLenPkgHeader); 
		if(calccrc != pPkgHeader->crc32) 
		{
            ngx_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中CRC错误[服务器:%d/客户端:%d]，丢弃数据!",calccrc,pPkgHeader->crc32);    
		}
	}


    unsigned short imsgCode = ntohs(pPkgHeader->msgCode); 

    lpngx_connection_t p_Conn = pMsgHeader->pConn;        

  
    if(p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)  
    {
        return; 
    }

  
	if(imsgCode >= AUTH_TOTAL_COMMANDS)
    {
        ngx_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码不对!",imsgCode);
        return; 
    }


    if(statusHandler[imsgCode] == NULL)
    {
        ngx_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码找不到对应的处理函数!",imsgCode); 
        return; 
    }

    (this->*statusHandler[imsgCode])(p_Conn,pMsgHeader,(char *)pPkgBody,pkglen-m_iLenPkgHeader);
    return;
}

void CLogicSocket::procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg, time_t cur_time)
{
    CMemory *p_memory = CMemory::GetInstance();

    if(tmpmsg->iCurrsequence == tmpmsg->pConn->iCurrsequence) 
    {
        lpngx_connection_t p_Conn = tmpmsg->pConn;

        
        if(m_ifTimeOutKick == 1) 
        {
            zdClosesocketProc(p_Conn);
        }
        else if( (cur_time - p_Conn->lastPingTime ) > (m_iWaitTime*3+10) ) 
        {
           
            zdClosesocketProc(p_Conn);
        }

        p_memory->FreeMemory(tmpmsg);
    }
    else 
    {
        p_memory->FreeMemory(tmpmsg);
    }
    return;
}
void CLogicSocket::SendNoBodyPkgToClient(LPSTRUC_MSG_HEADER pMsgHeader,unsigned short iMsgCode)
{
    CMemory  *p_memory = CMemory::GetInstance();

    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader, false);
    char *p_tmpbuf = p_sendbuf;

	memcpy(p_tmpbuf, pMsgHeader, m_iLenMsgHeader);
	p_tmpbuf += m_iLenMsgHeader;

    LPCOMM_PKG_HEADER pPkgHeader = (LPCOMM_PKG_HEADER)p_tmpbuf;	 
    pPkgHeader->msgCode = htons(iMsgCode);
    pPkgHeader->pkgLen = htons(m_iLenPkgHeader); 
	pPkgHeader->crc32 = 0;
    msgSend(p_sendbuf);
    return;
}


bool CLogicSocket::_HandleRegister(lpngx_connection_t pConn,LPSTRUC_MSG_HEADER pMsgHeader,char *pPkgBody,unsigned short iBodyLength)
{
    
    if(pPkgBody == NULL) 
    {
        return false;
    }

    int iRecvLen = sizeof(STRUCT_REGISTER); 
    if(iRecvLen != iBodyLength) 
    {
        return false;
    }


    CLock lock(&pConn->logicPorcMutex); 

    LPSTRUCT_REGISTER p_RecvInfo = (LPSTRUCT_REGISTER)pPkgBody;
    p_RecvInfo->iType = ntohl(p_RecvInfo->iType);          
    p_RecvInfo->username[sizeof(p_RecvInfo->username)-1]=0;
    p_RecvInfo->password[sizeof(p_RecvInfo->password)-1]=0;


 
	LPCOMM_PKG_HEADER pPkgHeader;
	CMemory  *p_memory = CMemory::GetInstance();
	CCRC32   *p_crc32 = CCRC32::GetInstance();
    int iSendLen = sizeof(STRUCT_REGISTER);

    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader+m_iLenPkgHeader+iSendLen,false);

    memcpy(p_sendbuf,pMsgHeader,m_iLenMsgHeader);                

    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf+m_iLenMsgHeader);    
    pPkgHeader->msgCode = _CMD_REGISTER;	                        
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode);	            
    pPkgHeader->pkgLen  = htons(m_iLenPkgHeader + iSendLen);        

    LPSTRUCT_REGISTER p_sendInfo = (LPSTRUCT_REGISTER)(p_sendbuf+m_iLenMsgHeader+m_iLenPkgHeader);	
    

    
    pPkgHeader->crc32   = p_crc32->Get_CRC((unsigned char *)p_sendInfo,iSendLen);
    pPkgHeader->crc32   = htonl(pPkgHeader->crc32);

    msgSend(p_sendbuf);
    return true;
}

bool CLogicSocket::_HandleLogIn(lpngx_connection_t pConn,LPSTRUC_MSG_HEADER pMsgHeader,char *pPkgBody,unsigned short iBodyLength)
{
    if(pPkgBody == NULL)
    {
        return false;
    }
    int iRecvLen = sizeof(STRUCT_LOGIN);
    if(iRecvLen != iBodyLength)
    {
        return false;
    }
    CLock lock(&pConn->logicPorcMutex);

    LPSTRUCT_LOGIN p_RecvInfo = (LPSTRUCT_LOGIN)pPkgBody;
    p_RecvInfo->username[sizeof(p_RecvInfo->username)-1]=0;
    p_RecvInfo->password[sizeof(p_RecvInfo->password)-1]=0;

	LPCOMM_PKG_HEADER pPkgHeader;
	CMemory  *p_memory = CMemory::GetInstance();
	CCRC32   *p_crc32 = CCRC32::GetInstance();

    int iSendLen = sizeof(STRUCT_LOGIN);
    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader+m_iLenPkgHeader+iSendLen,false);
    memcpy(p_sendbuf,pMsgHeader,m_iLenMsgHeader);
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf+m_iLenMsgHeader);
    pPkgHeader->msgCode = _CMD_LOGIN;
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode);
    pPkgHeader->pkgLen  = htons(m_iLenPkgHeader + iSendLen);
    LPSTRUCT_LOGIN p_sendInfo = (LPSTRUCT_LOGIN)(p_sendbuf+m_iLenMsgHeader+m_iLenPkgHeader);
    pPkgHeader->crc32   = p_crc32->Get_CRC((unsigned char *)p_sendInfo,iSendLen);
    pPkgHeader->crc32   = htonl(pPkgHeader->crc32);
    msgSend(p_sendbuf);
    return true;
}


bool CLogicSocket::_HandlePing(lpngx_connection_t pConn,LPSTRUC_MSG_HEADER pMsgHeader,char *pPkgBody,unsigned short iBodyLength)
{
    if(iBodyLength != 0)  
		return false;

    CLock lock(&pConn->logicPorcMutex); 
    pConn->lastPingTime = time(NULL);   

    SendNoBodyPkgToClient(pMsgHeader,_CMD_PING);

    return true;
}
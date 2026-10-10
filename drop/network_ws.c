#include "network.h"
#include <stdlib.h>
#include <string.h>

typedef struct drop_ws_io {
    HINTERNET socket; HANDLE received,sent,closed; CRITICAL_SECTION guard;
    LONG references,closing; DWORD rx_error,tx_error,bytes;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
} drop_ws_io;
static void release(drop_ws_io *io) {
    if(io && InterlockedDecrement(&io->references)==0) {
        CloseHandle(io->received); CloseHandle(io->sent); CloseHandle(io->closed);
        DeleteCriticalSection(&io->guard); free(io);
    }
}
static void CALLBACK completed(HINTERNET handle,DWORD_PTR context,DWORD status,void *info,DWORD size) {
    drop_ws_io *io=(drop_ws_io *)context; (void)handle;
    if(!io) return;
    if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { SetEvent(io->closed); release(io); return; }
    if(status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE && info && size==sizeof(WINHTTP_WEB_SOCKET_STATUS)) {
        WINHTTP_WEB_SOCKET_STATUS *result=info; io->bytes=result->dwBytesTransferred; io->type=result->eBufferType; SetEvent(io->received);
    } else if(status==WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE) SetEvent(io->sent);
    else if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && info && size==sizeof(WINHTTP_ASYNC_RESULT)) {
        DWORD error=((WINHTTP_ASYNC_RESULT *)info)->dwError;
        io->rx_error=error; io->tx_error=error; SetEvent(io->received); SetEvent(io->sent);
    }
}
int drop_ws_attach(drop_ws *ws) {
    drop_ws_io *io=calloc(1,sizeof(*io)); DWORD_PTR context;
    if(!io) return 0;
    io->references=1; io->socket=ws->socket; InitializeCriticalSection(&io->guard);
    io->received=CreateEventW(NULL,TRUE,FALSE,NULL); io->sent=CreateEventW(NULL,TRUE,FALSE,NULL); io->closed=CreateEventW(NULL,TRUE,FALSE,NULL);
    context=(DWORD_PTR)io;
    if(!io->received || !io->sent || !io->closed || !WinHttpSetOption(ws->socket,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context)) ||
        WinHttpSetStatusCallback(ws->socket,completed,WINHTTP_CALLBACK_FLAG_READ_COMPLETE|WINHTTP_CALLBACK_FLAG_WRITE_COMPLETE|
            WINHTTP_CALLBACK_FLAG_REQUEST_ERROR|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK) {
        WinHttpSetStatusCallback(ws->socket,NULL,0,0); release(io); return 0;
    }
    InterlockedIncrement(&io->references); ws->io=io; return 1;
}
void drop_ws_retain(drop_ws *ws) { if(ws->io) InterlockedIncrement(&ws->io->references); }
void drop_ws_release(drop_ws *ws) { release(ws->io); memset(ws,0,sizeof(*ws)); }
static void abort_socket(drop_ws_io *io) {
    if(!io) return;
    EnterCriticalSection(&io->guard);
    if(!InterlockedExchange(&io->closing,1)) WinHttpCloseHandle(io->socket);
    LeaveCriticalSection(&io->guard);
}
static DWORD wait_result(drop_ws_io *io,HANDLE ready,HANDLE cancel,DWORD timeout) {
    HANDLE events[3]={ready,io->closed,cancel}; DWORD result=WaitForMultipleObjects(cancel?3:2,events,FALSE,timeout);
    if(result==WAIT_OBJECT_0) return NO_ERROR;
    if(result==WAIT_TIMEOUT || result==WAIT_OBJECT_0+2) { abort_socket(io); WaitForSingleObject(io->closed,INFINITE); }
    return result==WAIT_TIMEOUT?ERROR_WINHTTP_TIMEOUT:ERROR_OPERATION_ABORTED;
}
DWORD drop_ws_receive(drop_ws *ws,void *buffer,DWORD capacity,DWORD *length,WINHTTP_WEB_SOCKET_BUFFER_TYPE *type,HANDLE cancel) {
    drop_ws_io *io=ws->io; DWORD error;
    if(!io) return ERROR_INVALID_HANDLE;
    EnterCriticalSection(&io->guard);
    if(io->closing) { LeaveCriticalSection(&io->guard); return ERROR_OPERATION_ABORTED; }
    ResetEvent(io->received); io->rx_error=0; io->bytes=0;
    error=WinHttpWebSocketReceive(io->socket,buffer,capacity,NULL,NULL);
    LeaveCriticalSection(&io->guard);
    if(error==NO_ERROR) error=wait_result(io,io->received,cancel,INFINITE);
    if(error==NO_ERROR) { error=io->rx_error; *length=io->bytes; *type=io->type; }
    return error;
}
DWORD drop_ws_send(drop_ws *ws,const void *buffer,DWORD length,HANDLE cancel) {
    drop_ws_io *io=ws->io; DWORD error;
    if(!io) return ERROR_INVALID_HANDLE;
    EnterCriticalSection(&io->guard);
    if(io->closing) { LeaveCriticalSection(&io->guard); return ERROR_OPERATION_ABORTED; }
    ResetEvent(io->sent); io->tx_error=0;
    error=WinHttpWebSocketSend(io->socket,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,(void *)buffer,length);
    LeaveCriticalSection(&io->guard);
    if(error==NO_ERROR) error=wait_result(io,io->sent,cancel,15000);
    return error==NO_ERROR?io->tx_error:error;
}
void drop_ws_close(drop_ws *ws) {
    if(ws->io) { abort_socket(ws->io); release(ws->io); }
    else if(ws->socket) WinHttpCloseHandle(ws->socket);
    if(ws->connection) WinHttpCloseHandle(ws->connection); if(ws->session) WinHttpCloseHandle(ws->session);
    memset(ws,0,sizeof(*ws));
}

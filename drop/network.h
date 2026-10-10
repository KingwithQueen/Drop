#ifndef DROP_NETWORK_H
#define DROP_NETWORK_H
#include "drop_common.h"
drop_error drop_http_json(const WCHAR *url, const WCHAR *method, const WCHAR *path,
    const char *token, const cJSON *body, cJSON **response, DWORD *status);
int drop_network_probe(const WCHAR *url);
void drop_network_cancel_event(HANDLE event);
int drop_network_wait(DWORD milliseconds);
drop_error drop_http_json_ca(const WCHAR *url, const WCHAR *method, const WCHAR *path,
    const char *token, const cJSON *body, cJSON **response, DWORD *status, const WCHAR *ca_file);
drop_error drop_http_bytes(const WCHAR *url,const WCHAR *method,const WCHAR *path,const char *token,
    const unsigned char *body,DWORD body_size,int binary_request,int binary_response,
    unsigned char **response,DWORD *response_size,DWORD *status,const WCHAR *ca_file);
typedef struct drop_ws { HINTERNET session, connection, socket; struct drop_ws_io *io; } drop_ws;
drop_error drop_ws_open(const WCHAR *url, const char *token, drop_ws *ws, const WCHAR *ca_file);
void drop_ws_close(drop_ws *ws);
int drop_ws_attach(drop_ws *ws);
void drop_ws_retain(drop_ws *ws);
void drop_ws_release(drop_ws *ws);
DWORD drop_ws_receive(drop_ws *ws,void *buffer,DWORD capacity,DWORD *length,WINHTTP_WEB_SOCKET_BUFFER_TYPE *type,HANDLE cancel);
DWORD drop_ws_send(drop_ws *ws,const void *buffer,DWORD length,HANDLE cancel);
#endif

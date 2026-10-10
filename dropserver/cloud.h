#ifndef DROP_CLOUD_H
#define DROP_CLOUD_H
#include "server.h"
typedef struct drop_cloud {
    drop_server *server;
    cJSON *credentials;
    char access[4097];
    WCHAR origin[2049],token_origin[2049],ca[32768],secret[32768];
    int machine;
} drop_cloud;
typedef struct cloud_reply {
    DWORD status,size;
    unsigned char *data;
    WCHAR location[4097],range[256];
} cloud_reply;
int drop_cloud_schema(drop_server *server);
drop_error drop_cloud_open(drop_cloud *cloud,drop_server *server);
void drop_cloud_close(drop_cloud *cloud);
drop_error drop_cloud_http(drop_cloud *cloud,const WCHAR *method,const WCHAR *url,const WCHAR *headers,const void *body,DWORD length,int authenticated,cloud_reply *reply);
void drop_cloud_reply_free(cloud_reply *reply);
drop_error drop_cloud_status(const cloud_reply *reply);
drop_error drop_cloud_refresh(drop_cloud *cloud);
drop_error drop_cloud_json(drop_cloud *cloud,const WCHAR *method,const WCHAR *path,cJSON *input,cJSON **output);
int drop_cloud_authorize(drop_server *server,const WCHAR *client_file);
int drop_cloud_test_authorize(drop_server *server,const WCHAR *origin,const WCHAR *ca);
drop_error drop_cloud_upload(drop_cloud *cloud,const WCHAR *backup);
drop_error drop_cloud_list(drop_cloud *cloud,const char *task,int complete_only,cJSON **files);
drop_error drop_cloud_download(drop_cloud *cloud,const char *task,const WCHAR *target);
drop_error drop_cloud_delete(drop_cloud *cloud,const char *task);
int drop_cloud_command(drop_server *server,int argc,WCHAR **argv);
int drop_cloud_start(drop_server *server);
void drop_cloud_stop(drop_server *server);
#endif

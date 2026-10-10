#ifndef DROP_CLIENT_H
#define DROP_CLIENT_H
#include "network.h"
#include <sqlite3.h>

typedef struct drop_client {
    sqlite3 *database;
    HANDLE lock;
    WCHAR root[32768], url[2049], ca_file[32768];
    WCHAR files[32768];
    char user[DROP_ID_CAP], device[DROP_ID_CAP], server[DROP_ID_CAP], generation[DROP_ID_CAP], token[DROP_TOKEN_CAP];
    uint64_t downloaded_bytes;
    int use_zstd,zstd_level;
    unsigned heartbeat_seconds,connection_timeout_seconds,reconcile_seconds;
    unsigned sync_page_limit;
    uint64_t download_quantum;
    int more_bodies,more_downloads;
    int restored;
} drop_client;
int drop_client_login(const WCHAR *root, const WCHAR *url, const char *login, const char *password, const WCHAR *ca_file);
int drop_client_open(drop_client *client, const WCHAR *root);
void drop_client_close(drop_client *client);
drop_error drop_client_queue(drop_client *client, const char *body, const char *recipient, char request[DROP_ID_CAP]);
drop_error drop_client_flush(drop_client *client);
drop_error drop_client_sync(drop_client *client);
int drop_client_watch(drop_client *client);
int drop_client_test(const WCHAR *root);
int drop_client_files_schema(drop_client *client);
drop_error drop_client_queue_files(drop_client *client,const char *body,const char *recipient,
    const WCHAR *const *paths,int count,char request[DROP_ID_CAP]);
drop_error drop_client_uploads(drop_client *client,const char *request,cJSON *array);
drop_error drop_client_downloads(drop_client *client);
int drop_client_verify_files(drop_client *client);
drop_error drop_client_register_files(drop_client *client,const cJSON *message);
int drop_client_ui_schema(drop_client *client);
int drop_client_receipts_schema(drop_client *client);
drop_error drop_client_message_order(drop_client *client,const cJSON *message);
drop_error drop_client_receipts(drop_client *client,int include_files);
int drop_client_read_queue(drop_client *client,const char *message);
int drop_client_snapshot(drop_client *client,int version);
drop_error drop_client_database_error(drop_client *client);
drop_error drop_client_reconcile(drop_client *client);
drop_error drop_client_retry_review(drop_client *client);
#endif

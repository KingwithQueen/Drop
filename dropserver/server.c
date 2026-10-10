#include "server.h"
#include "cloud.h"
#include <civetweb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct drop_peer {
    struct mg_connection *connection;
    char user[DROP_ID_CAP], device[DROP_ID_CAP], token[DROP_TOKEN_CAP];
    sqlite3_int64 state_seq;
    struct drop_peer *next;
} drop_peer;

static HANDLE console_stop;
static BOOL WINAPI control_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        if (console_stop) SetEvent(console_stop); return TRUE;
    }
    return FALSE;
}
static int send_json(struct mg_connection *connection, int status, cJSON *object) {
    char *body = cJSON_PrintUnformatted(object); const char *reason;
    int header_result, body_result;
    if (!body) { mg_send_http_error(connection, 500, "allocation failed"); return 500; }
    reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" :
        status == 413 ? "Payload Too Large" : status == 404 ? "Not Found" : "Internal Server Error";
    header_result = mg_printf(connection, "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n", status, reason, strlen(body));
    body_result = mg_write(connection, body, strlen(body));
    if (header_result < 0 || body_result != (int)strlen(body)) {
        fprintf(stderr, "response_write_failed status=%d\n", status);
    }
    /* Login responses contain a token. Zero the serialized response before freeing. */
    sodium_memzero(body, strlen(body)); cJSON_free(body); return status;
}
static int error_json(struct mg_connection *connection, int status, drop_error error) {
    cJSON *object = cJSON_CreateObject(); int result;
    if (!object) { mg_send_http_error(connection, 500, "allocation failed"); return 500; }
    cJSON_AddStringToObject(object, "error", drop_error_name(error));
    result = send_json(connection, status, object); cJSON_Delete(object); return result;
}
static int api_error(struct mg_connection *connection,drop_error error) {
    return error_json(connection,error==DROP_E_AUTH?401:error==DROP_E_CONFLICT?409:error==DROP_E_LIMIT?413:error==DROP_E_ARGUMENT?400:error==DROP_E_SPACE?507:500,error);
}
static cJSON *read_json(struct mg_connection *connection) {
    const struct mg_request_info *request = mg_get_request_info(connection);
    const char *type = mg_get_header(connection, "Content-Type");
    size_t length, position = 0; char *body; cJSON *object;
    if (!type || strcmp(type, "application/json") || request->content_length <= 0 ||
        request->content_length > DROP_MAX_JSON) return NULL;
    length = (size_t)request->content_length;
    body = malloc(length + 1); if (!body) return NULL;
    while (position < length) {
        int amount = mg_read(connection, body + position, length - position);
        if (amount <= 0) { sodium_memzero(body, length); free(body); return NULL; }
        position += (size_t)amount;
    }
    body[length] = 0; object = drop_json_parse(body, length); sodium_memzero(body, length); free(body); return object;
}
static const char *bearer(struct mg_connection *connection) {
    const char *header = mg_get_header(connection, "Authorization");
    if (!header || strncmp(header, "Bearer ", 7) || strlen(header + 7) != 64) return NULL;
    return header + 7;
}
static int api_request_inner(struct mg_connection *connection) {
    const struct mg_request_info *request = mg_get_request_info(connection);
    drop_server *server = request->user_data; cJSON *object = NULL, *response = NULL;
    const char *path = request->local_uri, *token, *login, *password, *device, *name;
    char user_id[DROP_ID_CAP], device_id[DROP_ID_CAP]; int result;
    if (!path || !request->request_method) return error_json(connection, 400, DROP_E_ARGUMENT);
    if (!request->is_ssl) return error_json(connection, 400, DROP_E_PROTOCOL);
    if (request->content_length > DROP_MAX_JSON) return error_json(connection, 413, DROP_E_LIMIT);
    if (!strcmp(path, "/api/v1/server-info") && !strcmp(request->request_method, "GET")) {
        response = cJSON_CreateObject(); if (!response) return error_json(connection, 500, DROP_E_IO);
        cJSON_AddNumberToObject(response, "protocol_version", DROP_PROTOCOL_VERSION);
        cJSON_AddStringToObject(response, "version", DROP_VERSION);
        cJSON_AddStringToObject(response, "server_id", server->keys.server_id);
        cJSON_AddStringToObject(response, "generation", server->keys.generation);
        result = send_json(connection, 200, response); cJSON_Delete(response); return result;
    }
    if (!strcmp(path, "/api/v1/login") && !strcmp(request->request_method, "POST")) {
        if (!(object = read_json(connection))) return error_json(connection, 400, DROP_E_ARGUMENT);
        login = drop_json_string(object, "login", 64); password = drop_json_string(object, "password", 1024);
        device = drop_json_string(object, "device_id", 32); name = drop_json_string(object, "device_name", 128);
        result = drop_login(server, login, password, device, name, &response);
        if (password) sodium_memzero((char *)password, strlen(password)); cJSON_Delete(object);
        if (!result) return error_json(connection, 401, DROP_E_AUTH);
        result = send_json(connection, 200, response);
        token = drop_json_string(response, "token", 64); if (token) sodium_memzero((char *)token, strlen(token));
        cJSON_Delete(response); return result;
    }
    if (strncmp(path, "/api/v1/", 8)) return error_json(connection, 404, DROP_E_UNSUPPORTED);
    token = bearer(connection);
    if (!drop_authorize(server, token, user_id, device_id)) return error_json(connection, 401, DROP_E_AUTH);
    if(!strcmp(path,"/api/v1/uploads") && !strcmp(request->request_method,"POST")) {
        drop_error error; object=read_json(connection); if(!object) return api_error(connection,DROP_E_ARGUMENT);
        error=drop_upload_create(server,user_id,object,&response); cJSON_Delete(object);
        if(error!=DROP_OK) return api_error(connection,error);
        result=send_json(connection,200,response); cJSON_Delete(response); return result;
    }
    if(!strncmp(path,"/api/v1/uploads/",16) || !strncmp(path,"/api/v1/attachments/",20)) {
        int download=!strncmp(path,"/api/v1/attachments/",20); const char *start=path+(download?20:16),*suffix;
        char id[DROP_ID_CAP]; uint64_t index; drop_error error;
        if(strlen(start)<32) return api_error(connection,DROP_E_ARGUMENT);
        memcpy(id,start,32); id[32]=0; suffix=start+32;
        if(!drop_id_valid(id)) return api_error(connection,DROP_E_ARGUMENT);
        if(!download && !*suffix && !strcmp(request->request_method,"GET")) error=drop_upload_status(server,user_id,id,&response);
        else if(!download && !strcmp(suffix,"/complete") && !strcmp(request->request_method,"POST")) error=drop_upload_finish(server,user_id,id,&response);
        else if(!strncmp(suffix,"/chunks/",8) && drop_u64_parse(suffix+8,&index)) {
            unsigned char *bytes=NULL; DWORD length=0;
            if(download && !strcmp(request->request_method,"GET")) {
                error=drop_attachment_chunk(server,user_id,id,index,&bytes,&length);
                if(error!=DROP_OK) return api_error(connection,error);
                mg_printf(connection,"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lu\r\nCache-Control: no-store\r\n\r\n",length);
                if(mg_write(connection,bytes,length)!=(int)length) fputs("attachment_write_failed\n",stderr);
                sodium_memzero(bytes,length); free(bytes); return 200;
            }
            if(download || strcmp(request->request_method,"PUT")) return api_error(connection,DROP_E_UNSUPPORTED);
            if(request->content_length<=0 || request->content_length>DROP_CHUNK_SIZE ||
                !mg_get_header(connection,"Content-Type") || strcmp(mg_get_header(connection,"Content-Type"),"application/octet-stream")) return api_error(connection,DROP_E_ARGUMENT);
            length=(DWORD)request->content_length; bytes=malloc(length); if(!bytes) return api_error(connection,DROP_E_IO);
            { DWORD position=0; while(position<length) { int n=mg_read(connection,bytes+position,length-position); if(n<=0) break; position+=(DWORD)n; }
                error=position==length?drop_upload_chunk(server,user_id,id,index,bytes,length):DROP_E_ARGUMENT; }
            sodium_memzero(bytes,length); free(bytes); if(error!=DROP_OK) return api_error(connection,error);
            response=cJSON_CreateObject(); if(!response || !cJSON_AddBoolToObject(response,"stored",1)) { cJSON_Delete(response); return api_error(connection,DROP_E_IO); }
        } else return api_error(connection,DROP_E_ARGUMENT);
        if(error!=DROP_OK) return api_error(connection,error);
        result=send_json(connection,200,response); cJSON_Delete(response); return result;
    }
    if (!strcmp(path, "/api/v1/events") && !strcmp(request->request_method, "GET")) {
        const char *upgrade = mg_get_header(connection, "Upgrade");
        if (!upgrade || _stricmp(upgrade, "websocket")) return error_json(connection, 400, DROP_E_PROTOCOL);
        return 0; /* Only this authenticated registered path enters CivetWeb's WS handler. */
    }
    if (!strcmp(request->request_method, "POST") &&
        (!strcmp(path, "/api/v1/messages") || !strcmp(path, "/api/v1/sync") || !strcmp(path, "/api/v1/accounts") ||
        !strcmp(path,"/api/v1/saved") || !strcmp(path,"/api/v1/read") || !strcmp(path,"/api/v1/states"))) {
        drop_error error; char recipient[DROP_ID_CAP] = {0};
        object = read_json(connection); if (!object) return error_json(connection, 400, DROP_E_ARGUMENT);
        if (!strcmp(path, "/api/v1/messages")) error = drop_message_submit(server, user_id, device_id, object, &response, recipient);
        else if (!strcmp(path, "/api/v1/sync")) error = drop_sync_read(server, user_id, object, &response);
        else if(!strcmp(path,"/api/v1/saved")) error=drop_saved_submit(server,user_id,device_id,object,&response);
        else if(!strcmp(path,"/api/v1/read")) error=drop_read_submit(server,user_id,object,&response);
        else if(!strcmp(path,"/api/v1/states")) error=drop_states_read(server,user_id,object,&response);
        else error = drop_account_lookup(server, object, &response);
        cJSON_Delete(object);
        if (error != DROP_OK) return api_error(connection,error);
        result = send_json(connection, 200, response);
        if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response,"changed"))) drop_notify_states(server);
        cJSON_Delete(response);
        if (*recipient) drop_notify(server, user_id, recipient);
        return result;
    }
    if (!strcmp(path, "/api/v1/logout") && !strcmp(request->request_method, "POST")) {
        if (!drop_logout(server, token)) return error_json(connection, 500, DROP_E_DATABASE);
        response = cJSON_CreateObject(); cJSON_AddBoolToObject(response, "revoked", 1);
    } else if (!strcmp(path, "/api/v1/devices") && !strcmp(request->request_method, "GET")) {
        sqlite3_stmt *statement = NULL; cJSON *array;
        response = cJSON_CreateObject(); array = cJSON_AddArrayToObject(response, "devices");
        EnterCriticalSection(&server->database_lock);
        if (sqlite3_prepare_v2(server->database, "SELECT device_id,name,revoked FROM devices WHERE user_id=?1 ORDER BY device_id LIMIT 100", -1, &statement, NULL) != SQLITE_OK) {
            LeaveCriticalSection(&server->database_lock); cJSON_Delete(response); return error_json(connection, 500, DROP_E_DATABASE);
        }
        sqlite3_bind_text(statement, 1, user_id, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(statement) == SQLITE_ROW) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "device_id", (const char *)sqlite3_column_text(statement, 0));
            cJSON_AddStringToObject(item, "name", (const char *)sqlite3_column_text(statement, 1));
            cJSON_AddBoolToObject(item, "revoked", sqlite3_column_int(statement, 2)); cJSON_AddItemToArray(array, item);
        }
        sqlite3_finalize(statement); LeaveCriticalSection(&server->database_lock);
    } else return error_json(connection, 404, DROP_E_UNSUPPORTED);
    result = send_json(connection, 200, response); cJSON_Delete(response); return result;
}
static int api_request(struct mg_connection *connection) {
    const struct mg_request_info *request=mg_get_request_info(connection); DWORD thread=GetCurrentThreadId(); int result,kind=0,trace=(DROP_DIAGNOSTICS?getenv("DROP_TEST_TRACE"):NULL)!=NULL;
    const char *path=request->local_uri;
    if(path) kind=!strcmp(path,"/api/v1/sync")?1:!strcmp(path,"/api/v1/saved")?2:!strcmp(path,"/api/v1/states")?3:
        !strcmp(path,"/api/v1/read")?4:!strcmp(path,"/api/v1/messages")?5:!strncmp(path,"/api/v1/uploads",15)?6:!strncmp(path,"/api/v1/attachments",19)?7:0;
    if(trace) fprintf(stderr,"request_start kind=%d thread=%lu tick=%llu\n",kind,thread,(unsigned long long)GetTickCount64());
    result=api_request_inner(connection);
    if(trace) fprintf(stderr,"request_end kind=%d thread=%lu tick=%llu status=%d\n",kind,thread,(unsigned long long)GetTickCount64(),result);
    return result;
}
static int silent_log(const struct mg_connection *connection, const char *message) {
    (void)connection; (void)message;
    /* Do not log raw HTTP messages, URLs, headers, or OpenSSL key material. */
    return 1;
}
static int ws_connect(const struct mg_connection *connection, void *data) {
    drop_server *server = data; char user[DROP_ID_CAP], device[DROP_ID_CAP];
    const char *header = mg_get_header(connection, "Authorization");
    return !header || strncmp(header, "Bearer ", 7) || !drop_authorize(server, header + 7, user, device);
}
static void ws_ready(struct mg_connection *connection, void *data) {
    drop_server *server = data; drop_peer *peer = calloc(1, sizeof(*peer)), *item; int count = 0;
    const char *token = bearer(connection);
    if (!peer || !drop_authorize(server, token, peer->user, peer->device)) { free(peer); mg_close_connection(connection); return; }
    peer->connection = connection; strcpy_s(peer->token, sizeof(peer->token), token);
    EnterCriticalSection(&server->peers_lock);
    for (item = server->peers; item; item = item->next) ++count;
    if (count >= 16) { LeaveCriticalSection(&server->peers_lock); sodium_memzero(peer, sizeof(*peer)); free(peer); mg_close_connection(connection); return; }
    peer->next = server->peers; server->peers = peer;
    if((DROP_DIAGNOSTICS?getenv("DROP_TEST_TRACE"):NULL)) fprintf(stderr,"ws_open tick=%llu\n",(unsigned long long)GetTickCount64());
    mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_TEXT, "{\"type\":\"changed\"}", 18);
    LeaveCriticalSection(&server->peers_lock);
}
static int ws_data(struct mg_connection *connection, int bits, char *data, size_t length, void *opaque) {
    drop_server *server = opaque; char user[DROP_ID_CAP], device[DROP_ID_CAP]; drop_peer *peer;
    int authorized = 0, opcode = bits & 15;
    EnterCriticalSection(&server->peers_lock);
    for (peer = server->peers; peer; peer = peer->next) if (peer->connection == connection) {
        authorized = drop_authorize(server, peer->token, user, device); break;
    }
    LeaveCriticalSection(&server->peers_lock);
    if (!authorized || length > 1024 || !(bits & 128)) return 0;
    if (opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) return 0;
    if (opcode == MG_WEBSOCKET_OPCODE_PING) { mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_PONG, data, length); return 1; }
    if (opcode != MG_WEBSOCKET_OPCODE_TEXT || length != 20 || memcmp(data, "{\"type\":\"heartbeat\"}", 20)) return 0;
    if((DROP_DIAGNOSTICS?getenv("DROP_TEST_TRACE"):NULL)) fprintf(stderr,"ws_heartbeat tick=%llu\n",(unsigned long long)GetTickCount64());
    if(DROP_DIAGNOSTICS && getenv("DROP_TEST_DROP_HEARTBEAT")) return 1;
    return mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_TEXT, "{\"type\":\"heartbeat\"}", 20) > 0;
}
static void ws_close(const struct mg_connection *connection, void *data) {
    drop_server *server = data; drop_peer **link;
    /* CivetWeb invokes this before taking the generic close_connection mutex.
       peers_lock prevents a broadcaster using a pointer after this callback. */
    EnterCriticalSection(&server->peers_lock);
    for (link = &server->peers; *link; link = &(*link)->next) if ((*link)->connection == connection) {
        drop_peer *peer = *link; *link = peer->next; sodium_memzero(peer, sizeof(*peer)); free(peer); break;
    }
    LeaveCriticalSection(&server->peers_lock);
}
void drop_notify(drop_server *server, const char *user, const char *recipient) {
    drop_peer *peer; char authorized_user[DROP_ID_CAP], device[DROP_ID_CAP];
    EnterCriticalSection(&server->peers_lock);
    for (peer = server->peers; peer; peer = peer->next) {
        if ((!strcmp(peer->user, user) || !strcmp(peer->user, recipient)) &&
            drop_authorize(server, peer->token, authorized_user, device))
            mg_websocket_write(peer->connection, MG_WEBSOCKET_OPCODE_TEXT, "{\"type\":\"changed\"}", 18);
    }
    LeaveCriticalSection(&server->peers_lock);
}
void drop_notify_states(drop_server *server) {
    drop_peer *peer; sqlite3_stmt *s=NULL; char user[DROP_ID_CAP],device[DROP_ID_CAP];
    EnterCriticalSection(&server->peers_lock);
    for(peer=server->peers;peer;peer=peer->next) {
        sqlite3_int64 seq=0;
        if(!drop_authorize(server,peer->token,user,device)) continue;
        EnterCriticalSection(&server->database_lock);
        if(sqlite3_prepare_v2(server->database,"SELECT last_seq FROM state_sequences WHERE user_id=?1",-1,&s,NULL)==SQLITE_OK) {
            sqlite3_bind_text(s,1,peer->user,-1,SQLITE_TRANSIENT);
            if(sqlite3_step(s)==SQLITE_ROW) seq=sqlite3_column_int64(s,0);
        }
        sqlite3_finalize(s); s=NULL; LeaveCriticalSection(&server->database_lock);
        if(seq>peer->state_seq && mg_websocket_write(peer->connection,MG_WEBSOCKET_OPCODE_TEXT,"{\"type\":\"changed\"}",18)>0) peer->state_seq=seq;
    }
    LeaveCriticalSection(&server->peers_lock);
}
int drop_server_run(drop_server *server, const char *listen, const char *certificate) {
    char tls_listen[256],ws_timeout[32]; struct mg_callbacks callbacks; struct mg_context *context;
    const char *port_text; uint64_t port;
    const char *options[] = {"listening_ports", tls_listen, "ssl_certificate", certificate,
        "ssl_protocol_version", "4", "enable_keep_alive", "yes", "num_threads", "32",
        "tcp_nodelay", "1",
        "request_timeout_ms", "15000", "websocket_timeout_ms",ws_timeout, "max_request_size", "16384", "decode_url", "no", "enable_directory_listing", "no", NULL};
    sprintf_s(ws_timeout,sizeof(ws_timeout),"%u",server->connection_timeout_seconds*1000);
    if (!listen || strlen(listen) > 250 || strpbrk(listen, " ,;\r\n") ||
        sprintf_s(tls_listen, sizeof(tls_listen), "%ss", listen) < 0) return 0;
    port_text = strrchr(listen, ':'); port_text = port_text ? port_text + 1 : listen;
    if (!drop_u64_parse(port_text, &port) || port < 1 || port > 65535) return 0;
    if (!(mg_init_library(MG_FEATURES_SSL) & MG_FEATURES_SSL)) return 0;
    memset(&callbacks, 0, sizeof(callbacks)); callbacks.begin_request = api_request; callbacks.log_message = silent_log;
    /* begin_request handles every path, so no static file, CGI or admin routes are exposed. */
    context = mg_start(&callbacks, server, options);
    if (!context) { mg_exit_library(); return 0; }
    if(!drop_backup_start(server) || !drop_cloud_start(server)) { mg_stop(context); drop_backup_stop(server); mg_exit_library(); return 0; }
    mg_set_websocket_handler(context, "/api/v1/events", ws_connect, ws_ready, ws_data, ws_close, server);
    drop_service_ready(server);
    console_stop = server->stop_event; SetConsoleCtrlHandler(control_handler, TRUE);
    printf("Drop server %s ready on HTTPS %s\n", DROP_VERSION, listen); fflush(stdout);
    (void)drop_upload_cleanup(server,(uint64_t)time(NULL));
    while(WaitForSingleObject(server->stop_event,3600000)==WAIT_TIMEOUT) {
        drop_error cleanup=drop_upload_cleanup(server,(uint64_t)time(NULL));
        if(cleanup!=DROP_OK) fprintf(stderr,"upload_cleanup_failed error=%s\n",drop_error_name(cleanup));
    }
    SetConsoleCtrlHandler(control_handler, FALSE); console_stop = NULL;
    mg_stop(context); drop_backup_stop(server); drop_cloud_stop(server); mg_exit_library(); return 1;
}

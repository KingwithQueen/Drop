#include "server.h"
#include <civetweb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE console_stop;
static BOOL WINAPI control_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        if (console_stop) SetEvent(console_stop); return TRUE;
    }
    return FALSE;
}
static int send_json(struct mg_connection *connection, int status, cJSON *object) {
    char *body = cJSON_PrintUnformatted(object); const char *reason;
    if (!body) { mg_send_http_error(connection, 500, "allocation failed"); return 500; }
    reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" :
        status == 413 ? "Payload Too Large" : status == 404 ? "Not Found" : "Internal Server Error";
    mg_printf(connection, "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", status, reason, strlen(body));
    mg_write(connection, body, strlen(body));
    /* Login responses contain a token. Zero the serialized response before freeing. */
    sodium_memzero(body, strlen(body)); cJSON_free(body); return status;
}
static int error_json(struct mg_connection *connection, int status, drop_error error) {
    cJSON *object = cJSON_CreateObject(); int result;
    if (!object) { mg_send_http_error(connection, 500, "allocation failed"); return 500; }
    cJSON_AddStringToObject(object, "error", drop_error_name(error));
    result = send_json(connection, status, object); cJSON_Delete(object); return result;
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
static int api_request(struct mg_connection *connection) {
    const struct mg_request_info *request = mg_get_request_info(connection);
    drop_server *server = request->user_data; cJSON *object = NULL, *response = NULL;
    const char *path = request->local_uri, *token, *login, *password, *device, *name;
    char user_id[DROP_ID_CAP], device_id[DROP_ID_CAP]; int result;
    if (!request->is_ssl) return error_json(connection, 400, DROP_E_PROTOCOL);
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
static int silent_log(const struct mg_connection *connection, const char *message) {
    (void)connection; (void)message;
    /* Do not log raw HTTP messages, URLs, headers, or OpenSSL key material. */
    return 1;
}
int drop_server_run(drop_server *server, const char *listen, const char *certificate) {
    char tls_listen[256]; struct mg_callbacks callbacks; struct mg_context *context;
    const char *options[] = {"listening_ports", tls_listen, "ssl_certificate", certificate,
        "ssl_protocol_version", "4", "enable_keep_alive", "no", "num_threads", "8",
        "request_timeout_ms", "15000", "decode_url", "no", "enable_directory_listing", "no", NULL};
    if (!listen || strlen(listen) > 250 || strpbrk(listen, " ,;\r\n") ||
        sprintf_s(tls_listen, sizeof(tls_listen), "%ss", listen) < 0) return 0;
    if (!(mg_init_library(MG_FEATURES_SSL) & MG_FEATURES_SSL)) return 0;
    memset(&callbacks, 0, sizeof(callbacks)); callbacks.begin_request = api_request; callbacks.log_message = silent_log;
    /* begin_request handles every path, so no static file, CGI or admin routes are exposed. */
    context = mg_start(&callbacks, server, options);
    if (!context) { mg_exit_library(); return 0; }
    console_stop = server->stop_event; SetConsoleCtrlHandler(control_handler, TRUE);
    printf("Drop server %s ready on HTTPS %s\n", DROP_VERSION, listen); fflush(stdout);
    WaitForSingleObject(server->stop_event, INFINITE);
    SetConsoleCtrlHandler(control_handler, FALSE); console_stop = NULL;
    mg_stop(context); mg_exit_library(); return 1;
}

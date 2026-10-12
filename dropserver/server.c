/* CivetWeb 请求及 WebSocket 分派；HTTP/PAKE 解密上下文按线程隔离，HTTPS 直接使用 TLS。 */
#include "server.h"
#include "cloud.h"
#include <civetweb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 已认证事件连接及其方向密钥，链表和状态序号受 peers_lock 保护；关闭时先摘链再清零释放。 */
typedef struct drop_peer {
    struct mg_connection *connection;
    char user[DROP_ID_CAP], device[DROP_ID_CAP], token[DROP_TOKEN_CAP];
    sqlite3_int64 state_seq;
    int encrypted;
    drop_pake_keys keys;
    uint64_t tx_sequence, rx_sequence;
    struct drop_peer *next;
} drop_peer;

/* 一次 HTTP/PAKE 请求的栈上认证上下文，plain 借用解密缓冲，position 记录已读取位置。 */
typedef struct secure_request {
    struct mg_request_info info;
    drop_pake_keys keys;
    uint64_t sequence;
    unsigned char *plain;
    DWORD length, position;
    char token[65], context[4096];
    int binary;
} secure_request;
/* 每个请求线程独立持有的解密上下文，仅在 api_request 调用业务分派期间指向栈上对象，返回前必须清空。 */
static __declspec(thread) secure_request *当前认证请求;
/* 优先返回当前线程已经认证解密的请求元数据；TLS 或公开请求直接使用 CivetWeb 数据。 */
static const struct mg_request_info *request_info(struct mg_connection *c) {
    return 当前认证请求 ? &当前认证请求->info : mg_get_request_info(c);
}
/* 在认证请求上下文中提供解密正文的内容类型，其他头部按底层连接读取。 */
static const char *request_header(struct mg_connection *c, const char *name) {
    if (当前认证请求 && !_stricmp(name, "Content-Type")) return 当前认证请求->binary ? "application/octet-stream" : "application/json";
    return mg_get_header(c, name);
}
/* 从当前解密正文推进读取位置，否则读取底层连接；返回值沿用 CivetWeb 读取语义。 */
static int request_read(struct mg_connection *c, void *buffer, size_t size) {
    DWORD n;
    if (!当前认证请求) return mg_read(c, buffer, size);
    n = 当前认证请求->length - 当前认证请求->position;
    if (size < n) n = (DWORD)size;
    memcpy(buffer, 当前认证请求->plain + 当前认证请求->position, n);
    当前认证请求->position += n;
    return (int)n;
}
/* 按当前请求模式发送普通或 PAKE 认证响应，绑定状态与类型；临时帧在发送后清零释放。 */
static int send_bytes(struct mg_connection *connection, int status, int binary, const unsigned char *body, size_t size) {
    unsigned char *plain = NULL, *frame = NULL;
    DWORD length = 0;
    int result = 500;
    if (当前认证请求) {
        plain = malloc(size + 5);
        if (!plain) return 500;
        plain[0] = (unsigned char)status;
        plain[1] = (unsigned char)(status >> 8);
        plain[2] = (unsigned char)(status >> 16);
        plain[3] = (unsigned char)(status >> 24);
        plain[4] = (unsigned char)binary;
        memcpy(plain + 5, body, size);
        if (drop_pake_encrypt(&当前认证请求->keys, DROP_PAKE_HTTP_RX, 当前认证请求->sequence, 当前认证请求->context, plain, size + 5, &frame, &length)) {
            mg_printf(connection, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lu\r\nCache-Control: no-store\r\n\r\n", length);
            if (mg_write(connection, frame, length) == (int)length) result = status;
        }
        drop_pake_bytes_free(plain, size + 5);
        drop_pake_bytes_free(frame, length);
        return result;
    }
    mg_printf(connection, "HTTP/1.1 %d Response\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n", status,
        binary ? "application/octet-stream" : "application/json; charset=utf-8", size);
    if (mg_write(connection, body, size) == (int)size) result = status;
    return result;
}

/* 前台运行时供控制台回调置位的借用句柄；移除回调后清空，由服务器上下文负责关闭。 */
static HANDLE 控制台退出事件;
/* 将控制台结束事件转为服务器停止事件，让主循环和后台线程按正常路径退出。 */
static BOOL WINAPI control_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        if (控制台退出事件) SetEvent(控制台退出事件);
        return TRUE;
    }
    return FALSE;
}
/* 序列化 JSON 并发送协议响应，含令牌的临时序列化内容在释放前清零。 */
static int send_json(struct mg_connection *connection, int status, cJSON *object) {
    char *body = cJSON_PrintUnformatted(object);
    int result;
    if (!body) {
        mg_send_http_error(connection, 500, "allocation failed");
        return 500;
    }
    result = send_bytes(connection, status, 0, (unsigned char *)body, strlen(body));
    if (result != status) {
        fprintf(stderr, "response_write_failed status=%d\n", status);
    }
    /* 登录响应包含令牌，释放序列化正文前先清零。 */
    sodium_memzero(body, strlen(body));
    cJSON_free(body);
    return status;
}
/* 构造统一错误对象并返回指定 HTTP 状态，避免输出请求内容或秘密材料。 */
static int error_json(struct mg_connection *connection, int status, drop_error error) {
    cJSON *object = cJSON_CreateObject();
    int result;
    if (!object) {
        mg_send_http_error(connection, 500, "allocation failed");
        return 500;
    }
    cJSON_AddStringToObject(object, "error", drop_error_name(error));
    result = send_json(connection, status, object);
    cJSON_Delete(object);
    return result;
}
/* 将业务错误映射为接口 HTTP 状态，再通过统一错误响应发送。 */
static int api_error(struct mg_connection *connection, drop_error error) {
    return error_json(connection,
        error == DROP_E_AUTH           ? 401
            : error == DROP_E_CONFLICT ? 409
            : error == DROP_E_LIMIT    ? 413
            : error == DROP_E_ARGUMENT ? 400
            : error == DROP_E_SPACE    ? 507
                                       : 500,
        error);
}
/* 检查请求类型和大小后完整读取受限正文并严格解析；临时正文清零，成功对象由调用者释放。 */
static cJSON *read_json(struct mg_connection *connection) {
    const struct mg_request_info *request = request_info(connection);
    const char *type = request_header(connection, "Content-Type");
    size_t length, position = 0;
    char *body;
    cJSON *object;
    if (!type || strcmp(type, "application/json") || request->content_length <= 0 || request->content_length > DROP_MAX_JSON) return NULL;
    length = (size_t)request->content_length;
    body = malloc(length + 1);
    if (!body) return NULL;
    while (position < length) {
        int amount = request_read(connection, body + position, length - position);
        if (amount <= 0) {
            sodium_memzero(body, length);
            free(body);
            return NULL;
        }
        position += (size_t)amount;
    }
    body[length] = 0;
    object = drop_json_parse(body, length);
    sodium_memzero(body, length);
    free(body);
    return object;
}
/* 从已认证 PAKE 上下文或合法 Authorization 头读取令牌，返回借用指针，仅在当前请求内使用。 */
static const char *bearer(struct mg_connection *connection) {
    if (当前认证请求) return 当前认证请求->token;
    const char *header = mg_get_header(connection, "Authorization");
    if (!header || strncmp(header, "Bearer ", 7) || strlen(header + 7) != 64) return NULL;
    return header + 7;
}
/* 分派认证后的业务路由，按账号和设备校验权限；消息及状态提交成功后发送变化提示。 */
static int api_request_inner(struct mg_connection *connection) {
    const struct mg_request_info *request = request_info(connection);
    drop_server *server = request->user_data;
    cJSON *object = NULL, *response = NULL;
    const char *path = request->local_uri, *token, *login, *password, *device, *name;
    char user_id[DROP_ID_CAP], device_id[DROP_ID_CAP];
    int result;
    if (!path || !request->request_method) return error_json(connection, 400, DROP_E_ARGUMENT);
    if (!request->is_ssl && !当前认证请求 && strcmp(path, "/api/v1/server-info")) return error_json(connection, 401, DROP_E_AUTH);
    if (request->content_length > DROP_MAX_JSON) return error_json(connection, 413, DROP_E_LIMIT);
    if (!strcmp(path, "/api/v1/server-info") && !strcmp(request->request_method, "GET")) {
        response = cJSON_CreateObject();
        if (!response) return error_json(connection, 500, DROP_E_IO);
        cJSON_AddNumberToObject(response, "protocol_version", DROP_PROTOCOL_VERSION);
        cJSON_AddStringToObject(response, "version", DROP_VERSION);
        cJSON_AddBoolToObject(response, "message_deletion", 1);
        cJSON_AddStringToObject(response, "transport", request->is_ssl ? "tls" : DROP_PAKE_SUITE);
        cJSON_AddStringToObject(response, "server_id", server->keys.server_id);
        cJSON_AddStringToObject(response, "generation", server->keys.generation);
        result = send_json(connection, 200, response);
        cJSON_Delete(response);
        return result;
    }
    if (request->is_ssl && !strcmp(path, "/api/v1/login") && !strcmp(request->request_method, "POST")) {
        if (!(object = read_json(connection))) return error_json(connection, 400, DROP_E_ARGUMENT);
        login = drop_json_string(object, "login", 64);
        password = drop_json_string(object, "password", 1024);
        device = drop_json_string(object, "device_id", 32);
        name = drop_json_string(object, "device_name", 128);
        result = drop_login(server, login, password, device, name, &response);
        if (password) sodium_memzero((char *)password, strlen(password));
        cJSON_Delete(object);
        if (!result) return error_json(connection, 401, DROP_E_AUTH);
        result = send_json(connection, 200, response);
        token = drop_json_string(response, "token", 64);
        if (token) sodium_memzero((char *)token, strlen(token));
        cJSON_Delete(response);
        return result;
    }
    if (strncmp(path, "/api/v1/", 8)) return error_json(connection, 404, DROP_E_UNSUPPORTED);
    token = bearer(connection);
    if (!drop_authorize(server, token, user_id, device_id)) return error_json(connection, 401, DROP_E_AUTH);
    if (!strcmp(path, "/api/v1/uploads") && !strcmp(request->request_method, "POST")) {
        drop_error error;
        object = read_json(connection);
        if (!object) return api_error(connection, DROP_E_ARGUMENT);
        error = drop_upload_create(server, user_id, object, &response);
        cJSON_Delete(object);
        if (error != DROP_OK) return api_error(connection, error);
        result = send_json(connection, 200, response);
        cJSON_Delete(response);
        return result;
    }
    if (!strncmp(path, "/api/v1/uploads/", 16) || !strncmp(path, "/api/v1/attachments/", 20)) {
        int download = !strncmp(path, "/api/v1/attachments/", 20);
        const char *start = path + (download ? 20 : 16), *suffix;
        char id[DROP_ID_CAP];
        uint64_t index;
        drop_error error;
        if (strlen(start) < 32) return api_error(connection, DROP_E_ARGUMENT);
        memcpy(id, start, 32);
        id[32] = 0;
        suffix = start + 32;
        if (!drop_id_valid(id)) return api_error(connection, DROP_E_ARGUMENT);
        if (!download && !*suffix && !strcmp(request->request_method, "GET"))
            error = drop_upload_status(server, user_id, id, &response);
        else if (!download && !strcmp(suffix, "/complete") && !strcmp(request->request_method, "POST"))
            error = drop_upload_finish(server, user_id, id, &response);
        else if (!strncmp(suffix, "/chunks/", 8) && drop_u64_parse(suffix + 8, &index)) {
            unsigned char *bytes = NULL;
            DWORD length = 0;
            if (download && !strcmp(request->request_method, "GET")) {
                error = drop_attachment_chunk(server, user_id, id, index, &bytes, &length);
                if (error != DROP_OK) return api_error(connection, error);
                if (send_bytes(connection, 200, 1, bytes, length) != 200) fputs("attachment_write_failed\n", stderr);
                sodium_memzero(bytes, length);
                free(bytes);
                return 200;
            }
            if (download || strcmp(request->request_method, "PUT")) return api_error(connection, DROP_E_UNSUPPORTED);
            if (request->content_length <= 0 || request->content_length > DROP_CHUNK_SIZE || !request_header(connection, "Content-Type") ||
                strcmp(request_header(connection, "Content-Type"), "application/octet-stream"))
                return api_error(connection, DROP_E_ARGUMENT);
            length = (DWORD)request->content_length;
            bytes = malloc(length);
            if (!bytes) return api_error(connection, DROP_E_IO);
            {
                DWORD position = 0;
                while (position < length) {
                    int n = request_read(connection, bytes + position, length - position);
                    if (n <= 0) break;
                    position += (DWORD)n;
                }
                error = position == length ? drop_upload_chunk(server, user_id, id, index, bytes, length) : DROP_E_ARGUMENT;
            }
            sodium_memzero(bytes, length);
            free(bytes);
            if (error != DROP_OK) return api_error(connection, error);
            response = cJSON_CreateObject();
            if (!response || !cJSON_AddBoolToObject(response, "stored", 1)) {
                cJSON_Delete(response);
                return api_error(connection, DROP_E_IO);
            }
        } else
            return api_error(connection, DROP_E_ARGUMENT);
        if (error != DROP_OK) return api_error(connection, error);
        result = send_json(connection, 200, response);
        cJSON_Delete(response);
        return result;
    }
    if (!strcmp(path, "/api/v1/events") && !strcmp(request->request_method, "GET")) {
        const char *upgrade = mg_get_header(connection, "Upgrade");
        if (!upgrade || _stricmp(upgrade, "websocket")) return error_json(connection, 400, DROP_E_PROTOCOL);
        return 0; /* 只有此经过认证且已注册的路径进入 CivetWeb 的 WebSocket 处理器。 */
    }
    if (!strcmp(request->request_method, "POST") &&
        (!strcmp(path, "/api/v1/messages") || !strcmp(path, "/api/v1/sync") || !strcmp(path, "/api/v1/accounts") || !strcmp(path, "/api/v1/saved") ||
            !strcmp(path, "/api/v1/read") || !strcmp(path, "/api/v1/states") || !strcmp(path, "/api/v1/profiles") || !strcmp(path, "/api/v1/messages/delete") ||
            !strcmp(path, "/api/v1/deletions"))) {
        drop_error error;
        char recipient[DROP_ID_CAP] = {0};
        object = read_json(connection);
        if (!object) return error_json(connection, 400, DROP_E_ARGUMENT);
        if (!strcmp(path, "/api/v1/messages"))
            error = drop_message_submit(server, user_id, device_id, object, &response, recipient);
        else if (!strcmp(path, "/api/v1/messages/delete"))
            error = drop_message_delete(server, user_id, token, object, &response, recipient);
        else if (!strcmp(path, "/api/v1/deletions"))
            error = drop_deletions_read(server, user_id, object, &response);
        else if (!strcmp(path, "/api/v1/sync"))
            error = drop_sync_read(server, user_id, object, &response);
        else if (!strcmp(path, "/api/v1/saved"))
            error = drop_saved_submit(server, user_id, device_id, object, &response);
        else if (!strcmp(path, "/api/v1/read"))
            error = drop_read_submit(server, user_id, object, &response);
        else if (!strcmp(path, "/api/v1/states"))
            error = drop_states_read(server, user_id, object, &response);
        else if (!strcmp(path, "/api/v1/profiles"))
            error = drop_profiles_read(server, user_id, object, &response);
        else
            error = drop_account_lookup(server, object, &response);
        cJSON_Delete(object);
        if (error != DROP_OK) {
            cJSON_Delete(response);
            return api_error(connection, error);
        }
        result = send_json(connection, 200, response);
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "changed"))) drop_notify_states(server);
        cJSON_Delete(response);
        if (*recipient) drop_notify(server, user_id, recipient);
        return result;
    }
    if (!strcmp(path, "/api/v1/logout") && !strcmp(request->request_method, "POST")) {
        if (!drop_logout(server, token)) return error_json(connection, 500, DROP_E_DATABASE);
        response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "revoked", 1);
    } else if (!strcmp(path, "/api/v1/devices") && !strcmp(request->request_method, "GET")) {
        sqlite3_stmt *statement = NULL;
        cJSON *array;
        response = cJSON_CreateObject();
        array = cJSON_AddArrayToObject(response, "devices");
        EnterCriticalSection(&server->database_lock);
        if (sqlite3_prepare_v2(server->database, "SELECT device_id,name,revoked FROM devices WHERE user_id=?1 ORDER BY device_id LIMIT 100", -1, &statement, NULL) != SQLITE_OK) {
            LeaveCriticalSection(&server->database_lock);
            cJSON_Delete(response);
            return error_json(connection, 500, DROP_E_DATABASE);
        }
        sqlite3_bind_text(statement, 1, user_id, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(statement) == SQLITE_ROW) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "device_id", (const char *)sqlite3_column_text(statement, 0));
            cJSON_AddStringToObject(item, "name", (const char *)sqlite3_column_text(statement, 1));
            cJSON_AddBoolToObject(item, "revoked", sqlite3_column_int(statement, 2));
            cJSON_AddItemToArray(array, item);
        }
        sqlite3_finalize(statement);
        LeaveCriticalSection(&server->database_lock);
    } else
        return error_json(connection, 404, DROP_E_UNSUPPORTED);
    result = send_json(connection, 200, response);
    cJSON_Delete(response);
    return result;
}
/* 在 HTTP 模式处理受限握手或认证帧，在当前线程安装解密请求上下文；请求结束撤销指针并清零临时秘密。 */
static int api_request(struct mg_connection *connection) {
    const struct mg_request_info *request = mg_get_request_info(connection);
    DWORD thread = GetCurrentThreadId();
    int result, kind = 0, trace = (DROP_DIAGNOSTICS ? getenv("DROP_TEST_TRACE") : NULL) != NULL;
    const char *path = request->local_uri;
    secure_request secure = {0};
    unsigned char *frame = NULL, *opened = NULL;
    DWORD size = 0, length = 0, position = 0;
    drop_server *server = request->user_data;
    if (!request->is_ssl) {
        if (!path || !request->request_method || request->query_string) return error_json(connection, 400, DROP_E_ARGUMENT);
        if (!strcmp(path, "/api/v1/pake/events") && !strcmp(request->request_method, "GET")) return 0;
        if (!strcmp(path, "/api/v1/pake/parameters") || !strcmp(path, "/api/v1/pake/start") || !strcmp(path, "/api/v1/pake/finish")) {
            cJSON *input = NULL, *output = NULL;
            if (strcmp(request->request_method, "POST") || request->content_length > 8192 || !(input = read_json(connection))) return error_json(connection, 400, DROP_E_ARGUMENT);
            result = drop_pake_handshake(server, path, input, &output);
            cJSON_Delete(input);
            if (result != 200) {
                cJSON_Delete(output);
                return error_json(connection, result, result == 401 ? DROP_E_AUTH : result == 429 ? DROP_E_LIMIT : DROP_E_ARGUMENT);
            }
            result = send_json(connection, 200, output);
            cJSON_Delete(output);
            return result;
        }
        if (strcmp(path, "/api/v1/server-info") || request->content_length > 0) {
            const char *type = mg_get_header(connection, "Content-Type");
            if (!type || strcmp(type, "application/octet-stream") || request->content_length < DROP_PAKE_FRAME_OVERHEAD || request->content_length > DROP_PAKE_FRAME_LIMIT ||
                sprintf_s(secure.context, sizeof(secure.context), "%s:%s", request->request_method, path) < 0)
                return error_json(connection, 401, DROP_E_AUTH);
            size = (DWORD)request->content_length;
            frame = malloc(size);
            if (!frame) return error_json(connection, 500, DROP_E_IO);
            while (position < size) {
                int n = mg_read(connection, frame + position, size - position);
                if (n <= 0) break;
                position += (DWORD)n;
            }
            if (position != size || !drop_pake_server_open(server, secure.context, frame, size, &secure.keys, &secure.sequence, &opened, &length, secure.token) || length < 65 ||
                opened[0] > 1 || sodium_memcmp(opened + 1, secure.token, 64)) {
                drop_pake_bytes_free(frame, size);
                drop_pake_bytes_free(opened, length);
                sodium_memzero(&secure, sizeof(secure));
                return error_json(connection, 401, DROP_E_AUTH);
            }
            drop_pake_bytes_free(frame, size);
            secure.info = *request;
            secure.info.content_length = length - 65;
            secure.plain = opened + 65;
            secure.length = length - 65;
            secure.binary = opened[0];
            当前认证请求 = &secure;
        }
    }
    if (path)
        kind = !strcmp(path, "/api/v1/sync")            ? 1
            : !strcmp(path, "/api/v1/saved")            ? 2
            : !strcmp(path, "/api/v1/states")           ? 3
            : !strcmp(path, "/api/v1/read")             ? 4
            : !strcmp(path, "/api/v1/messages")         ? 5
            : !strncmp(path, "/api/v1/uploads", 15)     ? 6
            : !strncmp(path, "/api/v1/attachments", 19) ? 7
                                                        : 0;
    if (trace) fprintf(stderr, "request_start kind=%d thread=%lu tick=%llu\n", kind, thread, (unsigned long long)GetTickCount64());
    result = api_request_inner(connection);
    当前认证请求 = NULL;
    drop_pake_bytes_free(opened, length);
    sodium_memzero(&secure, sizeof(secure));
    if (trace) fprintf(stderr, "request_end kind=%d thread=%lu tick=%llu status=%d\n", kind, thread, (unsigned long long)GetTickCount64(), result);
    return result;
}
/* 屏蔽可能含请求头、正文或密钥的底层日志，业务诊断只记录受限状态信息。 */
static int silent_log(const struct mg_connection *connection, const char *message) {
    (void)connection;
    (void)message;
    /* 不记录原始 HTTP 内容、URL、请求头或 OpenSSL 密钥材料。 */
    return 1;
}
/* 在连接列表锁保护下向事件连接写文本或 PAKE 帧，使用独立发送序号并清理临时密文。 */
static int peer_write(drop_peer *peer, const char *text, size_t size) {
    unsigned char *frame = NULL;
    DWORD length = 0;
    int result;
    if (!peer->encrypted) return mg_websocket_write(peer->connection, MG_WEBSOCKET_OPCODE_TEXT, text, size);
    if (peer->tx_sequence == UINT64_MAX || !drop_pake_encrypt(&peer->keys, DROP_PAKE_WS_RX, ++peer->tx_sequence, "ws-v1", (const unsigned char *)text, size, &frame, &length))
        return 0;
    result = mg_websocket_write(peer->connection, MG_WEBSOCKET_OPCODE_BINARY, (const char *)frame, length);
    drop_pake_bytes_free(frame, length);
    return result;
}
/* 清理尚未移交到事件连接列表的连接私有状态，清零认证材料后释放。 */
static void connection_closed(const struct mg_connection *connection) {
    drop_peer *peer = mg_get_user_connection_data(connection);
    if (peer) {
        sodium_memzero(peer, sizeof(*peer));
        free(peer);
        mg_set_user_connection_data(connection, NULL);
    }
}
/* 在升级前验证 TLS 令牌或 PAKE 通道证明，拒绝无授权连接；PAKE 成功后保存待移交连接状态。 */
static int ws_connect(const struct mg_connection *connection, void *data) {
    drop_server *server = data;
    char user[DROP_ID_CAP], device[DROP_ID_CAP];
    const char *header = mg_get_header(connection, "Authorization");
    if (!mg_get_request_info(connection)->is_ssl) {
        unsigned char frame[109], *plain = NULL;
        DWORD length = 0;
        uint64_t sequence = 0;
        drop_peer *peer = calloc(1, sizeof(*peer));
        int ok = 0;
        header = mg_get_header(connection, "X-Drop-Channel");
        if (peer && drop_pake_hex(header, frame, sizeof(frame)) &&
            drop_pake_server_open(server, "GET:/api/v1/pake/events", frame, sizeof(frame), &peer->keys, &sequence, &plain, &length, peer->token) && length == 65 && !plain[0] &&
            !sodium_memcmp(plain + 1, peer->token, 64) && drop_authorize(server, peer->token, peer->user, peer->device)) {
            peer->encrypted = 1;
            drop_pake_channel_derive(&peer->keys, sequence);
            mg_set_user_connection_data(connection, peer);
            ok = 1;
        }
        drop_pake_bytes_free(plain, length);
        if (!ok && peer) {
            sodium_memzero(peer, sizeof(*peer));
            free(peer);
        }
        return !ok;
    }
    return !header || strncmp(header, "Bearer ", 7) || !drop_authorize(server, header + 7, user, device);
}
/* 将认证连接加入受锁保护的有限列表，并立即提示客户端补拉持久记录。 */
static void ws_ready(struct mg_connection *connection, void *data) {
    drop_server *server = data;
    drop_peer *peer = mg_get_user_connection_data(connection), *item;
    int count = 0;
    const char *token = bearer(connection);
    if (!peer) {
        peer = calloc(1, sizeof(*peer));
        if (!peer || !drop_authorize(server, token, peer->user, peer->device)) {
            free(peer);
            mg_close_connection(connection);
            return;
        }
        strcpy_s(peer->token, sizeof(peer->token), token);
    }
    mg_set_user_connection_data(connection, NULL);
    peer->connection = connection;
    EnterCriticalSection(&server->peers_lock);
    for (item = server->peers; item; item = item->next)
        ++count;
    if (count >= 16) {
        LeaveCriticalSection(&server->peers_lock);
        sodium_memzero(peer, sizeof(*peer));
        free(peer);
        mg_close_connection(connection);
        return;
    }
    peer->next = server->peers;
    server->peers = peer;
    if ((DROP_DIAGNOSTICS ? getenv("DROP_TEST_TRACE") : NULL)) fprintf(stderr, "ws_open tick=%llu\n", (unsigned long long)GetTickCount64());
    peer_write(peer, "{\"type\":\"changed\"}", 18);
    LeaveCriticalSection(&server->peers_lock);
}
/* 重新验证设备授权并处理完整心跳帧，PAKE 要求认证及连续序号；异常、撤销或关闭帧结束连接。 */
static int ws_data(struct mg_connection *connection, int bits, char *data, size_t length, void *opaque) {
    drop_server *server = opaque;
    char user[DROP_ID_CAP], device[DROP_ID_CAP];
    drop_peer *peer;
    int authorized = 0, opcode = bits & 15, result = 0;
    unsigned char *plain = NULL;
    DWORD size = 0;
    uint64_t sequence = 0;
    EnterCriticalSection(&server->peers_lock);
    for (peer = server->peers; peer; peer = peer->next)
        if (peer->connection == connection) {
            authorized = drop_authorize(server, peer->token, user, device);
            break;
        }
    if (!authorized || length > 1024 || !(bits & 128) || opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) goto done;
    if (opcode == MG_WEBSOCKET_OPCODE_PING) {
        result = mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_PONG, data, length) > 0;
        goto done;
    }
    if (peer->encrypted) {
        if (opcode != MG_WEBSOCKET_OPCODE_BINARY || !drop_pake_decrypt(&peer->keys, DROP_PAKE_WS_TX, "ws-v1", (unsigned char *)data, length, &sequence, &plain, &size) ||
            !sequence || sequence != peer->rx_sequence + 1)
            goto done;
        peer->rx_sequence = sequence;
        data = (char *)plain;
        length = size;
    } else if (opcode != MG_WEBSOCKET_OPCODE_TEXT)
        goto done;
    if (length != 20 || memcmp(data, "{\"type\":\"heartbeat\"}", 20)) goto done;
    if ((DROP_DIAGNOSTICS ? getenv("DROP_TEST_TRACE") : NULL)) fprintf(stderr, "ws_heartbeat tick=%llu\n", (unsigned long long)GetTickCount64());
    result = (DROP_DIAGNOSTICS && getenv("DROP_TEST_DROP_HEARTBEAT")) ? 1 : peer_write(peer, "{\"type\":\"heartbeat\"}", 20) > 0;
done:
    LeaveCriticalSection(&server->peers_lock);
    drop_pake_bytes_free(plain, size);
    return result;
}
/* 在连接列表锁内移除并清零连接，防止广播线程在关闭回调后访问已释放状态。 */
static void ws_close(const struct mg_connection *connection, void *data) {
    drop_server *server = data;
    drop_peer **link;
    /* CivetWeb 在获取通用关闭连接锁前调用本回调；连接列表锁防止广播线程继续使用本回调已释放的指针。 */
    EnterCriticalSection(&server->peers_lock);
    for (link = &server->peers; *link; link = &(*link)->next)
        if ((*link)->connection == connection) {
            drop_peer *peer = *link;
            *link = peer->next;
            sodium_memzero(peer, sizeof(*peer));
            free(peer);
            break;
        }
    LeaveCriticalSection(&server->peers_lock);
}
/* 向发送和接收账号的有效在线设备提示变化；通知只触发补拉，持久消息由增量接口提供。 */
void drop_notify(drop_server *server, const char *user, const char *recipient) {
    drop_peer *peer;
    char authorized_user[DROP_ID_CAP], device[DROP_ID_CAP];
    EnterCriticalSection(&server->peers_lock);
    for (peer = server->peers; peer; peer = peer->next) {
        if ((!strcmp(peer->user, user) || !strcmp(peer->user, recipient)) && drop_authorize(server, peer->token, authorized_user, device))
            peer_write(peer, "{\"type\":\"changed\"}", 18);
    }
    LeaveCriticalSection(&server->peers_lock);
}
/* 比较在线账号的持久状态序号，仅在状态推进后提示补拉，写成功才更新连接记录。 */
void drop_notify_states(drop_server *server) {
    drop_peer *peer;
    sqlite3_stmt *s = NULL;
    char user[DROP_ID_CAP], device[DROP_ID_CAP];
    EnterCriticalSection(&server->peers_lock);
    for (peer = server->peers; peer; peer = peer->next) {
        sqlite3_int64 seq = 0;
        if (!drop_authorize(server, peer->token, user, device)) continue;
        EnterCriticalSection(&server->database_lock);
        if (sqlite3_prepare_v2(server->database, "SELECT last_seq FROM state_sequences WHERE user_id=?1", -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, peer->user, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) == SQLITE_ROW) seq = sqlite3_column_int64(s, 0);
        }
        sqlite3_finalize(s);
        s = NULL;
        LeaveCriticalSection(&server->database_lock);
        if (seq > peer->state_seq && peer_write(peer, "{\"type\":\"changed\"}", 18) > 0) peer->state_seq = seq;
    }
    LeaveCriticalSection(&server->peers_lock);
}
/* 管理提交后只提示变化，不携带账户资料；撤销设备收到提示后立即补拉并得到认证失效。 */
void drop_notify_accounts(drop_server *server) {
    drop_peer *peer;
    EnterCriticalSection(&server->peers_lock);
    for (peer = server->peers; peer; peer = peer->next)
        peer_write(peer, "{\"type\":\"changed\"}", 18);
    LeaveCriticalSection(&server->peers_lock);
}
/* 按证书参数选择 HTTP/PAKE 或 HTTPS/TLS，注册受限路由及后台任务，等待退出并统一停止服务资源。 */
int drop_server_run(drop_server *server, const char *listen, const char *certificate) {
    char tls_listen[256], ws_timeout[32];
    struct mg_callbacks callbacks;
    struct mg_context *context;
    const char *port_text;
    uint64_t port;
    const char *options[] = {"listening_ports", tls_listen, "ssl_protocol_version", "4", "enable_keep_alive", "yes", "num_threads", "32", "tcp_nodelay", "1", "request_timeout_ms",
        "15000", "websocket_timeout_ms", ws_timeout, "max_request_size", "16384", "decode_url", "no", "enable_directory_listing", "no", NULL, NULL, NULL};
    sprintf_s(ws_timeout, sizeof(ws_timeout), "%u", server->connection_timeout_seconds * 1000);
    if (!listen || strlen(listen) > 250 || strpbrk(listen, " ,;\r\n") || sprintf_s(tls_listen, sizeof(tls_listen), "%s%s", listen, certificate ? "s" : "") < 0) return 0;
    if (certificate) {
        options[20] = "ssl_certificate";
        options[21] = certificate;
        options[22] = NULL;
    }
    port_text = strrchr(listen, ':');
    port_text = port_text ? port_text + 1 : listen;
    if (!drop_u64_parse(port_text, &port) || port < 1 || port > 65535) return 0;
    if (!(mg_init_library(MG_FEATURES_SSL) & MG_FEATURES_SSL)) return 0;
    if (!certificate && !drop_pake_server_start(server)) {
        mg_exit_library();
        return 0;
    }
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.begin_request = api_request;
    callbacks.log_message = silent_log;
    callbacks.connection_closed = connection_closed;
    /* 所有路径都经过 begin_request 分派，因此不暴露静态文件、CGI 或网络管理路由。 */
    context = mg_start(&callbacks, server, options);
    if (!context) {
        mg_exit_library();
        return 0;
    }
    if (!drop_admin_start(server) || !drop_backup_start(server) || !drop_cloud_start(server)) {
        drop_admin_stop(server);
        mg_stop(context);
        drop_backup_stop(server);
        mg_exit_library();
        return 0;
    }
    mg_set_websocket_handler(context, "/api/v1/events", ws_connect, ws_ready, ws_data, ws_close, server);
    if (!certificate) mg_set_websocket_handler(context, "/api/v1/pake/events", ws_connect, ws_ready, ws_data, ws_close, server);
    drop_service_ready(server);
    控制台退出事件 = server->stop_event;
    SetConsoleCtrlHandler(control_handler, TRUE);
    printf("Drop server %s ready on %s %s\n", DROP_VERSION, certificate ? "HTTPS" : "HTTP + PAKE", listen);
    fflush(stdout);
    (void)drop_upload_cleanup(server, (uint64_t)time(NULL));
    while (WaitForSingleObject(server->stop_event, 3600000) == WAIT_TIMEOUT) {
        drop_error cleanup = drop_upload_cleanup(server, (uint64_t)time(NULL));
        if (cleanup != DROP_OK) fprintf(stderr, "upload_cleanup_failed error=%s\n", drop_error_name(cleanup));
    }
    SetConsoleCtrlHandler(control_handler, FALSE);
    控制台退出事件 = NULL;
    drop_admin_stop(server);
    mg_stop(context);
    drop_backup_stop(server);
    drop_cloud_stop(server);
    mg_exit_library();
    return 1;
}

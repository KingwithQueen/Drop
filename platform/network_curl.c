/* libcurl HTTP 与 WebSocket 边界，TLS 验证及 PAKE 模式均保持显式，无重定向或证书错误回退。 */
#include "network.h"
#include <curl/curl.h>
#include <sodium.h>
#include <stdatomic.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>

static _Atomic(HANDLE) 退出事件;
static pthread_once_t curl初始化 = PTHREAD_ONCE_INIT;
static int curl就绪;
static pthread_key_t HTTP缓存键;

/* 缓存严格属于一个线程，不跨线程共享连接；线程退出时所有请求已完成，才释放空闲连接及 TLS 会话。 */
static void http_cache_free(void *context) {
    curl_share_cleanup(context);
}

/* curl 全局初始化在创建任何工作线程前发布；失败后请求保持不可用。 */
static void curl_ready(void) {
    curl就绪 = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK && pthread_key_create(&HTTP缓存键, http_cache_free) == 0;
}
/* 请求对象仍逐次销毁以清除认证头；仅复用同一线程的连接、DNS 和 TLS 会话，不缓存正文、密码或 Cookie。
   libcurl 负责按 origin 与 TLS 配置匹配可复用连接；WebSocket 拥有独立连接，不加入此缓存。
   依据 https://curl.se/libcurl/c/CURLSHOPT_SHARE.html，连接缓存不能在多个并发线程之间共享。 */
static CURLSH *http_cache(void) {
    CURLSH *cache = pthread_getspecific(HTTP缓存键);
    if (cache) return cache;
    cache = curl_share_init();
    if (!cache) return NULL;
    if (curl_share_setopt(cache, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT) != CURLSHE_OK || curl_share_setopt(cache, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS) != CURLSHE_OK ||
        curl_share_setopt(cache, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION) != CURLSHE_OK || pthread_setspecific(HTTP缓存键, cache)) {
        curl_share_cleanup(cache);
        return NULL;
    }
    return cache;
}
void drop_network_cancel_event(HANDLE event) {
    atomic_store(&退出事件, event);
}
static int cancelled(HANDLE event) {
    return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}
int drop_network_wait(DWORD milliseconds) {
    HANDLE event = atomic_load(&退出事件);
    uint64_t until = GetTickCount64() + milliseconds;
    while (GetTickCount64() < until) {
        struct timespec delay = {0, 20000000};
        if (cancelled(event)) return 0;
        nanosleep(&delay, NULL);
    }
    return !cancelled(event);
}
typedef struct response_buffer {
    unsigned char *data;
    size_t size;
    int limit, allocation_failed;
} response_buffer;
/* 回调严格限制响应大小，服务器声明的长度不能绕过实际接收上限。 */
static size_t receive_bytes(char *data, size_t unit, size_t count, void *context) {
    response_buffer *buffer = context;
    size_t length;
    unsigned char *grown;
    if (unit && count > SIZE_MAX / unit) {
        buffer->limit = 1;
        return 0;
    }
    length = unit * count;
    if (length > DROP_PAKE_FRAME_LIMIT - buffer->size) {
        buffer->limit = 1;
        return 0;
    }
    grown = realloc(buffer->data, buffer->size + length + 1);
    if (!grown) {
        buffer->allocation_failed = 1;
        return 0;
    }
    buffer->data = grown;
    memcpy(buffer->data + buffer->size, data, length);
    buffer->size += length;
    buffer->data[buffer->size] = 0;
    return length;
}
static int progress(void *context, curl_off_t total, curl_off_t received, curl_off_t upload, curl_off_t sent) {
    (void)context;
    (void)total;
    (void)received;
    (void)upload;
    (void)sent;
    return cancelled(atomic_load(&退出事件));
}
/* 用共同选项固定证书、超时和 origin；私有 CA 只增加明确的信任文件，不关闭主机名校验。 */
static CURL *request_new(const WCHAR *url, const WCHAR *path, const WCHAR *ca_file, char *address, char *ca, int websocket) {
    char root[8193], route[3073];
    drop_url parsed;
    CURL *request;
    size_t length;
    if (pthread_once(&curl初始化, curl_ready) || !curl就绪 || !drop_url_parse(url, &parsed) || !drop_to_utf8(url, root, sizeof(root)) ||
        !drop_to_utf8(path, route, sizeof(route)) || route[0] != '/' || route[1] == '/' || strpbrk(route, "\r\n?# ") || !drop_utf8_valid(route, strlen(route)))
        return NULL;
    length = strlen(root);
    if (length && root[length - 1] == '/') root[length - 1] = 0;
    if (sprintf_s(address, 12288, "%s%s", root, route) < 0) return NULL;
    if (websocket) {
        memmove(address + 2, address + 4, strlen(address + 4) + 1);
        address[0] = 'w';
        address[1] = 's';
    }
    if (!(request = curl_easy_init())) return NULL;
#define OPTION(name, value)                                                                                                                                                        \
    if (curl_easy_setopt(request, name, value) != CURLE_OK) goto failed
    if (!websocket) {
        CURLSH *cache = http_cache();
        if (!cache) goto failed;
        OPTION(CURLOPT_SHARE, cache);
    }
    OPTION(CURLOPT_URL, address);
    OPTION(CURLOPT_PROTOCOLS_STR, websocket ? "ws,wss" : "http,https");
    OPTION(CURLOPT_FOLLOWLOCATION, 0L);
    OPTION(CURLOPT_MAXREDIRS, 0L);
    OPTION(CURLOPT_NETRC, CURL_NETRC_IGNORED);
    OPTION(CURLOPT_HTTPAUTH, (long)CURLAUTH_NONE);
    OPTION(CURLOPT_SSL_VERIFYPEER, 1L);
    OPTION(CURLOPT_SSL_VERIFYHOST, 2L);
    OPTION(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    OPTION(CURLOPT_TIMEOUT_MS, websocket ? 15000L : 45000L);
    OPTION(CURLOPT_NOSIGNAL, 1L);
    OPTION(CURLOPT_NOPROGRESS, 0L);
    OPTION(CURLOPT_XFERINFOFUNCTION, progress);
    OPTION(CURLOPT_USERAGENT, "Drop/" DROP_VERSION);
    OPTION(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    if (ca_file && *ca_file) {
        if (!drop_to_utf8(ca_file, ca, 131072)) goto failed;
        OPTION(CURLOPT_CAINFO, ca);
    }
#ifdef __APPLE__
    else if (drop_bundle_ca(ca, 131072)) {
        OPTION(CURLOPT_CAINFO, ca);
    }
#endif
    return request;
failed:
    curl_easy_cleanup(request);
    return NULL;
#undef OPTION
}
static drop_error curl_error(CURLcode code) {
    if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE || code == CURLE_SSL_CONNECT_ERROR) return DROP_E_CERTIFICATE;
    if (code == CURLE_UNSUPPORTED_PROTOCOL || code == CURLE_NOT_BUILT_IN) return DROP_E_UNSUPPORTED;
    return code == CURLE_OK ? DROP_OK : DROP_E_NETWORK;
}
drop_error drop_http_bytes_raw(const WCHAR *url, const WCHAR *method, const WCHAR *path, const char *token, const unsigned char *body, DWORD body_size, int binary_request,
    int binary_response, unsigned char **response, DWORD *response_size, DWORD *status, const WCHAR *ca_file) {
    char address[12288], ca[131072], verb[16], authorization[128] = {0};
    CURL *request = NULL;
    struct curl_slist *headers = NULL, *added;
    response_buffer buffer = {0};
    CURLcode code;
    long http_status = 0;
    drop_url parsed;
    drop_error error = DROP_E_ARGUMENT;
    if (!response || !response_size || !status) return error;
    *response = NULL;
    *response_size = *status = 0;
    if (body_size > DROP_PAKE_FRAME_LIMIT || (body_size && !body) || !drop_url_parse(url, &parsed) || !drop_to_utf8(method, verb, sizeof(verb)) ||
        (strcmp(verb, "GET") && strcmp(verb, "POST") && strcmp(verb, "PUT") && strcmp(verb, "DELETE")) ||
        (token && (!parsed.secure || strlen(token) != 64 || strspn(token, "0123456789abcdef") != 64)))
        goto done;
    if (!(request = request_new(url, path, ca_file, address, ca, 0))) goto done;
#define HEADER(value)                                                                                                                                                              \
    do {                                                                                                                                                                           \
        added = curl_slist_append(headers, value);                                                                                                                                 \
        if (!added) {                                                                                                                                                              \
            error = DROP_E_IO;                                                                                                                                                     \
            goto done;                                                                                                                                                             \
        }                                                                                                                                                                          \
        headers = added;                                                                                                                                                           \
    } while (0)
    HEADER(binary_request ? "Content-Type: application/octet-stream" : "Content-Type: application/json");
    HEADER(binary_response ? "Accept: application/octet-stream" : "Accept: application/json");
    HEADER("Expect:");
    if (token) {
        sprintf_s(authorization, sizeof(authorization), "Authorization: Bearer %s", token);
        HEADER(authorization);
    }
#undef HEADER
#define OPTION(name, value)                                                                                                                                                        \
    if (curl_easy_setopt(request, name, value) != CURLE_OK) {                                                                                                                      \
        error = DROP_E_NETWORK;                                                                                                                                                    \
        goto done;                                                                                                                                                                 \
    }
    OPTION(CURLOPT_CUSTOMREQUEST, verb);
    OPTION(CURLOPT_HTTPHEADER, headers);
    OPTION(CURLOPT_WRITEFUNCTION, receive_bytes);
    OPTION(CURLOPT_WRITEDATA, &buffer);
    if (body || !strcmp(verb, "POST") || !strcmp(verb, "PUT")) {
        OPTION(CURLOPT_POSTFIELDS, body ? (const char *)body : "");
        OPTION(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_size);
    }
#undef OPTION
    code = curl_easy_perform(request);
#ifndef NDEBUG
    /* 调试时仅记录错误号和接收长度，不输出 URL、认证头、密码或响应正文。 */
    if (code != CURLE_OK) {
        curl_off_t expected = -1;
        (void)curl_easy_getinfo(request, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &expected);
        fprintf(stderr, "Transfer failure: curl=%d bytes=%zu expected=%lld\n", (int)code, buffer.size, (long long)expected);
    }
#endif
    error = buffer.limit ? DROP_E_LIMIT : buffer.allocation_failed ? DROP_E_IO : curl_error(code);
    if (error != DROP_OK) goto done;
    if (curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &http_status) || http_status < 100 || http_status > 599) {
        error = DROP_E_PROTOCOL;
        goto done;
    }
    if (!buffer.data && !(buffer.data = calloc(1, 1))) {
        error = DROP_E_IO;
        goto done;
    }
    *response = buffer.data;
    *response_size = (DWORD)buffer.size;
    *status = (DWORD)http_status;
    buffer.data = NULL;
done:
    if (buffer.data) {
        sodium_memzero(buffer.data, buffer.size);
        free(buffer.data);
    }
    if (request) curl_easy_cleanup(request);
    /* curl_slist 中的认证头也含令牌，释放前清零。 */
    for (added = headers; added; added = added->next)
        sodium_memzero(added->data, strlen(added->data));
    curl_slist_free_all(headers);
    sodium_memzero(authorization, sizeof(authorization));
    return error;
}
drop_error drop_http_bytes(const WCHAR *url, const WCHAR *method, const WCHAR *path, const char *token, const unsigned char *body, DWORD body_size, int binary_request,
    int binary_response, unsigned char **response, DWORD *response_size, DWORD *status, const WCHAR *ca_file) {
    drop_url address;
    if (!response || !response_size || !status || body_size > DROP_MAX_JSON || (body_size && !body) || !drop_url_parse(url, &address)) return DROP_E_ARGUMENT;
    if (address.secure) return drop_http_bytes_raw(url, method, path, token, body, body_size, binary_request, binary_response, response, response_size, status, ca_file);
    return drop_pake_client_http(url, method, path, token, body, body_size, binary_request, binary_response, response, response_size, status);
}
drop_error drop_http_json_ca(
    const WCHAR *url, const WCHAR *method, const WCHAR *path, const char *token, const cJSON *body, cJSON **response, DWORD *status, const WCHAR *ca_file) {
    char *serialized = body ? cJSON_PrintUnformatted(body) : NULL;
    unsigned char *bytes = NULL;
    DWORD length = 0;
    drop_error error = DROP_E_ARGUMENT;
    if (!response || !status || (body && !serialized)) goto done;
    *response = NULL;
    *status = 0;
    if (serialized && strlen(serialized) > DROP_MAX_JSON) {
        error = DROP_E_LIMIT;
        goto done;
    }
    error = drop_http_bytes(url, method, path, token, (unsigned char *)serialized, serialized ? (DWORD)strlen(serialized) : 0, 0, 0, &bytes, &length, status, ca_file);
    if (error == DROP_OK && !(*response = drop_json_parse((char *)bytes, length))) error = DROP_E_PROTOCOL;
done:
    if (serialized) {
        sodium_memzero(serialized, strlen(serialized));
        cJSON_free(serialized);
    }
    drop_pake_bytes_free(bytes, length);
    return error;
}
drop_error drop_http_json(const WCHAR *url, const WCHAR *method, const WCHAR *path, const char *token, const cJSON *body, cJSON **response, DWORD *status) {
    return drop_http_json_ca(url, method, path, token, body, response, status, NULL);
}
int drop_network_probe(const WCHAR *url) {
    cJSON *response = NULL, *protocol;
    DWORD status;
    drop_error error = drop_http_json(url, L"GET", L"/api/v1/server-info", NULL, NULL, &response, &status);
    protocol = cJSON_GetObjectItemCaseSensitive(response, "protocol_version");
    int ok = error == DROP_OK && status == 200 && cJSON_IsNumber(protocol) && protocol->valuedouble == DROP_PROTOCOL_VERSION &&
        drop_id_valid(drop_json_string(response, "server_id", 32)) && drop_id_valid(drop_json_string(response, "generation", 32));
    printf("Connection: %s\n", ok ? "protocol v1; login required" : drop_error_name(error == DROP_OK ? DROP_E_PROTOCOL : error));
    cJSON_Delete(response);
    return ok;
}

/* 一个 curl socket 仅由串行收发循环使用；引用允许共享连接，关闭和每个 curl 调用用同一锁排除。 */
struct drop_ws_io {
    CURL *request;
    pthread_mutex_t lock;
    atomic_uint references;
    int closed;
};
int drop_ws_attach(drop_ws *ws) {
    return ws && ws->io;
}
void drop_ws_retain(drop_ws *ws) {
    if (ws && ws->io) atomic_fetch_add(&ws->io->references, 1);
}
void drop_ws_release(drop_ws *ws) {
    struct drop_ws_io *io = ws->io;
    if (io && atomic_fetch_sub(&io->references, 1) == 1) {
        pthread_mutex_destroy(&io->lock);
        free(io);
    }
    sodium_memzero(ws, sizeof(*ws));
}
void drop_ws_close(drop_ws *ws) {
    if (ws->io) {
        pthread_mutex_lock(&ws->io->lock);
        ws->io->closed = 1;
        if (ws->io->request) curl_easy_cleanup(ws->io->request);
        ws->io->request = NULL;
        pthread_mutex_unlock(&ws->io->lock);
    }
    drop_ws_release(ws);
}
/* 保留地址和认证方式，HTTP 会话过期时仅重新执行 PAKE，绝不换用另一种传输。 */
static drop_error ws_open_once(const WCHAR *url, const char *token, drop_ws *ws, const WCHAR *ca_file) {
    drop_url parsed;
    WCHAR header[512] = {0};
    char address[12288], ca[131072], text[2049] = {0};
    struct curl_slist *headers = NULL;
    CURL *request = NULL;
    long status = 0;
    drop_error error = DROP_E_ARGUMENT;
    memset(ws, 0, sizeof(*ws));
    if (!drop_url_parse(url, &parsed) || !token || strlen(token) != 64 || strspn(token, "0123456789abcdef") != 64) goto done;
    if (parsed.secure)
        sprintf_s(text, sizeof(text), "Authorization: Bearer %s", token);
    else {
        if (!drop_pake_client_channel(url, token, &ws->keys, header) || !drop_to_utf8(header, text, sizeof(text))) {
            error = DROP_E_AUTH;
            goto done;
        }
        text[strcspn(text, "\r\n")] = 0;
        ws->encrypted = 1;
    }
    request = request_new(url, parsed.secure ? L"/api/v1/events" : L"/api/v1/pake/events", ca_file, address, ca, 1);
    if (!request || !(headers = curl_slist_append(NULL, text))) goto done;
    if (curl_easy_setopt(request, CURLOPT_HTTPHEADER, headers) || curl_easy_setopt(request, CURLOPT_CONNECT_ONLY, 2L)) {
        error = DROP_E_NETWORK;
        goto done;
    }
    error = curl_error(curl_easy_perform(request));
    curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &status);
    if (status == 401) error = DROP_E_AUTH;
    if (error != DROP_OK) goto done;
    if (status != 101) {
        error = DROP_E_PROTOCOL;
        goto done;
    }
    ws->io = calloc(1, sizeof(*ws->io));
    if (!ws->io) {
        error = DROP_E_IO;
        goto done;
    }
    if (pthread_mutex_init(&ws->io->lock, NULL)) {
        free(ws->io);
        ws->io = NULL;
        error = DROP_E_IO;
        goto done;
    }
    atomic_init(&ws->io->references, 1);
    ws->io->request = request;
    request = NULL;
done:
    if (request) curl_easy_cleanup(request);
    if (headers) sodium_memzero(headers->data, strlen(headers->data));
    curl_slist_free_all(headers);
    sodium_memzero(header, sizeof(header));
    sodium_memzero(text, sizeof(text));
    if (error != DROP_OK) sodium_memzero(ws, sizeof(*ws));
    return error;
}
drop_error drop_ws_open(const WCHAR *url, const char *token, drop_ws *ws, const WCHAR *ca_file) {
    drop_error error = ws_open_once(url, token, ws, ca_file);
    drop_url parsed;
    if (error == DROP_E_AUTH && drop_url_parse(url, &parsed) && !parsed.secure) {
        drop_pake_client_expire(url, token);
        error = ws_open_once(url, token, ws, ca_file);
    }
    return error;
}
/* 等待 socket 时释放 curl 锁，每 100ms 检查取消；同一连接的收发由调用者串行驱动。 */
static int socket_wait(struct drop_ws_io *io, short events, HANDLE cancel) {
    curl_socket_t socket = CURL_SOCKET_BAD;
    struct pollfd p;
    if (cancelled(cancel) || cancelled(atomic_load(&退出事件))) return 0;
    pthread_mutex_lock(&io->lock);
    if (!io->closed) curl_easy_getinfo(io->request, CURLINFO_ACTIVESOCKET, &socket);
    pthread_mutex_unlock(&io->lock);
    if (socket == CURL_SOCKET_BAD) return 0;
    p.fd = socket;
    p.events = events;
    p.revents = 0;
    return poll(&p, 1, 100) >= 0 || errno == EINTR;
}
DWORD drop_ws_receive(drop_ws *ws, void *buffer, DWORD capacity, DWORD *length, WINHTTP_WEB_SOCKET_BUFFER_TYPE *type, HANDLE cancel) {
    struct drop_ws_io *io = ws->io;
    unsigned char *wire = NULL, *plain = NULL;
    size_t used = 0, limit = (size_t)capacity + DROP_PAKE_FRAME_OVERHEAD;
    DWORD plain_size = 0, result = ERROR_INVALID_DATA;
    uint64_t sequence = 0, deadline = GetTickCount64() + 90000;
    unsigned message_type = 0;
    if (!io || !buffer || !length || !type || !capacity || limit > DROP_PAKE_FRAME_LIMIT || !(wire = malloc(limit))) return result;
    *length = 0;
    while (GetTickCount64() < deadline) {
        const struct curl_ws_frame *meta = NULL;
        size_t n = 0;
        unsigned flags;
        curl_off_t remaining;
        CURLcode code;
        if (cancelled(cancel) || cancelled(atomic_load(&退出事件))) {
            result = ERROR_OPERATION_ABORTED;
            goto done;
        }
        pthread_mutex_lock(&io->lock);
        code = io->closed ? CURLE_GOT_NOTHING : curl_ws_recv(io->request, wire + used, limit - used, &n, &meta);
        flags = meta ? (unsigned)meta->flags : 0;
        remaining = meta ? meta->bytesleft : 0;
        pthread_mutex_unlock(&io->lock);
        if (code == CURLE_AGAIN) {
            if (!socket_wait(io, POLLIN, cancel)) goto done;
            continue;
        }
        if (code != CURLE_OK) goto done;
        if (flags & CURLWS_CLOSE) {
            *type = WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE;
            result = NO_ERROR;
            goto done;
        }
        if (flags & (CURLWS_PING | CURLWS_PONG)) continue;
        if (!message_type) message_type = flags & (CURLWS_TEXT | CURLWS_BINARY);
        if (!message_type || message_type != (flags & (CURLWS_TEXT | CURLWS_BINARY))) goto done;
        used += n;
        if (remaining > (curl_off_t)(limit - used) || (used == limit && (remaining || (flags & CURLWS_CONT)))) goto done;
        if (remaining || (flags & CURLWS_CONT)) continue;
        if (ws->encrypted) {
            if (message_type != CURLWS_BINARY || ws->rx_sequence == UINT64_MAX ||
                !drop_pake_decrypt(&ws->keys, DROP_PAKE_WS_RX, "ws-v1", wire, used, &sequence, &plain, &plain_size) || sequence != ws->rx_sequence + 1 || plain_size > capacity)
                goto done;
            ws->rx_sequence = sequence;
            memcpy(buffer, plain, plain_size);
            *length = plain_size;
        } else {
            if (message_type != CURLWS_TEXT || used > capacity) goto done;
            memcpy(buffer, wire, used);
            *length = (DWORD)used;
        }
        *type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        result = NO_ERROR;
        goto done;
    }
    result = ERROR_TIMEOUT;
done:
    drop_pake_bytes_free(plain, plain_size);
    drop_pake_bytes_free(wire, limit);
    return result;
}
DWORD drop_ws_send(drop_ws *ws, const void *buffer, DWORD length, HANDLE cancel) {
    unsigned char *frame = NULL;
    DWORD size = length, result = ERROR_INVALID_DATA;
    const unsigned char *bytes = buffer;
    size_t sent = 0;
    uint64_t deadline = GetTickCount64() + 15000;
    if (!ws->io || !buffer || length > DROP_MAX_JSON) return result;
    if (ws->encrypted) {
        if (ws->tx_sequence == UINT64_MAX || !drop_pake_encrypt(&ws->keys, DROP_PAKE_WS_TX, ++ws->tx_sequence, "ws-v1", buffer, length, &frame, &size)) goto done;
        bytes = frame;
    }
    while (GetTickCount64() < deadline) {
        size_t n = 0;
        CURLcode code;
        if (cancelled(cancel) || cancelled(atomic_load(&退出事件))) {
            result = ERROR_OPERATION_ABORTED;
            goto done;
        }
        pthread_mutex_lock(&ws->io->lock);
        code = ws->io->closed ? CURLE_GOT_NOTHING : curl_ws_send(ws->io->request, bytes + sent, size - sent, &n, 0, ws->encrypted ? CURLWS_BINARY : CURLWS_TEXT);
        pthread_mutex_unlock(&ws->io->lock);
        sent += n;
        if (code != CURLE_OK && code != CURLE_AGAIN) goto done;
        if (sent == size) {
            result = NO_ERROR;
            goto done;
        }
        if (!socket_wait(ws->io, POLLOUT, cancel)) goto done;
    }
    result = ERROR_TIMEOUT;
done:
    drop_pake_bytes_free(frame, size);
    return result;
}

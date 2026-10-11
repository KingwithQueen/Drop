#include "network.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 同一把锁串行处理 HTTP 帧及重新认证；WebSocket 使用独立密钥和序号。缓存凭据只来自已解开的 DPAPI 文件。 */
/* 从 DPAPI 凭据加载的进程内 PAKE 缓存槽位，HTTP 会话密钥和序号受凭据缓存锁保护。 */
typedef struct credential {
    WCHAR url[2049];
    char login[65], server[33], device[33], token[65];
    unsigned char secret[32], salt[16];
    drop_pake_keys keys;
    uint64_t sequence, expires;
} credential;
/* 保护下方凭据缓存，并串行执行会话认证及 HTTP 请求；WebSocket 使用独立子密钥和序号。 */
static SRWLOCK 凭据缓存锁 = SRWLOCK_INIT;
/* 仅保存当前进程已解开的 DPAPI 凭据，按服务器地址和令牌区分；移除槽位时清零全部秘密。 */
static credential 凭据缓存[16];
/* 在调用者已持有凭据缓存锁时按地址和令牌查找会话，返回指针仅在锁内使用。 */
static credential *find(const WCHAR *url, const char *token, int create) {
    unsigned i;
    credential *empty = NULL;
    for (i = 0; i < 16; i++) {
        if (!凭据缓存[i].url[0]) {
            if (!empty) empty = &凭据缓存[i];
            continue;
        }
        if (!wcscmp(凭据缓存[i].url, url) && token && !strcmp(凭据缓存[i].token, token)) return &凭据缓存[i];
    }
    return create ? empty : NULL;
}
/* 将已解开的 DPAPI 凭据绑定到受锁保护的有限缓存；复用同地址和令牌的已有槽位，临时秘密始终清零。 */
int drop_pake_client_bind(const WCHAR *url, const cJSON *json) {
    credential c = {0}, *slot;
    int ok = 0;
    drop_url address;
    const char *login = drop_json_string(json, "pake_login", 64), *server = drop_json_string(json, "server_id", 32), *device = drop_json_string(json, "device_id", 32),
               *token = drop_json_string(json, "token", 64);
    if (!drop_url_parse(url, &address)) return 0;
    if (address.secure) return 1;
    if (!login || !drop_id_valid(server) || !drop_id_valid(device) || !token || strlen(token) != 64 || !drop_pake_hex(drop_json_string(json, "pake_secret", 64), c.secret, 32) ||
        !drop_pake_hex(drop_json_string(json, "pake_salt", 32), c.salt, 16))
        goto done;
    wcscpy_s(c.url, 2049, url);
    strcpy_s(c.login, 65, login);
    strcpy_s(c.server, 33, server);
    strcpy_s(c.device, 33, device);
    strcpy_s(c.token, 65, token);
    AcquireSRWLockExclusive(&凭据缓存锁);
    slot = find(url, token, 1);
    if (slot) {
        if (!slot->url[0]) *slot = c;
        ok = 1;
    }
    ReleaseSRWLockExclusive(&凭据缓存锁);
done:
    sodium_memzero(&c, sizeof(c));
    return ok;
}
/* 移除指定地址和令牌的缓存凭据，同时清零会话密钥及认证材料。 */
void drop_pake_client_forget(const WCHAR *url, const char *token) {
    credential *c;
    AcquireSRWLockExclusive(&凭据缓存锁);
    c = find(url, token, 0);
    if (c) sodium_memzero(c, sizeof(*c));
    ReleaseSRWLockExclusive(&凭据缓存锁);
}
/* 只使缓存传输会话失效，保留受保护凭据解开的认证材料以便重新握手。 */
void drop_pake_client_expire(const WCHAR *url, const char *token) {
    credential *c;
    AcquireSRWLockExclusive(&凭据缓存锁);
    c = find(url, token, 0);
    if (c) c->expires = 0;
    ReleaseSRWLockExclusive(&凭据缓存锁);
}
/* 调用未携带令牌的 PAKE 握手接口，解析受限 JSON；成功结果由调用者释放。 */
static drop_error public_json(const WCHAR *url, const WCHAR *path, cJSON *body, cJSON **out, DWORD *status) {
    char *text = cJSON_PrintUnformatted(body);
    unsigned char *bytes = NULL;
    DWORD size = 0;
    drop_error error = DROP_E_IO;
    *out = NULL;
    if (text) error = drop_http_bytes_raw(url, L"POST", path, NULL, (unsigned char *)text, (DWORD)strlen(text), 0, 0, &bytes, &size, status, NULL);
    if (error == DROP_OK && !(*out = drop_json_parse((char *)bytes, size))) error = DROP_E_PROTOCOL;
    if (text) {
        sodium_memzero(text, strlen(text));
        cJSON_free(text);
    }
    drop_pake_bytes_free(bytes, size);
    return error;
}
/* 执行 SRP 双向认证并验证服务器证明，派生方向密钥后认证完成帧；失败清理临时秘密，不发送明文密码。 */
static drop_error authenticate(credential *c, const char *password, cJSON **result) {
    cJSON *request = NULL, *response = NULL, *metadata = NULL, *finish = NULL;
    struct SRPUser *user = NULL;
    unsigned char salt[32], public_bytes[512], proof[32], *frame = NULL, *plain = NULL;
    const unsigned char *A = NULL, *M = NULL, *shared = NULL;
    unsigned A_size = 0, M_size = 0;
    int shared_size = 0;
    DWORD status = 0, frame_size = 0, plain_size = 0;
    uint64_t sequence = 0;
    char identity[128], hex[1025], server_id[33], session[33], *text = NULL, *frame_hex = NULL;
    const char *value;
    drop_error error = DROP_E_AUTH;
    *result = NULL;
    request = cJSON_CreateObject();
    if (!request || !drop_srp_initialize()) {
        error = DROP_E_IO;
        goto done;
    }
    cJSON_AddStringToObject(request, "login", c->login);
    error = public_json(c->url, L"/api/v1/pake/parameters", request, &response, &status);
    if (error != DROP_OK) goto done;
    error = DROP_E_AUTH;
    value = drop_json_string(response, "server_id", 32);
    if (status != 200 || !drop_id_valid(value) || strcmp(drop_json_string(response, "suite", 128) ? drop_json_string(response, "suite", 128) : "", DROP_PAKE_SUITE) ||
        !drop_pake_hex(drop_json_string(response, "salt", 32), salt, 16))
        goto done;
    strcpy_s(server_id, 33, value);
    if (password) {
        memcpy(c->salt, salt, 16);
        if (!drop_pake_password(password, c->salt, c->secret)) goto done;
    } else if (strcmp(c->server, server_id) || sodium_memcmp(c->salt, salt, 16))
        goto done;
    if (!drop_pake_identity(server_id, c->login, identity)) goto done;
    user = srp_user_new(SRP_SHA256, SRP_NG_4096, identity, c->secret, 32, NULL, NULL, 1);
    if (!user) {
        error = DROP_E_IO;
        goto done;
    }
    srp_user_start_authentication(user, &value, &A, &A_size);
    if (A_size <= 0 || !drop_srp_public_valid(A, (size_t)A_size)) goto done;
    sodium_bin2hex(hex, sizeof(hex), A, (size_t)A_size);
    cJSON_AddStringToObject(request, "A", hex);
    cJSON_Delete(response);
    response = NULL;
    error = public_json(c->url, L"/api/v1/pake/start", request, &response, &status);
    if (error != DROP_OK) goto done;
    error = DROP_E_AUTH;
    value = drop_json_string(response, "session_id", 32);
    if (status != 200 || !drop_id_valid(value)) goto done;
    strcpy_s(session, 33, value);
    value = drop_json_string(response, "B", 1024);
    if (!value || !strlen(value) || strlen(value) % 2 || !drop_pake_hex(value, public_bytes, strlen(value) / 2) || !drop_srp_public_valid(public_bytes, strlen(value) / 2) ||
        !drop_pake_hex(drop_json_string(response, "srp_salt", 64), salt, 32))
        goto done;
    srp_user_process_challenge(user, salt, 32, public_bytes, (unsigned)(strlen(value) / 2), &M, &M_size);
    shared = srp_user_get_session_key(user, &shared_size);
    if (!M || M_size != 32 || shared_size != 32 || !drop_pake_derive(&c->keys, shared, server_id, session)) goto done;
    metadata = cJSON_CreateObject();
    finish = cJSON_CreateObject();
    if (!metadata || !finish) {
        error = DROP_E_IO;
        goto done;
    }
    cJSON_AddStringToObject(metadata, "device_id", c->device);
    cJSON_AddStringToObject(metadata, "device_name", "Drop Windows");
    if (!password) cJSON_AddStringToObject(metadata, "token", c->token);
    text = cJSON_PrintUnformatted(metadata);
    if (!text || !drop_pake_encrypt(&c->keys, DROP_PAKE_FINISH_TX, 0, "pake-finish-v1", (unsigned char *)text, strlen(text), &frame, &frame_size)) goto done;
    frame_hex = malloc((size_t)frame_size * 2 + 1);
    if (!frame_hex) {
        error = DROP_E_IO;
        goto done;
    }
    sodium_bin2hex(frame_hex, (size_t)frame_size * 2 + 1, frame, frame_size);
    sodium_bin2hex(hex, sizeof(hex), M, 32);
    cJSON_AddStringToObject(finish, "session_id", session);
    cJSON_AddStringToObject(finish, "proof", hex);
    cJSON_AddStringToObject(finish, "payload", frame_hex);
    cJSON_Delete(response);
    response = NULL;
    error = public_json(c->url, L"/api/v1/pake/finish", finish, &response, &status);
    if (error != DROP_OK) goto done;
    error = DROP_E_AUTH;
    if (status != 200 || !drop_pake_hex(drop_json_string(response, "proof", 64), proof, 32)) goto done;
    srp_user_verify_session(user, proof);
    if (!srp_user_is_authenticated(user)) goto done;
    value = drop_json_string(response, "payload", 8192);
    drop_pake_bytes_free(frame, frame_size);
    frame = NULL;
    if (!value || strlen(value) % 2) goto done;
    frame_size = (DWORD)(strlen(value) / 2);
    frame = malloc(frame_size);
    if (!frame || !drop_pake_hex(value, frame, frame_size) ||
        !drop_pake_decrypt(&c->keys, DROP_PAKE_FINISH_RX, "pake-finish-v1", frame, frame_size, &sequence, &plain, &plain_size) || sequence ||
        !(*result = drop_json_parse((char *)plain, plain_size)))
        goto done;
    value = drop_json_string(*result, "token", 64);
    if (!value || strlen(value) != 64 || strcmp(drop_json_string(*result, "server_id", 32) ? drop_json_string(*result, "server_id", 32) : "", server_id) ||
        strcmp(drop_json_string(*result, "device_id", 32) ? drop_json_string(*result, "device_id", 32) : "", c->device) || (!password && strcmp(value, c->token)))
        goto done;
    strcpy_s(c->token, 65, value);
    strcpy_s(c->server, 33, server_id);
    c->sequence = 0;
    c->expires = GetTickCount64() + 3300000;
    if (password) {
        sodium_bin2hex(hex, sizeof(hex), c->secret, 32);
        cJSON_AddStringToObject(*result, "pake_secret", hex);
        sodium_bin2hex(hex, sizeof(hex), c->salt, 16);
        cJSON_AddStringToObject(*result, "pake_salt", hex);
        cJSON_AddStringToObject(*result, "pake_login", c->login);
    }
    error = DROP_OK;
done:
    if (error != DROP_OK) {
        c->expires = 0;
        cJSON_Delete(*result);
        *result = NULL;
    }
    if (user) srp_user_delete(user);
    if (text) {
        sodium_memzero(text, strlen(text));
        cJSON_free(text);
    }
    if (metadata) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(metadata, "token");
        if (cJSON_IsString(t)) sodium_memzero(t->valuestring, strlen(t->valuestring));
    }
    cJSON_Delete(metadata);
    cJSON_Delete(finish);
    cJSON_Delete(request);
    cJSON_Delete(response);
    free(frame_hex);
    drop_pake_bytes_free(frame, frame_size);
    drop_pake_bytes_free(plain, plain_size);
    sodium_memzero(hex, sizeof(hex));
    sodium_memzero(proof, sizeof(proof));
    return error;
}
/* 在缓存锁保护下保证凭据具备可用会话；已有会话直接复用，否则重新完成双向认证。 */
static drop_error ensure(credential *c) {
    cJSON *response = NULL;
    drop_error error;
    if (c->expires > GetTickCount64()) return DROP_OK;
    error = authenticate(c, NULL, &response);
    if (response) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(response, "token");
        if (cJSON_IsString(t)) sodium_memzero(t->valuestring, strlen(t->valuestring));
    }
    cJSON_Delete(response);
    return error;
}
/* 串行认证并加密 HTTP 请求，将方法、路由和序号绑定到帧；验证响应后返回正文，失败保留明确错误。 */
drop_error drop_pake_client_http(const WCHAR *url, const WCHAR *method, const WCHAR *path, const char *token, const unsigned char *body, DWORD size, int binary_request,
    int binary_response, unsigned char **response, DWORD *response_size, DWORD *status) {
    credential *c, login = {0};
    cJSON *json = NULL, *answer = NULL;
    unsigned char *plain = NULL, *frame = NULL, *wire = NULL, *opened = NULL;
    DWORD frame_size = 0, wire_size = 0, opened_size = 0;
    uint64_t sequence = 0, received = 0;
    char context[4096], wide_method[16], wide_path[3073];
    drop_error error = DROP_E_AUTH;
    unsigned attempt;
    *response = NULL;
    *response_size = 0;
    *status = 0;
    if (!token && !wcscmp(path, L"/api/v1/login") && !wcscmp(method, L"POST")) {
        const char *name, *password, *device;
        char *text = NULL;
        json = drop_json_parse((char *)body, size);
        name = drop_json_string(json, "login", 64);
        password = drop_json_string(json, "password", 1024);
        device = drop_json_string(json, "device_id", 32);
        if (!name || !password || !drop_id_valid(device)) goto done;
        wcscpy_s(login.url, 2049, url);
        strcpy_s(login.login, 65, name);
        strcpy_s(login.device, 33, device);
        error = authenticate(&login, password, &answer);
        if (error == DROP_OK) {
            text = cJSON_PrintUnformatted(answer);
            if (!text)
                error = DROP_E_IO;
            else {
                *response = (unsigned char *)text;
                *response_size = (DWORD)strlen(text);
                *status = 200;
            }
        }
        goto done;
    }
    if (!token && !wcscmp(path, L"/api/v1/server-info") && !wcscmp(method, L"GET"))
        return drop_http_bytes_raw(url, method, path, NULL, NULL, 0, 0, 0, response, response_size, status, NULL);
    if (!token || strlen(token) != 64 || size > DROP_MAX_JSON || !drop_to_utf8(method, wide_method, sizeof(wide_method)) || !drop_to_utf8(path, wide_path, sizeof(wide_path)) ||
        sprintf_s(context, sizeof(context), "%s:%s", wide_method, wide_path) < 0)
        goto done;
    AcquireSRWLockExclusive(&凭据缓存锁);
    c = find(url, token, 0);
    if (!c) {
        ReleaseSRWLockExclusive(&凭据缓存锁);
        goto done;
    }
    plain = malloc((size_t)size + 65);
    if (!plain) {
        error = DROP_E_IO;
        goto unlock;
    }
    plain[0] = (unsigned char)(binary_request ? 1 : 0);
    memcpy(plain + 1, token, 64);
    if (size) memcpy(plain + 65, body, size);
    for (attempt = 0; attempt < 2; attempt++) {
        error = ensure(c);
        if (error != DROP_OK) break;
        if (c->sequence == UINT64_MAX) {
            error = DROP_E_PROTOCOL;
            break;
        }
        sequence = ++c->sequence;
        if (!drop_pake_encrypt(&c->keys, DROP_PAKE_HTTP_TX, sequence, context, plain, (size_t)size + 65, &frame, &frame_size)) {
            error = DROP_E_IO;
            break;
        }
        error = drop_http_bytes_raw(url, method, path, NULL, frame, frame_size, 1, 1, &wire, &wire_size, status, NULL);
        drop_pake_bytes_free(frame, frame_size);
        frame = NULL;
        if (error != DROP_OK) break;
        if (*status == 401 && !attempt) {
            c->expires = 0;
            drop_pake_bytes_free(wire, wire_size);
            wire = NULL;
            continue;
        }
        if (*status != 200 || !drop_pake_decrypt(&c->keys, DROP_PAKE_HTTP_RX, context, wire, wire_size, &received, &opened, &opened_size) || received != sequence ||
            opened_size < 5 || opened[4] > 1) {
            error = DROP_E_AUTH;
            break;
        }
        *status = (DWORD)opened[0] | ((DWORD)opened[1] << 8) | ((DWORD)opened[2] << 16) | ((DWORD)opened[3] << 24);
        if (*status < 100 || *status > 599 || ((*status == 200 && binary_response) ? 1 : 0) != opened[4]) {
            error = DROP_E_PROTOCOL;
            break;
        }
        *response_size = opened_size - 5;
        *response = malloc((size_t)*response_size + 1);
        if (!*response) {
            error = DROP_E_IO;
            break;
        }
        memcpy(*response, opened + 5, *response_size);
        (*response)[*response_size] = 0;
        error = DROP_OK;
        break;
    }
unlock:
    ReleaseSRWLockExclusive(&凭据缓存锁);
done:
    if (json) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(json, "password");
        if (cJSON_IsString(p)) sodium_memzero(p->valuestring, strlen(p->valuestring));
    }
    if (answer) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(answer, "pake_secret"), *t = cJSON_GetObjectItemCaseSensitive(answer, "token");
        if (cJSON_IsString(p)) sodium_memzero(p->valuestring, strlen(p->valuestring));
        if (cJSON_IsString(t)) sodium_memzero(t->valuestring, strlen(t->valuestring));
    }
    cJSON_Delete(json);
    cJSON_Delete(answer);
    sodium_memzero(&login, sizeof(login));
    drop_pake_bytes_free(plain, (size_t)size + 65);
    drop_pake_bytes_free(frame, frame_size);
    drop_pake_bytes_free(wire, wire_size);
    drop_pake_bytes_free(opened, opened_size);
    return error;
}
/* 生成绑定 WebSocket 路由的认证头，并派生本次连接专用双向密钥；输出秘密由连接拥有者清理。 */
int drop_pake_client_channel(const WCHAR *url, const char *token, drop_pake_keys *keys, WCHAR header[512]) {
    credential *c;
    unsigned char plain[65], *frame = NULL;
    DWORD size = 0;
    char hex[219];
    int ok = 0;
    AcquireSRWLockExclusive(&凭据缓存锁);
    c = find(url, token, 0);
    if (c && ensure(c) == DROP_OK && c->sequence < UINT64_MAX) {
        plain[0] = 0;
        memcpy(plain + 1, token, 64);
        if (drop_pake_encrypt(&c->keys, DROP_PAKE_HTTP_TX, ++c->sequence, "GET:/api/v1/pake/events", plain, sizeof(plain), &frame, &size)) {
            sodium_bin2hex(hex, sizeof(hex), frame, size);
            if (swprintf_s(header, 512, L"X-Drop-Channel: %hs\r\n", hex) > 0) {
                *keys = c->keys;
                drop_pake_channel_derive(keys, c->sequence);
                ok = 1;
            }
        }
    }
    ReleaseSRWLockExclusive(&凭据缓存锁);
    drop_pake_bytes_free(frame, size);
    sodium_memzero(plain, sizeof(plain));
    return ok;
}

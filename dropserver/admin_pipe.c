/* 本机命名管道管理：受 Windows 权限限制，使用实例密钥认证加密请求，在线操作交给持有数据目录锁的服务器。 */
#include "server.h"
#include <sddl.h>
#include <stdlib.h>
#include <string.h>

#define ADMIN_LIMIT (512 * 1024)

/* 按规范化绝对数据目录区分实例；命名管道只能从本机打开。 */
static int pipe_name(const WCHAR *directory, WCHAR name[96]) {
    WCHAR *absolute = malloc(32768 * sizeof(WCHAR));
    unsigned char hash[32];
    char hex[65];
    DWORD length;
    int ok = 0;
    if (!absolute) return 0;
    length = GetFullPathNameW(directory, 32768, absolute, NULL);
    if (!length || length >= 32768) goto done;
    while (length > 3 && absolute[length - 1] == L'\\')
        absolute[--length] = 0;
    CharLowerBuffW(absolute, length);
    crypto_hash_sha256(hash, (const unsigned char *)absolute, (size_t)length * sizeof(WCHAR));
    sodium_bin2hex(hex, sizeof(hex), hash, sizeof(hash));
    ok = swprintf_s(name, 96, L"\\\\.\\pipe\\DropAdmin-%hs", hex) > 0;
done:
    free(absolute);
    return ok;
}
/* 以独立用途标签从数据库密钥派生管理密钥，不直接复用数据库或附件加密格式。 */
static void admin_key(const drop_keys *keys, unsigned char key[32]) {
    static const unsigned char purpose[] = "DropLocalAdministration-v1";
    crypto_generichash(key, 32, purpose, sizeof(purpose) - 1, keys->database_key, 32);
}
/* 清零管理密码后释放 JSON，调用者不得继续借用其中的字符串。 */
void drop_admin_json_free(cJSON *json) {
    cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (cJSON_IsString(password)) sodium_memzero(password->valuestring, strlen(password->valuestring));
    cJSON_Delete(json);
}
/* 等待可取消的管道操作；超时或退出先取消并等待完成，再释放 OVERLAPPED 和缓冲，避免异步悬挂引用。 */
static int wait_io(HANDLE pipe, OVERLAPPED *operation, HANDLE stop, DWORD timeout, DWORD *transferred) {
    HANDLE events[2] = {operation->hEvent, stop};
    if (WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, timeout) == WAIT_OBJECT_0) return GetOverlappedResult(pipe, operation, transferred, FALSE) != 0;
    CancelIoEx(pipe, operation);
    GetOverlappedResult(pipe, operation, transferred, TRUE);
    return 0;
}
/* 完整收发有上限的字节帧；每一步受退出事件和超时限制，异常不会执行未完整验证的管理请求。 */
static int transfer(HANDLE pipe, void *bytes, DWORD size, int writing, HANDLE stop, DWORD timeout) {
    DWORD position = 0;
    OVERLAPPED operation = {0};
    int ok = 0;
    operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!operation.hEvent) return 0;
    while (position < size) {
        DWORD count = 0;
        BOOL complete;
        ResetEvent(operation.hEvent);
        complete = writing ? WriteFile(pipe, (unsigned char *)bytes + position, size - position, &count, &operation)
                           : ReadFile(pipe, (unsigned char *)bytes + position, size - position, &count, &operation);
        if (!complete && (GetLastError() != ERROR_IO_PENDING || !wait_io(pipe, &operation, stop, timeout, &count))) goto done;
        if (!count || count > size - position) goto done;
        position += count;
    }
    ok = 1;
done:
    CloseHandle(operation.hEvent);
    return ok;
}
/* 序列化后用独立随机 nonce 加密并认证；请求和响应使用不同附加数据，防止方向混用。 */
static int send_json(HANDLE pipe, cJSON *json, const unsigned char key[32], int response, HANDLE stop) {
    char *text = cJSON_PrintUnformatted(json);
    unsigned char *frame = NULL;
    const char *direction = response ? "DropAdminResponse-v1" : "DropAdminRequest-v1";
    unsigned long long encrypted = 0;
    DWORD size;
    int ok = 0;
    if (!text || strlen(text) > ADMIN_LIMIT - 40) goto done;
    size = (DWORD)strlen(text) + 40;
    frame = malloc(size);
    if (!frame) goto done;
    randombytes_buf(frame, 24);
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            frame + 24, &encrypted, (unsigned char *)text, strlen(text), (const unsigned char *)direction, strlen(direction), NULL, frame, key) ||
        encrypted != size - 24)
        goto done;
    ok = transfer(pipe, &size, sizeof(size), 1, stop, 5000) && transfer(pipe, frame, size, 1, stop, 5000);
done:
    if (text) {
        sodium_memzero(text, strlen(text));
        cJSON_free(text);
    }
    free(frame);
    return ok;
}
/* 先检查长度再解密认证，完整严格解析后才交给执行器；明文始终在释放前清零。 */
static cJSON *receive_json(HANDLE pipe, const unsigned char key[32], int response, HANDLE stop) {
    DWORD size = 0;
    unsigned char *frame = NULL, *plain = NULL;
    unsigned long long length = 0;
    const char *direction = response ? "DropAdminResponse-v1" : "DropAdminRequest-v1";
    cJSON *json = NULL;
    if (!transfer(pipe, &size, sizeof(size), 0, stop, response ? 300000 : 5000) || size < 40 || size > ADMIN_LIMIT) goto done;
    frame = malloc(size);
    plain = malloc(size);
    if (!frame || !plain || !transfer(pipe, frame, size, 0, stop, 15000)) goto done;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &length, NULL, frame + 24, size - 24, (const unsigned char *)direction, strlen(direction), frame, key)) goto done;
    json = drop_json_parse((const char *)plain, (size_t)length);
done:
    if (plain) {
        sodium_memzero(plain, size);
        free(plain);
    }
    free(frame);
    return json;
}
/* 串行受限管理请求，但复用服务器各项锁；连接等待可取消，非法或未认证请求不会调用执行器。 */
static DWORD WINAPI worker(void *context) {
    drop_server *server = context;
    unsigned char key[32];
    OVERLAPPED connected = {0};
    admin_key(&server->keys, key);
    connected.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!connected.hEvent) {
        sodium_memzero(key, sizeof(key));
        return 1;
    }
    while (WaitForSingleObject(server->stop_event, 0) != WAIT_OBJECT_0) {
        cJSON *request = NULL, *response = NULL;
        char challenge[DROP_ID_CAP];
        const char *id;
        DWORD count = 0;
        BOOL ready;
        ResetEvent(connected.hEvent);
        ready = ConnectNamedPipe(server->admin_pipe, &connected);
        if (!ready) {
            DWORD error = GetLastError();
            ready = error == ERROR_PIPE_CONNECTED || (error == ERROR_IO_PENDING && wait_io(server->admin_pipe, &connected, server->stop_event, INFINITE, &count));
        }
        if (!ready) {
            if (WaitForSingleObject(server->stop_event, 0) == WAIT_OBJECT_0) break;
            /* 客户端可能在连接完成前退出；复位管道后继续服务，避免一次断开永久停止管理通道。 */
            DisconnectNamedPipe(server->admin_pipe);
            continue;
        }
        drop_id_new(challenge);
        response = cJSON_CreateObject();
        if (!response || !cJSON_AddStringToObject(response, "request_id", challenge) || !send_json(server->admin_pipe, response, key, 1, server->stop_event)) {
            drop_admin_json_free(response);
            DisconnectNamedPipe(server->admin_pipe);
            continue;
        }
        drop_admin_json_free(response);
        response = NULL;
        request = receive_json(server->admin_pipe, key, 0, server->stop_event);
        id = drop_json_string(request, "request_id", 32);
        if (request && id && !strcmp(id, challenge)) {
            drop_error error = drop_admin_execute(server, request, &response);
            if (!response) response = cJSON_CreateObject();
            if (response && cJSON_AddStringToObject(response, "request_id", id) && cJSON_AddStringToObject(response, "error", drop_error_name(error)) &&
                send_json(server->admin_pipe, response, key, 1, server->stop_event)) {
                unsigned char acknowledgement = 0;
                /* 等待客户端读完的确认后断开，避免管道缓存中的成功回复被提前丢弃。 */
                (void)transfer(server->admin_pipe, &acknowledgement, 1, 0, server->stop_event, 5000);
            }
        }
        drop_admin_json_free(request);
        drop_admin_json_free(response);
        DisconnectNamedPipe(server->admin_pipe);
    }
    CloseHandle(connected.hEvent);
    sodium_memzero(key, sizeof(key));
    return 0;
}
/* 管道 ACL 只允许运行用户、管理员和 SYSTEM；拒绝远程连接及同名管道抢占，服务启动失败时释放已创建对象。 */
int drop_admin_start(drop_server *server) {
    WCHAR name[96], descriptor[512], *sid = NULL;
    HANDLE token = NULL;
    TOKEN_USER *user = NULL;
    DWORD needed = 0;
    PSECURITY_DESCRIPTOR security = NULL;
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, FALSE};
    int ok = 0;
    if (!pipe_name(server->directory, name) || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) goto done;
    GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    user = malloc(needed);
    if (!user || !GetTokenInformation(token, TokenUser, user, needed, &needed) || !ConvertSidToStringSidW(user->User.Sid, &sid) ||
        swprintf_s(descriptor, 512, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%s)", sid) < 0 ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor, SDDL_REVISION_1, &security, NULL))
        goto done;
    attributes.lpSecurityDescriptor = security;
    server->admin_pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, ADMIN_LIMIT, ADMIN_LIMIT, 5000, &attributes);
    if (server->admin_pipe == INVALID_HANDLE_VALUE) {
        server->admin_pipe = NULL;
        goto done;
    }
    server->admin_thread = CreateThread(NULL, 0, worker, server, 0, NULL);
    ok = server->admin_thread != NULL;
done:
    if (!ok && server->admin_pipe) {
        CloseHandle(server->admin_pipe);
        server->admin_pipe = NULL;
    }
    if (token) CloseHandle(token);
    if (sid) LocalFree(sid);
    if (security) LocalFree(security);
    free(user);
    return ok;
}
/* 先置位共享停止事件并取消等待，再等待管理线程；数据库和密钥在调用返回后才允许销毁。 */
void drop_admin_stop(drop_server *server) {
    if (server->admin_thread) {
        SetEvent(server->stop_event);
        CancelIoEx(server->admin_pipe, NULL);
        WaitForSingleObject(server->admin_thread, INFINITE);
        CloseHandle(server->admin_thread);
        server->admin_thread = NULL;
    }
    if (server->admin_pipe) {
        CloseHandle(server->admin_pipe);
        server->admin_pipe = NULL;
    }
}
/* 客户端读取原实例的 DPAPI 密钥而不打开数据库；帧认证防止伪管道截取密码或伪造成功，回复须对应本次随机请求 ID。 */
int drop_admin_local(const WCHAR *directory, cJSON *request, cJSON **response) {
    WCHAR name[96], *path = malloc(32768 * sizeof(WCHAR));
    HANDLE pipe = INVALID_HANDLE_VALUE;
    unsigned char *secret = NULL, key[32] = {0};
    DWORD size = 0;
    char id[DROP_ID_CAP];
    int result = 0;
    *response = NULL;
    if (!path || !pipe_name(directory, name)) goto done;
    pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
        if (WaitNamedPipeW(name, 30000))
            pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) result = -1;
        goto done;
    }
    if (!drop_path_join(directory, L"runtime-machine.dpapi", path, 32768)) goto done;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && !drop_path_join(directory, L"runtime-user.dpapi", path, 32768)) goto done;
    if (!drop_secret_load(path, &secret, &size) || size != sizeof(drop_keys) || memcmp(secret, "DROPKEY1", 8)) goto done;
    admin_key((const drop_keys *)secret, key);
    *response = receive_json(pipe, key, 1, NULL);
    if (!*response || !drop_id_valid(drop_json_string(*response, "request_id", 32))) goto done;
    strcpy_s(id, sizeof(id), drop_json_string(*response, "request_id", 32));
    drop_admin_json_free(*response);
    *response = NULL;
    if (!cJSON_AddStringToObject(request, "request_id", id) || !send_json(pipe, request, key, 0, NULL)) goto done;
    *response = receive_json(pipe, key, 1, NULL);
    if (*response) {
        const char *returned = drop_json_string(*response, "request_id", 32);
        result = returned && !strcmp(returned, id);
        if (result) {
            unsigned char acknowledgement = 1;
            result = transfer(pipe, &acknowledgement, 1, 1, NULL, 5000);
        }
    }
done:
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    if (secret) {
        sodium_memzero(secret, size);
        free(secret);
    }
    sodium_memzero(key, sizeof(key));
    free(path);
    return result;
}

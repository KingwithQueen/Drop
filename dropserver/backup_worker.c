/* 服务器定时及手动备份线程，使用按数据目录命名且有受限 ACL 的跨进程事件。 */
#include "server.h"
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>

/* 对规范化且忽略大小写的目录计算哈希，生成同一服务器实例的跨进程备份事件名。 */
static int event_name(const WCHAR *directory, WCHAR name[96]) {
    WCHAR *full = malloc(32768 * sizeof(WCHAR)), wide[65];
    char *utf8 = malloc(131072), hex[65];
    unsigned char hash[32];
    DWORD n;
    int ok = 0;
    if (!full || !utf8) goto done;
    n = GetFullPathNameW(directory, 32768, full, NULL);
    if (!n || n >= 32768 || !CharLowerBuffW(full, n) || !drop_to_utf8(full, utf8, 131072)) goto done;
    crypto_hash_sha256(hash, (const unsigned char *)utf8, strlen(utf8));
    drop_hash_hex(hash, hex);
    if (drop_to_wide(hex, wide, 65) && swprintf_s(name, 96, L"Global\\DropBackup-%s", wide) >= 0) ok = 1;
done:
    free(full);
    free(utf8);
    return ok;
}
/* 等待定时或手动备份请求，失败保留任务并延后重试；退出事件打断等待。 */
static DWORD WINAPI worker(void *opaque) {
    drop_server *server = opaque;
    HANDLE events[2] = {server->stop_event, server->backup_request};
    DWORD wait = server->backup_seconds ? server->backup_seconds * 1000 : INFINITE;
    for (;;) {
        DWORD result = WaitForMultipleObjects(2, events, FALSE, wait);
        char task[DROP_ID_CAP] = {0};
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;
        if (drop_backup_create(server, server->backup_directory, task)) {
            fprintf(stderr, "backup_complete task=%s\n", task);
            wait = server->backup_seconds ? server->backup_seconds * 1000 : INFINITE;
        } else {
            fprintf(stderr, "backup_retry_pending\n");
            wait = 60000;
        }
    }
    return 0;
}
/* 按受限 ACL 创建本实例备份事件和线程；未配置备份目录时直接成功，不启动后台任务。 */
int drop_backup_start(drop_server *server) {
    WCHAR name[96], descriptor[512], *sid = NULL;
    HANDLE token = NULL;
    TOKEN_USER *user = NULL;
    DWORD needed = 0;
    PSECURITY_DESCRIPTOR security = NULL;
    SECURITY_ATTRIBUTES attributes;
    int ok = 0;
    if (!*server->backup_directory) return 1;
    if (!event_name(server->directory, name) || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) goto done;
    GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    user = malloc(needed);
    if (!user || !GetTokenInformation(token, TokenUser, user, needed, &needed) || !ConvertSidToStringSidW(user->User.Sid, &sid) ||
        swprintf_s(descriptor, 512, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%s)", sid) < 0 ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor, SDDL_REVISION_1, &security, NULL))
        goto done;
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = security;
    attributes.bInheritHandle = FALSE;
    server->backup_request = CreateEventW(&attributes, FALSE, FALSE, name);
    if (!server->backup_request || GetLastError() == ERROR_ALREADY_EXISTS) goto done;
    server->backup_thread = CreateThread(NULL, 0, worker, server, 0, NULL);
    ok = server->backup_thread != NULL;
done:
    if (!ok && server->backup_request) {
        CloseHandle(server->backup_request);
        server->backup_request = NULL;
    }
    if (token) CloseHandle(token);
    if (sid) LocalFree(sid);
    if (security) LocalFree(security);
    free(user);
    return ok;
}
/* 通知并等待备份线程停止后释放句柄；服务器必须存活到线程退出。 */
void drop_backup_stop(drop_server *server) {
    if (server->backup_thread) {
        SetEvent(server->stop_event);
        WaitForSingleObject(server->backup_thread, INFINITE);
        CloseHandle(server->backup_thread);
        server->backup_thread = NULL;
    }
    if (server->backup_request) {
        CloseHandle(server->backup_request);
        server->backup_request = NULL;
    }
}
/* 打开既有服务器目录对应的备份事件并置位，只请求运行实例执行备份。 */
int drop_backup_trigger(const WCHAR *directory) {
    WCHAR name[96];
    HANDLE event;
    int ok;
    if (!event_name(directory, name)) return 0;
    event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!event) return 0;
    ok = SetEvent(event) != 0;
    CloseHandle(event);
    return ok;
}

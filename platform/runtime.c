/* 原生界面的单拥有者调度器：队列唤醒和网络事件分离，正文、附件、删除均沿用现有持久核心。 */
#include "runtime.h"
#include <sodium.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>

struct drop_runtime {
    pthread_t worker, events;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    HANDLE stop, work, lock;
    WCHAR root[32768];
    drop_task *head, *tail;
    unsigned queued;
    drop_snapshot *snapshot;
    atomic_int changed, stopping;
    int events_running;
    uint64_t send_serial, send_finished;
    int notify, attention;
    char saved_url[8193], saved_login[65], saved_ca[131072];
    WCHAR event_url[2049], event_ca[32768];
    char event_token[DROP_TOKEN_CAP];
    HANDLE event_stop;
};

void drop_task_free(drop_task *task) {
    if (!task) return;
    if (task->password) {
        sodium_memzero(task->password, strlen(task->password));
        free(task->password);
    }
    free(task->text);
    free(task->url);
    free(task->login);
    free(task->ca);
    free(task->destination);
    for (int i = 0; i < task->count && i < DROP_MAX_ATTACHMENTS; ++i)
        free(task->paths[i]);
    free(task);
}
void drop_snapshot_free(drop_snapshot *snapshot) {
    if (snapshot) {
        drop_ui_history_free(snapshot->history);
        free(snapshot);
    }
}
int drop_runtime_submit(drop_runtime *runtime, drop_task *task) {
    if (!runtime || !task || task->next || task->count < 0 || task->count > DROP_MAX_ATTACHMENTS) return 0;
    pthread_mutex_lock(&runtime->mutex);
    if (runtime->queued == 32 || atomic_load(&runtime->stopping)) {
        pthread_mutex_unlock(&runtime->mutex);
        return 0;
    }
    if (runtime->tail)
        runtime->tail->next = task;
    else
        runtime->head = task;
    runtime->tail = task;
    ++runtime->queued;
    drop_posix_event_set(runtime->work);
    pthread_cond_signal(&runtime->wake);
    pthread_mutex_unlock(&runtime->mutex);
    return 1;
}
drop_snapshot *drop_runtime_take(drop_runtime *runtime) {
    drop_snapshot *snapshot;
    pthread_mutex_lock(&runtime->mutex);
    snapshot = runtime->snapshot;
    runtime->snapshot = NULL;
    pthread_mutex_unlock(&runtime->mutex);
    return snapshot;
}
/* 快照只复制公开设置和展示数据；旧快照若尚未取走，由后台安全释放。 */
static void publish(drop_runtime *runtime, drop_client *client, int ready, int busy, drop_error error, const char *status, const char *peer, const char *search, int page,
    int64_t from, int64_t to) {
    drop_snapshot *snapshot = calloc(1, sizeof(*snapshot)), *previous;
    if (!snapshot) return;
    snapshot->ready = ready;
    snapshot->busy = busy;
    snapshot->error = error;
    snapshot->send_serial = runtime->send_serial;
    snapshot->send_finished = runtime->send_finished;
    snapshot->notify = runtime->notify;
    snapshot->attention = runtime->attention;
    strcpy_s(snapshot->url, sizeof(snapshot->url), runtime->saved_url);
    strcpy_s(snapshot->login, sizeof(snapshot->login), runtime->saved_login);
    strcpy_s(snapshot->ca, sizeof(snapshot->ca), runtime->saved_ca);
    strcpy_s(snapshot->status, sizeof(snapshot->status), status);
    if (ready) {
        snapshot->history = drop_ui_history(client, page, search, from, to, peer);
        drop_to_utf8(client->url, snapshot->url, sizeof(snapshot->url));
        drop_to_utf8(client->ca_file, snapshot->ca, sizeof(snapshot->ca));
        if (snapshot->history && snapshot->history->peer_count) drop_to_utf8(snapshot->history->peers[0].login, snapshot->login, sizeof(snapshot->login));
    }
    pthread_mutex_lock(&runtime->mutex);
    previous = runtime->snapshot;
    runtime->snapshot = snapshot;
    pthread_mutex_unlock(&runtime->mutex);
    drop_snapshot_free(previous);
}
/* 独立事件线程串行收发心跳与通知；只发布变化标志，不读取或写入数据库。 */
typedef struct heartbeat_context {
    drop_ws ws;
    HANDLE stop;
} heartbeat_context;
/* macOS 的 pthread 默认栈较小，现有宽路径调用链需要明确的栈容量；仅预留虚拟地址，页面按需提交。 */
static int start_thread(pthread_t *thread, void *(*entry)(void *), void *context) {
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error) return error;
    error = pthread_attr_setstacksize(&attributes, 8u * 1024u * 1024u);
    if (!error) error = pthread_create(thread, &attributes, entry, context);
    pthread_attr_destroy(&attributes);
    return error;
}
/* 心跳副本持有独立引用；拥有者先关闭 socket 再等待此线程，避免销毁仍被使用的连接状态。 */
static void *heartbeat_worker(void *context) {
    heartbeat_context *heartbeat = context;
    while (WaitForSingleObject(heartbeat->stop, 30000) == WAIT_TIMEOUT) {
        if (drop_ws_send(&heartbeat->ws, "{\"type\":\"heartbeat\"}", 20, heartbeat->stop) != NO_ERROR) break;
    }
    drop_ws_close(&heartbeat->ws);
    return NULL;
}
static void *event_worker(void *context) {
    drop_runtime *runtime = context;
    unsigned retry = 1;
    while (WaitForSingleObject(runtime->event_stop, 0) != WAIT_OBJECT_0 && !atomic_load(&runtime->stopping)) {
        drop_ws ws;
        drop_error error = drop_ws_open(runtime->event_url, runtime->event_token, &ws, runtime->event_ca);
        if (error == DROP_OK) {
            heartbeat_context heartbeat = {0};
            pthread_t timer;
            int timer_running = 0;
            heartbeat.stop = drop_posix_event_create();
            heartbeat.ws = ws;
            if (heartbeat.stop) {
                drop_ws_retain(&heartbeat.ws);
                timer_running = start_thread(&timer, heartbeat_worker, &heartbeat) == 0;
                if (!timer_running) drop_ws_release(&heartbeat.ws);
            }
            retry = 1;
            while (timer_running && WaitForSingleObject(runtime->event_stop, 0) != WAIT_OBJECT_0 && !atomic_load(&runtime->stopping)) {
                char bytes[4096];
                DWORD length;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
                cJSON *event;
                if (drop_ws_receive(&ws, bytes, sizeof(bytes), &length, &type, runtime->event_stop) != NO_ERROR || type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                    !(event = drop_json_parse(bytes, length)))
                    break;
                const char *kind = drop_json_string(event, "type", 32);
                if (kind && !strcmp(kind, "changed")) {
                    atomic_store(&runtime->changed, 1);
                    pthread_mutex_lock(&runtime->mutex);
                    pthread_cond_signal(&runtime->wake);
                    pthread_mutex_unlock(&runtime->mutex);
                }
                cJSON_Delete(event);
            }
            drop_ws_close(&ws);
            if (heartbeat.stop) drop_posix_event_set(heartbeat.stop);
            if (timer_running) pthread_join(timer, NULL);
            if (heartbeat.stop) CloseHandle(heartbeat.stop);
        }
        if (error == DROP_E_AUTH || error == DROP_E_CERTIFICATE || error == DROP_E_UNSUPPORTED) break;
        if (WaitForSingleObject(runtime->event_stop, retry * 1000) == WAIT_OBJECT_0) break;
        if (retry < 60) retry = retry > 30 ? 60 : retry * 2;
    }
    return NULL;
}
static void events_stop(drop_runtime *runtime) {
    if (runtime->events_running) {
        drop_posix_event_set(runtime->event_stop);
        pthread_join(runtime->events, NULL);
        runtime->events_running = 0;
    }
    sodium_memzero(runtime->event_token, sizeof(runtime->event_token));
}
/* 只在原同步核心拥有凭据时启动事件线程；重新登录先结束旧线程，防止跨账号通知错配。 */
static void events_start(drop_runtime *runtime, drop_client *client) {
    events_stop(runtime);
    wcscpy_s(runtime->event_url, 2049, client->url);
    wcscpy_s(runtime->event_ca, 32768, client->ca_file);
    strcpy_s(runtime->event_token, DROP_TOKEN_CAP, client->token);
    drop_posix_event_reset(runtime->event_stop);
    runtime->events_running = start_thread(&runtime->events, event_worker, runtime) == 0;
}
/* 公开设置只有服务器、CA 和登录名，绝不把密码写入 JSON。 */
static int settings_save(drop_runtime *runtime, const drop_task *task) {
    WCHAR path[32768];
    cJSON *settings = cJSON_CreateObject();
    char *text = NULL;
    int ok = 0;
    if (task) {
        strcpy_s(runtime->saved_url, sizeof(runtime->saved_url), task->url);
        strcpy_s(runtime->saved_login, sizeof(runtime->saved_login), task->login);
        strcpy_s(runtime->saved_ca, sizeof(runtime->saved_ca), task->ca ? task->ca : "");
    }
    if (!settings || !cJSON_AddStringToObject(settings, "url", runtime->saved_url) || !cJSON_AddStringToObject(settings, "login", runtime->saved_login) ||
        !cJSON_AddStringToObject(settings, "ca_file", runtime->saved_ca) || !cJSON_AddBoolToObject(settings, "notifications", runtime->notify) ||
        !cJSON_AddBoolToObject(settings, "attention", runtime->attention) || !(text = cJSON_PrintUnformatted(settings)))
        goto done;
    ok = drop_path_join(runtime->root, L"ui.json", path, 32768) && drop_file_write(path, text, (DWORD)strlen(text), 1);
done:
    cJSON_free(text);
    cJSON_Delete(settings);
    return ok;
}
/* 公开偏好在后台加载，损坏文件不能引入秘密或改变已绑定服务器身份；登录凭据仍由独立存储处理。 */
static void settings_load(drop_runtime *runtime) {
    WCHAR path[32768];
    unsigned char *bytes = NULL;
    DWORD size;
    cJSON *json = NULL, *value;
    const char *text;
    runtime->notify = runtime->attention = 1;
    if (!drop_path_join(runtime->root, L"ui.json", path, 32768) || !drop_file_read(path, &bytes, &size, 65536) || !(json = drop_json_parse((char *)bytes, size))) goto done;
    if ((text = drop_json_string(json, "url", 8192))) strcpy_s(runtime->saved_url, sizeof(runtime->saved_url), text);
    if ((text = drop_json_string(json, "login", 64))) strcpy_s(runtime->saved_login, sizeof(runtime->saved_login), text);
    if ((text = drop_json_string(json, "ca_file", 32768))) strcpy_s(runtime->saved_ca, sizeof(runtime->saved_ca), text);
    value = cJSON_GetObjectItemCaseSensitive(json, "notifications");
    if (cJSON_IsBool(value)) runtime->notify = cJSON_IsTrue(value);
    value = cJSON_GetObjectItemCaseSensitive(json, "attention");
    if (cJSON_IsBool(value)) runtime->attention = cJSON_IsTrue(value);
done:
    cJSON_Delete(json);
    free(bytes);
}
/* 所有数据库访问和凭据变更都在此线程进行，任务失败不清空原有持久发送/删除队列。 */
static void *worker(void *context) {
    drop_runtime *runtime = context;
    settings_load(runtime);
    drop_client *client = calloc(1, sizeof(*client));
    char peer[DROP_ID_CAP] = "", *search = NULL;
    int ready = client && drop_client_open(client, runtime->root), page = 0;
    int64_t from = 0, to = INT64_MAX;
    uint64_t next_sync = 0;
    unsigned retry = 1;
    drop_error error = DROP_OK;
    if (ready) {
        strcpy_s(peer, DROP_ID_CAP, client->user);
        client->work_pending = runtime->work;
        events_start(runtime, client);
    }
    publish(runtime, client, ready, 0, DROP_OK, ready ? "正在连接…" : "请在配置与账号中登录。", peer, search, page, from, to);
    while (client && !atomic_load(&runtime->stopping)) {
        drop_task *task;
        char status[256] = "已同步";
        int do_sync = 0;
        pthread_mutex_lock(&runtime->mutex);
        task = runtime->head;
        if (task) {
            runtime->head = task->next;
            if (!runtime->head) runtime->tail = NULL;
            --runtime->queued;
        }
        if (!runtime->head) drop_posix_event_reset(runtime->work);
        pthread_mutex_unlock(&runtime->mutex);
        if (task) {
            error = ready ? DROP_OK : DROP_E_AUTH;
            if (task->kind == DROP_TASK_SEND) ++runtime->send_finished;
            if (task->kind == DROP_TASK_PREFERENCES) {
                int old_notify = runtime->notify, old_attention = runtime->attention;
                runtime->notify = task->notify != 0;
                runtime->attention = task->attention != 0;
                error = settings_save(runtime, NULL) ? DROP_OK : DROP_E_IO;
                if (error != DROP_OK) {
                    runtime->notify = old_notify;
                    runtime->attention = old_attention;
                }
                strcpy_s(status, sizeof(status), error == DROP_OK ? "提醒设置已保存。" : "提醒设置保存失败。 ");
            } else if (task->kind == DROP_TASK_LOGIN) {
                WCHAR *url = calloc(2049, sizeof(WCHAR)), *ca = calloc(32768, sizeof(WCHAR));
                publish(runtime, client, ready, 1, DROP_OK, "正在登录…", peer, search, page, from, to);
                events_stop(runtime);
                if (ready) drop_client_close(client);
                ready = 0;
                error = DROP_E_ARGUMENT;
                if (url && ca && task->url && task->login && task->password && drop_to_wide(task->url, url, 2049) &&
                    (!task->ca || !*task->ca || drop_to_wide(task->ca, ca, 32768))) {
                    ready = drop_client_login(runtime->root, url, task->login, task->password, ca) && settings_save(runtime, task) && drop_client_open(client, runtime->root);
                    error = ready ? DROP_OK : DROP_E_AUTH;
                }
                free(url);
                free(ca);
                if (ready) {
                    strcpy_s(peer, DROP_ID_CAP, client->user);
                    page = 0;
                    client->work_pending = runtime->work;
                    events_start(runtime, client);
                    do_sync = 1;
                }
                strcpy_s(status, sizeof(status), ready ? "登录成功" : "登录失败，请检查账号、服务器或已解锁的系统密钥环。 ");
            } else if (task->kind == DROP_TASK_LOGOUT) {
                WCHAR path[32768];
                events_stop(runtime);
                if (ready) {
                    cJSON *answer = NULL;
                    DWORD http_status;
                    (void)drop_http_json_ca(client->url, L"POST", L"/api/v1/logout", client->token, NULL, &answer, &http_status, client->ca_file);
                    cJSON_Delete(answer);
                    drop_client_close(client);
                }
                ready = 0;
                error = drop_path_join(runtime->root, DROP_SESSION_FILE, path, 32768) && (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES || DeleteFileW(path)) ? DROP_OK
                                                                                                                                                                    : DROP_E_IO;
                strcpy_s(status, sizeof(status), error == DROP_OK ? "已注销，历史和附件保留。" : "注销凭据删除失败。 ");
            } else if (ready && task->kind == DROP_TASK_VIEW) {
                if (drop_id_valid(task->peer)) strcpy_s(peer, DROP_ID_CAP, task->peer);
                page = task->page >= 0 && task->page <= 1000000 ? task->page : 0;
                from = task->from >= 0 ? task->from : 0;
                to = task->to > 0 ? task->to : INT64_MAX;
                free(search);
                search = task->text;
                task->text = NULL;
            } else if (ready && task->kind == DROP_TASK_SEND) {
                WCHAR *paths[DROP_MAX_ATTACHMENTS] = {0};
                char request[DROP_ID_CAP];
                int valid = task->text && drop_id_valid(task->peer) && !strcmp(task->account, client->user);
                for (int i = 0; i < task->count; ++i) {
                    paths[i] = calloc(32768, sizeof(WCHAR));
                    if (!paths[i] || !task->paths[i] || !drop_to_wide(task->paths[i], paths[i], 32768)) valid = 0;
                }
                error = valid ? (task->count ? drop_client_queue_files(client, task->text, task->peer, (const WCHAR *const *)paths, task->count, request)
                                             : drop_client_queue(client, task->text, task->peer, request))
                              : DROP_E_ARGUMENT;
                if (error == DROP_OK) ++runtime->send_serial;
                for (int i = 0; i < task->count; ++i)
                    free(paths[i]);
                strcpy_s(status, sizeof(status), error == DROP_OK ? "已保存到待发送队列。" : "发送副本保存失败，未加入队列。 ");
                do_sync = error == DROP_OK;
            } else if (ready && task->kind == DROP_TASK_FIND) {
                error = drop_ui_recipient(client, task->login, peer);
                page = 0;
                strcpy_s(status, sizeof(status), error == DROP_OK ? "已找到账号。" : "账号不可用或网络连接失败。 ");
            } else if (ready && task->kind == DROP_TASK_READ) {
                error = drop_client_read_queue(client, task->message) ? DROP_OK : DROP_E_DATABASE;
                do_sync = error == DROP_OK;
            } else if (ready && task->kind == DROP_TASK_DELETE) {
                error = drop_client_delete_queue(client, task->message);
                strcpy_s(status, sizeof(status), error == DROP_OK ? "删除已排队，将同步到全部设备。" : "不能删除该消息。 ");
                do_sync = error == DROP_OK;
            } else if (ready && task->kind == DROP_TASK_EXPORT) {
                drop_detail *detail = drop_ui_detail(client, task->message, 0);
                WCHAR *destination = calloc(32768, sizeof(WCHAR));
                error = DROP_E_ARGUMENT;
                if (detail && destination && task->destination && drop_to_wide(task->destination, destination, 32768)) {
                    for (int i = 0; i < detail->count; ++i)
                        if (!strcmp(detail->files[i].id, task->object)) error = drop_ui_export(client, &detail->files[i], destination);
                }
                drop_ui_detail_free(detail);
                free(destination);
                strcpy_s(status, sizeof(status), error == DROP_OK ? "附件已导出到所选位置。" : "导出失败，请检查附件完整性或目标位置。 ");
            } else if (ready && task->kind == DROP_TASK_REVIEW) {
                error = drop_client_retry_review(client);
                do_sync = error == DROP_OK;
            } else if (ready && task->kind == DROP_TASK_SYNC)
                do_sync = 1;
            publish(runtime, client, ready, 0, error, status, peer, search, page, from, to);
            drop_task_free(task);
        }
        if (ready && (do_sync || atomic_exchange(&runtime->changed, 0) || GetTickCount64() >= next_sync)) {
            client->sync_page_limit = 100;
            client->download_quantum = UINT64_C(4194304);
            client->downloaded_bytes = 0;
            error = drop_client_reconcile(client);
            if (error == DROP_OK) error = drop_client_deletions(client);
            if (error == DROP_OK) error = drop_client_read_receipts(client);
            if (error == DROP_OK) error = drop_client_sync(client);
            if (error == DROP_OK) error = drop_client_flush(client);
            if (error == DROP_OK) error = drop_client_deletions(client);
            if (error == DROP_OK) {
                retry = 1;
                next_sync = GetTickCount64() + ((client->more_bodies || client->more_downloads || client->more_deletions) ? 100 : 30000);
            } else {
                next_sync = GetTickCount64() + (uint64_t)retry * 1000;
                if (retry < 60) retry = retry > 30 ? 60 : retry * 2;
            }
            if (error == DROP_E_AUTH || error == DROP_E_PROTOCOL || error == DROP_E_CERTIFICATE) next_sync = UINT64_MAX;
            if (error == DROP_E_AUTH)
                strcpy_s(status, sizeof(status), "登录已失效，请重新登录。 ");
            else if (error == DROP_E_CONFLICT)
                strcpy_s(status, sizeof(status), "服务器已恢复，请复核待发送任务。 ");
            else if (error == DROP_E_PENDING)
                strcpy_s(status, sizeof(status), "已保存，继续处理附件和同步任务。 ");
            else
                sprintf_s(status, sizeof(status), "%s · %s", error == DROP_OK ? "已同步" : "连接待重试", drop_error_name(error));
            publish(runtime, client, ready, 0, error, status, peer, search, page, from, to);
        }
        pthread_mutex_lock(&runtime->mutex);
        if (!runtime->head && !atomic_load(&runtime->stopping) && !atomic_load(&runtime->changed)) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            ++until.tv_sec;
            pthread_cond_timedwait(&runtime->wake, &runtime->mutex, &until);
        }
        pthread_mutex_unlock(&runtime->mutex);
    }
    events_stop(runtime);
    if (ready) drop_client_close(client);
    free(client);
    free(search);
    return NULL;
}
drop_runtime *drop_runtime_start(const WCHAR *root) {
    drop_runtime *runtime = calloc(1, sizeof(*runtime));
    WCHAR path[32768];
    if (!runtime) return NULL;
    runtime->lock = INVALID_HANDLE_VALUE;
    if ((root ? !GetFullPathNameW(root, 32768, runtime->root, NULL) : !drop_program_data_directory(L"Drop", runtime->root, 32768, 1)) || !drop_private_directory(runtime->root) ||
        !drop_path_join(runtime->root, L"ui.lock", path, 32768) ||
        (runtime->lock = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL)) == INVALID_HANDLE_VALUE)
        goto failed;
    runtime->stop = drop_posix_event_create();
    runtime->work = drop_posix_event_create();
    runtime->event_stop = drop_posix_event_create();
    if (!runtime->stop || !runtime->work || !runtime->event_stop) goto failed;
    if (pthread_mutex_init(&runtime->mutex, NULL)) goto failed;
    if (pthread_cond_init(&runtime->wake, NULL)) {
        pthread_mutex_destroy(&runtime->mutex);
        goto failed;
    }
    atomic_init(&runtime->changed, 0);
    atomic_init(&runtime->stopping, 0);
    drop_network_cancel_event(runtime->stop);
    if (start_thread(&runtime->worker, worker, runtime)) {
        pthread_cond_destroy(&runtime->wake);
        pthread_mutex_destroy(&runtime->mutex);
        drop_network_cancel_event(NULL);
        goto failed;
    }
    return runtime;
failed:
    if (runtime->lock != INVALID_HANDLE_VALUE) CloseHandle(runtime->lock);
    if (runtime->stop) CloseHandle(runtime->stop);
    if (runtime->work) CloseHandle(runtime->work);
    if (runtime->event_stop) CloseHandle(runtime->event_stop);
    free(runtime);
    return NULL;
}
void drop_runtime_stop(drop_runtime *runtime) {
    if (!runtime) return;
    atomic_store(&runtime->stopping, 1);
    drop_posix_event_set(runtime->stop);
    drop_posix_event_set(runtime->event_stop);
    pthread_mutex_lock(&runtime->mutex);
    pthread_cond_signal(&runtime->wake);
    pthread_mutex_unlock(&runtime->mutex);
    pthread_join(runtime->worker, NULL);
    drop_network_cancel_event(NULL);
    while (runtime->head) {
        drop_task *task = runtime->head;
        runtime->head = task->next;
        drop_task_free(task);
    }
    drop_snapshot_free(runtime->snapshot);
    pthread_cond_destroy(&runtime->wake);
    pthread_mutex_destroy(&runtime->mutex);
    CloseHandle(runtime->stop);
    CloseHandle(runtime->work);
    CloseHandle(runtime->event_stop);
    CloseHandle(runtime->lock);
    sodium_memzero(runtime, sizeof(*runtime));
    free(runtime);
}

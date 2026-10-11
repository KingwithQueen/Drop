/* 后台同步和界面任务分派；网络线程通过事件唤醒核心，结果通过窗口消息交还界面。 */
#include "ui_internal.h"
#include "object.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 释放任务的正文、密码和路径；密码先清零，任务移交后由工作线程调用。 */
void drop_ui_job_free(ui_job *job) {
    int i;
    if (!job) return;
    if (job->password) {
        sodium_memzero(job->password, strlen(job->password));
        free(job->password);
    }
    free(job->body);
    for (i = 0; i < job->count; ++i)
        free(job->paths[i]);
    free(job);
}
/* 释放尚未被界面接管的历史和详情，再释放结果结构。 */
void drop_ui_result_free(ui_result *result) {
    if (result) {
        drop_ui_history_free(result->history);
        drop_ui_detail_free(result->detail);
        free(result);
    }
}
/* 在锁内移出指定或当前连接，锁外关闭；避免阻塞 socket 的关闭过程占住界面取消所需的锁。 */
static void ws_discard_for(ui_ws_context *context, struct drop_ws_io *expected) {
    drop_ws old;
    EnterCriticalSection(&context->lock);
    if (expected && context->ws.io != expected) {
        LeaveCriticalSection(&context->lock);
        return;
    }
    old = context->ws;
    memset(&context->ws, 0, sizeof(context->ws));
    LeaveCriticalSection(&context->lock);
    /* 在锁外中止连接，避免阻塞的网络操作占住界面取消所需的锁。 */
    drop_ws_close(&old);
}
/* 关闭事件通道当前连接，实际中止和引用释放由 ws_discard_for 完成。 */
static void ws_discard(ui_ws_context *context) {
    ws_discard_for(context, NULL);
}
/* 定时发送应用心跳并检查回应期限，连接副本持有引用；失败关闭当前连接并唤醒重连。 */
static DWORD WINAPI heartbeat_worker(void *data) {
    ui_ws_context *context = data;
    HANDLE events[2] = {context->app_stop, context->stop};
    while (WaitForMultipleObjects(2, events, FALSE, 1000) == WAIT_TIMEOUT) {
        drop_ws current;
        ULONGLONG now = GetTickCount64();
        int expired = 0, send = 0;
        EnterCriticalSection(&context->lock);
        current = context->ws;
        drop_ws_retain(&current);
        if (current.socket) {
            expired = now - context->last_received > (ULONGLONG)context->timeout_seconds * 1000;
            send = !expired && now >= context->next_heartbeat;
            if (send) context->next_heartbeat = now + (ULONGLONG)context->heartbeat_seconds * 1000;
        }
        LeaveCriticalSection(&context->lock);
        if (expired) {
            ws_discard_for(context, current.io);
            SetEvent(context->wake);
        } else if (send && drop_ws_send(&current, "{\"type\":\"heartbeat\"}", 20, context->app_stop) != NO_ERROR) {
            ws_discard_for(context, current.io);
            SetEvent(context->wake);
        }
        drop_ws_release(&current);
    }
    return 0;
}
/* 维护事件连接和退避重连，验证完整通知后唤醒增量同步；停止时结束心跳并清理连接。 */
static DWORD WINAPI ws_worker(void *data) {
    ui_ws_context *context = data;
    unsigned char buffer[1024];
    DWORD length, error;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    HANDLE events[3] = {context->app_stop, context->stop, context->reconnect};
    unsigned attempt = 0;
    static const DWORD delays[] = {1000, 2000, 4000, 8000, 16000, 30000, 60000};
    context->timer = CreateThread(NULL, 0, heartbeat_worker, context, 0, NULL);
    if (!context->timer) return 1;
    while (WaitForMultipleObjects(2, events, FALSE, 0) == WAIT_TIMEOUT) {
        drop_ws opened;
        ULONGLONG started = GetTickCount64();
        DWORD delay, jitter;
        ResetEvent(context->reconnect);
        drop_error result = drop_ws_open(context->url, context->token, &opened, context->ca);
        if (result == DROP_E_AUTH || result == DROP_E_CERTIFICATE || result == DROP_E_PROTOCOL) {
            SetEvent(context->wake);
            break;
        }
        if (result == DROP_OK) {
            EnterCriticalSection(&context->lock);
            context->ws = opened;
            context->last_received = GetTickCount64();
            context->next_heartbeat = context->last_received + (ULONGLONG)context->heartbeat_seconds * 1000;
            LeaveCriticalSection(&context->lock);
            SetEvent(context->wake);
            while (WaitForMultipleObjects(2, events, FALSE, 0) == WAIT_TIMEOUT) {
                drop_ws current;
                EnterCriticalSection(&context->lock);
                current = context->ws;
                drop_ws_retain(&current);
                LeaveCriticalSection(&context->lock);
                if (!current.socket) {
                    drop_ws_release(&current);
                    break;
                }
                error = drop_ws_receive(&current, buffer, sizeof(buffer), &length, &type, context->app_stop);
                drop_ws_release(&current);
                if (error || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
                if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) break;
                if (length == 18 && !memcmp(buffer, "{\"type\":\"changed\"}", 18))
                    SetEvent(context->wake);
                else if (length == 20 && !memcmp(buffer, "{\"type\":\"heartbeat\"}", 20)) {
                    attempt = 0;
                    EnterCriticalSection(&context->lock);
                    context->last_received = GetTickCount64();
                    LeaveCriticalSection(&context->lock);
                } else
                    break;
            }
            ws_discard(context);
            if (GetTickCount64() - started > (ULONGLONG)context->timeout_seconds * 1000) attempt = 0;
        }
        delay = delays[attempt < 6 ? attempt : 6];
        if (attempt < 6) ++attempt;
        jitter = randombytes_uniform(delay / 10 < 1000 ? delay / 10 + 1 : 1001);
        error = WaitForMultipleObjects(3, events, FALSE, delay + jitter);
        if (error == WAIT_OBJECT_0 || error == WAIT_OBJECT_0 + 1) break;
        if (error == WAIT_OBJECT_0 + 2) attempt = 0;
    }
    SetEvent(context->stop);
    WaitForSingleObject(context->timer, INFINITE);
    CloseHandle(context->timer);
    context->timer = NULL;
    return 0;
}
/* 向事件连接发出可并发处理的取消信号，供退出、设置或网络恢复路径调用。 */
void drop_ui_cancel_ws(drop_ui *ui) {
    EnterCriticalSection(&ui->lock);
    if (ui->ws) {
        SetEvent(ui->ws->stop);
        ws_discard(ui->ws);
    }
    LeaveCriticalSection(&ui->lock);
}
/* 网络或休眠恢复后打断退避并唤醒同步，使持久增量及时补齐。 */
void drop_ui_resume(drop_ui *ui) {
    SetEvent(ui->wake);
    EnterCriticalSection(&ui->lock);
    if (ui->ws) {
        SetEvent(ui->ws->reconnect);
        ws_discard(ui->ws);
    }
    LeaveCriticalSection(&ui->lock);
}
/* 通知连接上下文停止并等待其线程结束，再清零凭据、关闭事件和释放上下文。 */
static void ws_stop(drop_ui *ui, ui_ws_context **owner) {
    ui_ws_context *context = *owner;
    if (!context) return;
    EnterCriticalSection(&ui->lock);
    if (ui->ws == context) ui->ws = NULL;
    LeaveCriticalSection(&ui->lock);
    SetEvent(context->stop);
    ws_discard(context);
    WaitForSingleObject(context->thread, INFINITE);
    CloseHandle(context->thread);
    CloseHandle(context->stop);
    CloseHandle(context->reconnect);
    DeleteCriticalSection(&context->lock);
    sodium_memzero(context->token, sizeof(context->token));
    free(context);
    *owner = NULL;
}
/* 按客户端设置创建独立事件连接上下文及线程；成功上下文由 ws_stop 统一销毁。 */
static ui_ws_context *ws_start(drop_ui *ui, drop_client *client) {
    ui_ws_context *context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    context->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    context->reconnect = CreateEventW(NULL, TRUE, FALSE, NULL);
    context->wake = ui->wake;
    context->app_stop = ui->stop;
    context->heartbeat_seconds = client->heartbeat_seconds;
    context->timeout_seconds = client->connection_timeout_seconds;
    wcscpy_s(context->url, 2049, client->url);
    wcscpy_s(context->ca, 32768, client->ca_file);
    strcpy_s(context->token, DROP_TOKEN_CAP, client->token);
    InitializeCriticalSection(&context->lock);
    context->thread = context->stop && context->reconnect ? CreateThread(NULL, 0, ws_worker, context, 0, NULL) : NULL;
    if (!context->stop || !context->reconnect || !context->thread) {
        if (context->stop) CloseHandle(context->stop);
        if (context->reconnect) CloseHandle(context->reconnect);
        DeleteCriticalSection(&context->lock);
        free(context);
        return NULL;
    }
    EnterCriticalSection(&ui->lock);
    ui->ws = context;
    LeaveCriticalSection(&ui->lock);
    return context;
}
/* 在后台检查服务器可达性、协议和当前传输模式，HTTPS 使用指定 CA 验证证书。 */
static drop_error probe(const ui_job *job) {
    cJSON *out = NULL;
    DWORD status;
    drop_error error = drop_http_json_ca(job->url, L"GET", L"/api/v1/server-info", NULL, NULL, &out, &status, job->ca);
    if (error == DROP_OK &&
        (status != 200 || !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(out, "protocol_version")) ||
            cJSON_GetObjectItemCaseSensitive(out, "protocol_version")->valuedouble != DROP_PROTOCOL_VERSION || !drop_id_valid(drop_json_string(out, "server_id", 32)) ||
            !drop_id_valid(drop_json_string(out, "generation", 32))))
        error = DROP_E_PROTOCOL;
    cJSON_Delete(out);
    return error;
}
/* 将任务中的公开设置保存到独立界面配置，保留与秘密凭据存储的边界。 */
static int save_public_settings(const drop_ui *ui, const ui_job *job) {
    drop_ui *settings = calloc(1, sizeof(*settings));
    int ok = 0;
    if (!settings) return 0;
    wcscpy_s(settings->root, 32768, ui->root);
    wcscpy_s(settings->url, 2049, job->url);
    wcscpy_s(settings->ca, 32768, job->ca);
    settings->notifications = job->notifications;
    settings->tray_flash = job->tray_flash;
    settings->autostart = job->autostart;
    settings->use_zstd = job->use_zstd;
    settings->zstd_level = job->zstd_level;
    ok = drop_to_wide(job->login, settings->login, 65) && drop_ui_settings_save(settings);
    free(settings);
    return ok;
}
/* 根据本机队列、下载和恢复复核状态生成界面状态文本，不将尚未完成任务报告为已保存。 */
static void status_text(ui_result *result, drop_client *client) {
    WCHAR error[64];
    drop_to_wide(drop_error_name(result->error), error, 64);
    if (result->type == JOB_SETTINGS) {
        if (result->error == DROP_OK)
            wcscpy_s(result->status, 256, L"配置已保存。连接或账号的修改在下次登录时使用。");
        else
            swprintf_s(result->status, 256, L"无法保存设置（Windows 错误 %lu），请核对自动启动开关。", result->system_error);
    } else if (result->type == JOB_CONTACT && result->error != DROP_OK)
        wcscpy_s(result->status, 256, L"查找账号失败，请核对完整登录名与服务器连接。");
    else if (result->error == DROP_E_AUTH)
        wcscpy_s(result->status, 256, L"凭据失效或账号停用 / 删除，请重新登录；本地副本保留。");
    else if (!result->ready)
        wcscpy_s(result->status, 256, result->error == DROP_OK ? L"尚未登录。配置服务器后登录。" : L"登录失败：检查账号、密码、服务器证书和连接。");
    else if (result->error == DROP_E_SPACE)
        wcscpy_s(result->status, 256, L"磁盘空间或账号配额不足；队列与副本保留，释放空间后同步 / 重试。");
    else if (result->error == DROP_E_PENDING)
        wcscpy_s(result->status, 256, L"服务器正在校验附件；完成后继续提交，正文同步仍在进行。");
    else if (result->error != DROP_OK)
        swprintf_s(result->status, 256, L"%s；已保存的发送队列和下载进度保留，可重试。", error);
    else if (result->type == JOB_EXPORT)
        wcscpy_s(result->status, 256, L"附件已导出到所选位置。");
    else if (result->history && result->history->restore_review)
        swprintf_s(result->status, 256, L"服务器已恢复 · 本机副本保留 · %d 个发送任务待核对；核对后可明确重新提交。", result->history->restore_review);
    else if (result->history)
        swprintf_s(result->status, 256, L"已登录 · 未读 %d · 待发送 %d · 附件待完成 %d · 本次下载 %llu 字节", result->history->unread, result->history->pending_count,
            result->history->downloads, (unsigned long long)client->downloaded_bytes);
    else
        wcscpy_s(result->status, 256, L"已登录，本机副本已保存。");
}
/* 独占本机同步核心，处理界面任务及定时补拉；发送失败仍拉取新正文，退出前关闭事件连接并释放任务。 */
DWORD WINAPI drop_ui_worker(void *data) {
    drop_ui *ui = data;
    drop_client *client = calloc(1, sizeof(*client));
    ui_ws_context *ws = NULL;
    HANDLE events[2] = {ui->stop, ui->wake};
    int ready = 0, auth_failed = 0, page = 0;
    char search[1025] = {0};
    char peer[DROP_ID_CAP] = {0};
    int64_t from = 0, to = INT64_MAX;
    ULONGLONG next_sync = 0;
    DWORD wait = 0;
    unsigned retry = 0;
    static const DWORD retry_delays[] = {1000, 2000, 4000, 8000, 16000, 30000, 60000};
    if (!client) return 1;
    drop_network_cancel_event(ui->stop);
    ready = drop_client_open(client, ui->root);
    if (ready) {
        client->sync_page_limit = 2;
        client->download_quantum = 4 * DROP_CHUNK_SIZE;
    }
    if (ready) ws = ws_start(ui, client);
    SetEvent(ui->wake);
    while (WaitForMultipleObjects(2, events, FALSE, wait) != WAIT_OBJECT_0) {
        ui_job *job;
        ui_result *result = calloc(1, sizeof(*result));
        drop_error error = DROP_OK;
        if (!result) break;
        EnterCriticalSection(&ui->lock);
        job = ui->job;
        ui->job = NULL;
        LeaveCriticalSection(&ui->lock);
        if (job) {
            result->type = job->type;
            result->selection = job->selection;
            result->task_done = 1;
            if (job->type == JOB_SETTINGS) {
                drop_ui *settings = calloc(1, sizeof(*settings));
                if (!settings)
                    error = DROP_E_IO;
                else {
                    wcscpy_s(settings->root, 32768, ui->root);
                    wcscpy_s(settings->class_name, 80, ui->class_name);
                    wcscpy_s(settings->url, 2049, job->url);
                    wcscpy_s(settings->ca, 32768, job->ca);
                    drop_to_wide(job->login, settings->login, 65);
                    settings->notifications = job->notifications;
                    settings->tray_flash = job->tray_flash;
                    settings->autostart = job->autostart;
                    settings->use_zstd = job->use_zstd;
                    settings->zstd_level = job->zstd_level;
                    if ((job->previous_autostart != settings->autostart && !drop_ui_autostart(settings, settings->autostart)) || !drop_ui_settings_save(settings)) {
                        error = DROP_E_IO;
                        result->system_error = GetLastError();
                    } else {
                        result->settings_saved = 1;
                        result->notifications = settings->notifications;
                        result->tray_flash = settings->tray_flash;
                        result->autostart = settings->autostart;
                        result->use_zstd = settings->use_zstd;
                        result->zstd_level = settings->zstd_level;
                        if (ready) {
                            client->use_zstd = settings->use_zstd;
                            client->zstd_level = settings->zstd_level;
                        }
                    }
                    free(settings);
                }
            } else if (job->type == JOB_LOGIN) {
                error = save_public_settings(ui, job) ? probe(job) : DROP_E_IO;
                if (error == DROP_OK) {
                    ws_stop(ui, &ws);
                    if (ready) drop_client_close(client);
                    ready = 0;
                    if (!drop_client_login(ui->root, job->url, job->login, job->password, job->ca))
                        error = DROP_E_AUTH;
                    else if (!(ready = drop_client_open(client, ui->root)))
                        error = DROP_E_DATABASE;
                    if (ready) {
                        auth_failed = 0;
                        peer[0] = 0;
                        page = 0;
                        search[0] = 0;
                        from = 0;
                        to = INT64_MAX;
                        if (!drop_ui_contact(client, client->user, job->login)) error = DROP_E_DATABASE;
                        client->sync_page_limit = 2;
                        client->download_quantum = 4 * DROP_CHUNK_SIZE;
                        ws = ws_start(ui, client);
                    }
                }
            } else if (job->type == JOB_PROBE) {
                error = save_public_settings(ui, job) ? probe(job) : DROP_E_IO;
                result->ready = ready && !auth_failed;
                result->error = error;
                drop_url address;
                int tls = drop_url_parse(job->url, &address) && address.secure;
                wcscpy_s(result->status, 256,
                    error == DROP_OK                  ? (tls ? L"连接成功：证书与协议版本通过。登录状态独立。" : L"HTTP 可连接：登录后通过 PAKE 验证服务器并加密通信。")
                        : error == DROP_E_CERTIFICATE ? L"证书验证失败：核对主机名、有效期与信任 CA。"
                        : error == DROP_E_PROTOCOL    ? L"协议版本或服务器响应不受支持。"
                                                      : L"网络连接失败：检查服务器地址、端口或代理。");
                drop_ui_job_free(job);
                if (!PostMessageW(ui->window, DROP_UI_RESULT, 0, (LPARAM)result)) drop_ui_result_free(result);
                continue;
            } else if (job->type == JOB_LOGOUT) {
                WCHAR path[32768];
                cJSON *out = NULL;
                DWORD status;
                if (ready) {
                    (void)drop_http_json_ca(client->url, L"POST", L"/api/v1/logout", client->token, NULL, &out, &status, client->ca_file);
                    cJSON_Delete(out);
                }
                ws_stop(ui, &ws);
                if (ready) drop_client_close(client);
                ready = 0;
                if (drop_path_join(ui->root, L"session.dpapi", path, 32768) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(path)) error = DROP_E_IO;
            } else if (!ready)
                error = DROP_E_AUTH;
            else if (job->type == JOB_SEND) {
                char recipient[DROP_ID_CAP], request[DROP_ID_CAP];
                if (*job->id && drop_id_valid(job->id)) {
                    strcpy_s(recipient, DROP_ID_CAP, job->id);
                    error = DROP_OK;
                } else
                    error = drop_ui_recipient(client, job->recipient, recipient);
                if (error == DROP_OK)
                    error = job->count ? drop_client_queue_files(client, job->body, recipient, (const WCHAR *const *)job->paths, job->count, request)
                                       : drop_client_queue(client, job->body, recipient, request);
                result->queued = error == DROP_OK;
            } else if (job->type == JOB_VIEW) {
                page = job->page;
                strcpy_s(search, sizeof(search), job->search);
                from = job->from;
                to = job->to;
                strcpy_s(peer, DROP_ID_CAP, job->peer);
            } else if (job->type == JOB_CONTACT) {
                char resolved[DROP_ID_CAP];
                error = drop_ui_recipient(client, job->recipient, resolved);
                if (error == DROP_OK) {
                    strcpy_s(peer, DROP_ID_CAP, resolved);
                    page = 0;
                    search[0] = 0;
                    from = 0;
                    to = INT64_MAX;
                }
            } else if (job->type == JOB_DETAIL) {
                result->detail = drop_ui_detail(client, job->id, job->pending);
                if (!result->detail)
                    error = DROP_E_DATABASE;
                else if (!job->pending && !drop_ui_mark_read(client, job->id))
                    error = DROP_E_DATABASE;
            } else if (job->type == JOB_EXPORT)
                error = drop_ui_export(client, &job->file, job->destination);
            else if (job->type == JOB_RESTORE_RETRY)
                error = drop_client_retry_review(client);
        }
        if (ready && error == DROP_OK && !auth_failed &&
            (!job || job->type == JOB_NONE || job->type == JOB_LOGIN || job->type == JOB_SEND || job->type == JOB_DETAIL || job->type == JOB_RESTORE_RETRY)) {
            drop_error send_error = drop_client_flush(client), sync_error;
            /* 即使待发送附件上传失败，也继续拉取新正文，避免发送故障阻塞接收。 */
            client->downloaded_bytes = 0;
            sync_error = drop_client_sync(client);
            error = send_error != DROP_OK ? send_error : sync_error;
            if (send_error == DROP_E_AUTH || sync_error == DROP_E_AUTH) {
                auth_failed = 1;
                ws_stop(ui, &ws);
                error = DROP_E_AUTH;
            }
            if (error == DROP_E_NETWORK) {
                wait = retry_delays[retry < 6 ? retry : 6];
                if (retry < 6) ++retry;
            } else if (error == DROP_E_PENDING)
                wait = 1000;
            else if (error == DROP_E_SPACE || error == DROP_E_CRYPTO || error == DROP_E_IO)
                wait = 60000;
            else {
                retry = 0;
                wait = client->more_bodies || client->more_downloads || client->more_profiles ? 1000 : client->reconcile_seconds * 1000;
            }
            next_sync = GetTickCount64() + wait;
        }
        if (ready && auth_failed && error == DROP_OK) error = DROP_E_AUTH;
        result->ready = ready && !auth_failed;
        result->error = error;
        if (ready) {
            result->history = drop_ui_history(client, page, search, from, to, peer);
            if (!result->history && error == DROP_OK) result->error = DROP_E_DATABASE;
        }
        status_text(result, client);
        drop_ui_job_free(job);
        if (!PostMessageW(ui->window, DROP_UI_RESULT, 0, (LPARAM)result)) drop_ui_result_free(result);
        {
            ULONGLONG now = GetTickCount64();
            wait = ready ? (next_sync > now ? (DWORD)(next_sync - now) : 0) : 300000;
        }
    }
    ws_stop(ui, &ws);
    if (ready) drop_client_close(client);
    free(client);
    return 0;
}

/* 云盘管理命令和后台密文镜像，云盘故障不阻塞主消息服务。 */
#include "cloud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 读取云盘命令中按名称和值成对提供的参数，缺失时返回空指针。 */
static const WCHAR *argument(int argc, WCHAR **argv, const WCHAR *name) {
    int i;
    for (i = 2; i + 1 < argc; i += 2)
        if (!wcscmp(argv[i], name)) return argv[i + 1];
    return NULL;
}
/* 分派云盘状态、上传、列出、下载和显式删除命令；统一清理授权状态并报告业务错误。 */
int drop_cloud_command(drop_server *server, int argc, WCHAR **argv) {
    drop_cloud *cloud = calloc(1, sizeof(*cloud));
    drop_error error = DROP_E_ARGUMENT;
    const WCHAR *parameter;
    char task[33];
    cJSON *files = NULL, *entry;
    if (!cloud) return 0;
    if (!wcscmp(argv[1], L"drive-authorize")) {
        parameter = argument(argc, argv, L"--oauth-client");
        error = parameter && drop_cloud_authorize(server, parameter) ? DROP_OK : DROP_E_AUTH;
        goto done;
    }
#if DROP_DIAGNOSTICS
    if (!wcscmp(argv[1], L"--test-drive-authorize")) {
        const WCHAR *origin = argument(argc, argv, L"--origin"), *ca = argument(argc, argv, L"--ca");
        error = origin && ca && drop_cloud_test_authorize(server, origin, ca) ? DROP_OK : DROP_E_ARGUMENT;
        goto done;
    }
#endif
    if (!wcscmp(argv[1], L"drive-status")) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(server->database,
                "SELECT task_id,state,error,(SELECT count(*) FROM cloud_files f WHERE f.task_id=b.task_id AND state='complete'),(SELECT count(*) FROM cloud_files f WHERE "
                "f.task_id=b.task_id),completed_at FROM cloud_backups b ORDER BY task_id",
                -1, &s, NULL) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW)
                printf("backup=%s state=%s error=%s files=%lld/%lld completed_at=%lld\n", sqlite3_column_text(s, 0), sqlite3_column_text(s, 1), sqlite3_column_text(s, 2),
                    sqlite3_column_int64(s, 3), sqlite3_column_int64(s, 4), sqlite3_column_int64(s, 5));
            error = DROP_OK;
        }
        sqlite3_finalize(s);
        goto done;
    }
    error = drop_cloud_open(cloud, server);
    if (error != DROP_OK) goto done;
    if (!wcscmp(argv[1], L"drive-upload")) {
        parameter = argument(argc, argv, L"--backup");
        error = parameter ? drop_cloud_upload(cloud, parameter) : DROP_E_ARGUMENT;
    } else if (!wcscmp(argv[1], L"drive-list")) {
        error = drop_cloud_list(cloud, NULL, 1, &files);
        if (error == DROP_OK) cJSON_ArrayForEach(entry, files) {
                const char *backup = drop_json_string(cJSON_GetObjectItemCaseSensitive(entry, "appProperties"), "dropBackup", 32);
                if (!drop_id_valid(backup)) {
                    error = DROP_E_PROTOCOL;
                    break;
                }
                printf("backup=%s\n", backup);
            }
    } else {
        parameter = argument(argc, argv, L"--backup-id");
        if (!parameter || !drop_to_utf8(parameter, task, sizeof(task)) || !drop_id_valid(task)) {
            error = DROP_E_ARGUMENT;
            goto done;
        }
        if (!wcscmp(argv[1], L"drive-download")) {
            parameter = argument(argc, argv, L"--target");
            error = parameter ? drop_cloud_download(cloud, task, parameter) : DROP_E_ARGUMENT;
        } else if (!wcscmp(argv[1], L"drive-delete"))
            error = drop_cloud_delete(cloud, task);
    }
done:
    printf("Drive: %s\n", drop_error_name(error));
    cJSON_Delete(files);
    drop_cloud_close(cloud);
    free(cloud);
    return error == DROP_OK;
}
/* 扫描已完成的本地密文备份，跳过已镜像或明确删除的任务；授权或网络失败留待后续重试。 */
static void scan(drop_server *server) {
    WCHAR *search = malloc(32768 * sizeof(WCHAR)), *root = malloc(32768 * sizeof(WCHAR)), *marker = malloc(32768 * sizeof(WCHAR));
    WIN32_FIND_DATAW item;
    HANDLE find = INVALID_HANDLE_VALUE;
    drop_cloud *cloud = calloc(1, sizeof(*cloud));
    if (!search || !root || !marker || !cloud || !drop_path_join(server->backup_directory, L"*", search, 32768)) goto done;
    find = FindFirstFileW(search, &item);
    if (find == INVALID_HANDLE_VALUE) goto done;
    if (drop_cloud_open(cloud, server) != DROP_OK) {
        fputs("cloud_authorization_pending\n", stderr);
        goto done;
    }
    do {
        char task[33];
        sqlite3_stmt *s = NULL;
        int skip = 0;
        drop_error error;
        if (WaitForSingleObject(server->stop_event, 0) == WAIT_OBJECT_0) break;
        if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !drop_to_utf8(item.cFileName, task, sizeof(task)) ||
            !drop_id_valid(task) || !drop_path_join(server->backup_directory, item.cFileName, root, 32768) || !drop_path_join(root, L"complete.bin", marker, 32768) ||
            GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES)
            continue;
        EnterCriticalSection(&server->database_lock);
        if (sqlite3_prepare_v2(server->database, "SELECT state FROM cloud_backups WHERE task_id=?1", -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, task, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) == SQLITE_ROW) {
                const char *state = (const char *)sqlite3_column_text(s, 0);
                skip = !strcmp(state, "complete") || !strcmp(state, "deleted");
            }
        }
        sqlite3_finalize(s);
        LeaveCriticalSection(&server->database_lock);
        if (skip) continue;
        error = drop_cloud_upload(cloud, root);
        fprintf(stderr, "cloud_backup task=%s status=%s\n", task, drop_error_name(error));
        if (error != DROP_OK) break;
    } while (FindNextFileW(find, &item));
done:
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
    if (cloud) {
        drop_cloud_close(cloud);
        free(cloud);
    }
    free(search);
    free(root);
    free(marker);
}
/* 周期扫描云盘镜像任务，退出事件停止循环；云盘失败与主消息服务隔离。 */
static DWORD WINAPI worker(void *opaque) {
    drop_server *server = opaque;
    do {
        scan(server);
    } while (WaitForSingleObject(server->stop_event, 60000) == WAIT_TIMEOUT);
    return 0;
}
/* 仅在存在备份目录及本机授权材料时启动镜像线程，未配置云盘不妨碍服务器启动。 */
int drop_cloud_start(drop_server *server) {
    WCHAR path[32768];
    int exists;
    if (!*server->backup_directory) return 1;
    exists = drop_path_join(server->directory, L"drive-user.dpapi", path, 32768) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    exists |= drop_path_join(server->directory, L"drive-machine.dpapi", path, 32768) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    if (!exists) return 1;
    server->cloud_thread = CreateThread(NULL, 0, worker, server, 0, NULL);
    return server->cloud_thread != NULL;
}
/* 通知并等待镜像线程结束，再释放句柄，确保服务器及密钥不被提前销毁。 */
void drop_cloud_stop(drop_server *server) {
    if (server->cloud_thread) {
        SetEvent(server->stop_event);
        WaitForSingleObject(server->cloud_thread, INFINITE);
        CloseHandle(server->cloud_thread);
        server->cloud_thread = NULL;
    }
}

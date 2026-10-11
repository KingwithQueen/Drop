/* 仅本机命令调用的账号、设备及服务身份管理，不作为公网管理接口。 */
#include "server.h"
#include "cloud.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* 在事务中启停账号或重置密码及 PAKE 材料，并撤销旧凭据；失败回滚，不改动既有消息。 */
int drop_admin_account(drop_server *server, const char *login, int enabled, const char *password) {
    sqlite3_stmt *s = NULL;
    char hash[crypto_pwhash_STRBYTES] = {0};
    int ok = 0;
    drop_pake_record record;
    char user[DROP_ID_CAP];
    if (!login || (password && (strlen(password) < 12 || strlen(password) > 1024 || !drop_utf8_valid(password, strlen(password))))) return 0;
    if (password && crypto_pwhash_str_alg(hash, password, strlen(password), crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13))
        return 0;
    if (password && !drop_pake_account_make(server, login, password, &record)) {
        sodium_memzero(hash, sizeof(hash));
        return 0;
    }
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_exec(server->database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(server->database,
            password ? "UPDATE accounts SET password_hash=?1,failed_logins=0,locked_until=0 WHERE login=?2 AND deleted_at=0"
                     : "UPDATE accounts SET enabled=?1 WHERE login=?2 AND deleted_at=0",
            -1, &s, NULL) != SQLITE_OK)
        goto rollback;
    if (password)
        sqlite3_bind_text(s, 1, hash, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_int(s, 1, enabled);
    sqlite3_bind_text(s, 2, login, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) != SQLITE_DONE || sqlite3_changes(server->database) != 1) goto rollback;
    if (password) {
        sqlite3_stmt *account = NULL;
        if (sqlite3_prepare_v2(server->database, "SELECT user_id FROM accounts WHERE login=?1", -1, &account, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_text(account, 1, login, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(account) != SQLITE_ROW || !drop_id_valid((const char *)sqlite3_column_text(account, 0))) {
            sqlite3_finalize(account);
            goto rollback;
        }
        strcpy_s(user, sizeof(user), (const char *)sqlite3_column_text(account, 0));
        sqlite3_finalize(account);
        if (!drop_pake_account_store(server, user, &record)) goto rollback;
    }
    sqlite3_finalize(s);
    s = NULL;
    if (password || !enabled) {
        if (sqlite3_prepare_v2(server->database, "UPDATE devices SET revoked=1 WHERE user_id=(SELECT user_id FROM accounts WHERE login=?1)", -1, &s, NULL) != SQLITE_OK)
            goto rollback;
        sqlite3_bind_text(s, 1, login, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
    }
    sqlite3_finalize(s);
    s = NULL;
    ok = sqlite3_exec(server->database, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
rollback:
    if (!ok) sqlite3_exec(server->database, "ROLLBACK", NULL, NULL, NULL);
done:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    sodium_memzero(hash, sizeof(hash));
    sodium_memzero(&record, sizeof(record));
    return ok;
}
/* 按登录名和设备 ID 撤销设备及其凭据，其他设备和历史保留。 */
int drop_admin_revoke(drop_server *server, const char *login, const char *device) {
    sqlite3_stmt *s = NULL;
    int ok = 0;
    if (!drop_id_valid(device)) return 0;
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, "UPDATE devices SET revoked=1 WHERE device_id=?1 AND user_id=(SELECT user_id FROM accounts WHERE login=?2)", -1, &s, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(s, 1, device, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, login, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(server->database) == 1;
    }
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    return ok;
}
/* 先保存 schema 10 的加密回退快照，再加入删除时间；账户 ID 和历史外键保持不变。 */
int drop_admin_schema(drop_server *server) {
    sqlite3_stmt *s = NULL;
    int version = -1;
    if (sqlite3_prepare_v2(server->database, "PRAGMA user_version", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (version == 11) return 1;
    if (version != 10 || !drop_schema_snapshot(server, 10)) return 0;
    if (sqlite3_exec(server->database,
            "BEGIN IMMEDIATE; ALTER TABLE accounts ADD COLUMN deleted_at INTEGER NOT NULL DEFAULT 0; ALTER TABLE accounts ADD COLUMN former_login TEXT NOT NULL DEFAULT ''; PRAGMA "
            "user_version=11; COMMIT;",
            NULL, NULL, NULL) == SQLITE_OK)
        return 1;
    sqlite3_exec(server->database, "ROLLBACK", NULL, NULL, NULL);
    return 0;
}
/* 在一个事务中改名或删除可登录身份；删除清除认证材料并释放登录名，保留原 ID、显示名、设备和历史外键。 */
int drop_admin_identity(drop_server *server, const char *login, const char *display, int deleting) {
    sqlite3_stmt *s = NULL;
    char user[DROP_ID_CAP], tombstone[64];
    int ok = 0;
    if (!drop_account_login_valid(login) || (!deleting && (!display || !*display || strlen(display) > 128 || !drop_utf8_valid(display, strlen(display))))) return 0;
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_exec(server->database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(server->database, "SELECT user_id FROM accounts WHERE login=?1 AND deleted_at=0", -1, &s, NULL) != SQLITE_OK) goto rollback;
    sqlite3_bind_text(s, 1, login, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) != SQLITE_ROW) goto rollback;
    strcpy_s(user, sizeof(user), (const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    s = NULL;
    if (deleting) {
        sprintf_s(tombstone, sizeof(tombstone), "!deleted!%s", user);
        if (sqlite3_prepare_v2(server->database,
                "UPDATE accounts SET former_login=login,login=?1,enabled=0,password_hash='',failed_logins=0,locked_until=0,deleted_at=?2 WHERE user_id=?3", -1, &s,
                NULL) != SQLITE_OK)
            goto rollback;
        sqlite3_bind_text(s, 1, tombstone, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)time(NULL));
        sqlite3_bind_text(s, 3, user, -1, SQLITE_TRANSIENT);
    } else {
        if (sqlite3_prepare_v2(server->database, "UPDATE accounts SET display_name=?1 WHERE user_id=?2", -1, &s, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_text(s, 1, display, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, user, -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(s) != SQLITE_DONE || sqlite3_changes(server->database) != 1) goto rollback;
    sqlite3_finalize(s);
    s = NULL;
    if (deleting) {
        if (sqlite3_prepare_v2(server->database, "UPDATE devices SET revoked=1 WHERE user_id=?1", -1, &s, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_text(s, 1, user, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
        sqlite3_finalize(s);
        s = NULL;
        if (sqlite3_prepare_v2(server->database, "DELETE FROM account_pake WHERE user_id=?1", -1, &s, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_text(s, 1, user, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
    }
    ok = sqlite3_exec(server->database, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
rollback:
    if (!ok) sqlite3_exec(server->database, "ROLLBACK", NULL, NULL, NULL);
done:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    return ok;
}
/* 批量读取明确指定的账户资料；仅自己的身份、既有会话参与者或匹配的已知登录名允许返回。 */
drop_error drop_profiles_read(drop_server *server, const char *user, const cJSON *request, cJSON **response) {
    const cJSON *users = cJSON_GetObjectItemCaseSensitive(request, "users"), *entry;
    sqlite3_stmt *s = NULL;
    cJSON *profiles;
    drop_error error = DROP_E_DATABASE;
    if (!cJSON_IsArray(users) || cJSON_GetArraySize(users) > 100) return DROP_E_ARGUMENT;
    *response = cJSON_CreateObject();
    if (!*response || !(profiles = cJSON_AddArrayToObject(*response, "profiles"))) return DROP_E_IO;
    EnterCriticalSection(&server->database_lock);
    cJSON_ArrayForEach(entry, users) {
        const char *id = drop_json_string(entry, "user_id", 32), *login = drop_json_string(entry, "login", 64);
        cJSON *profile;
        int step;
        if (!drop_id_valid(id) || (login && *login && !drop_account_login_valid(login))) {
            error = DROP_E_ARGUMENT;
            goto done;
        }
        if (sqlite3_prepare_v2(server->database,
                "SELECT user_id,CASE WHEN deleted_at=0 THEN login ELSE '' END,display_name,enabled,deleted_at FROM accounts WHERE user_id=?1 AND "
                "(user_id=?2 OR (login=?3 AND deleted_at=0) OR (former_login=?3 AND former_login!='') OR EXISTS(SELECT 1 FROM conversations WHERE (lo=?2 AND hi=?1) OR (hi=?2 AND "
                "lo=?1)))",
                -1, &s, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, user, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, login ? login : "", -1, SQLITE_TRANSIENT);
        step = sqlite3_step(s);
        if (step == SQLITE_ROW) {
            profile = cJSON_CreateObject();
            if (!profile || !cJSON_AddStringToObject(profile, "user_id", (const char *)sqlite3_column_text(s, 0)) ||
                !cJSON_AddStringToObject(profile, "login", (const char *)sqlite3_column_text(s, 1)) ||
                !cJSON_AddStringToObject(profile, "display_name", (const char *)sqlite3_column_text(s, 2)) ||
                !cJSON_AddBoolToObject(profile, "enabled", sqlite3_column_int(s, 3)) || !cJSON_AddBoolToObject(profile, "deleted", sqlite3_column_int64(s, 4) != 0) ||
                !cJSON_AddItemToArray(profiles, profile)) {
                cJSON_Delete(profile);
                error = DROP_E_IO;
                goto done;
            }
        } else if (step != SQLITE_DONE)
            goto done;
        sqlite3_finalize(s);
        s = NULL;
    }
    error = DROP_OK;
done:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    return error;
}
/* 执行同一套在线及离线管理操作；成功响应不含密码或认证材料，昂贵操作不持有全程数据库锁。 */
drop_error drop_admin_execute(drop_server *server, const cJSON *request, cJSON **response) {
    const char *operation = drop_json_string(request, "operation", 32), *login = drop_json_string(request, "login", 64), *display = drop_json_string(request, "display", 128);
    const char *password = drop_json_string(request, "password", 1024), *value;
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_ARGUMENT;
    int ok = 0;
    uint64_t bytes;
    WCHAR *path = NULL;
    if (!operation || (login && !drop_account_login_valid(login)) || (cJSON_HasObjectItem(request, "display") && (!display || !*display))) return DROP_E_ARGUMENT;
    *response = cJSON_CreateObject();
    if (!*response) return DROP_E_IO;
    if (!strcmp(operation, "account-create"))
        error = login && password ? drop_account_create_status(server, login, display ? display : login, password) : DROP_E_ARGUMENT;
    else if (!strcmp(operation, "account-display") || !strcmp(operation, "account-delete")) {
        ok = login && drop_admin_identity(server, login, display, !strcmp(operation, "account-delete"));
        error = ok ? DROP_OK : DROP_E_ARGUMENT;
    } else if (!strcmp(operation, "account-enable") || !strcmp(operation, "account-disable") || !strcmp(operation, "account-reset")) {
        if (!strcmp(operation, "account-reset") && !password) return DROP_E_ARGUMENT;
        ok = login && drop_admin_account(server, login, !strcmp(operation, "account-enable"), !strcmp(operation, "account-reset") ? password : NULL);
        error = ok ? DROP_OK : DROP_E_ARGUMENT;
    } else if (!strcmp(operation, "device-revoke")) {
        value = drop_json_string(request, "device", 32);
        error = login && value && drop_admin_revoke(server, login, value) ? DROP_OK : DROP_E_ARGUMENT;
    } else if (!strcmp(operation, "quota-set")) {
        if (!login || !drop_u64_parse(drop_json_string(request, "bytes", 20), &bytes) || bytes > INT64_MAX) return DROP_E_ARGUMENT;
        EnterCriticalSection(&server->database_lock);
        if (sqlite3_prepare_v2(server->database, "UPDATE accounts SET quota_bytes=?1 WHERE login=?2 AND deleted_at=0", -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)bytes);
            sqlite3_bind_text(s, 2, login, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(server->database) == 1;
        }
        sqlite3_finalize(s);
        s = NULL;
        LeaveCriticalSection(&server->database_lock);
        error = ok ? DROP_OK : DROP_E_ARGUMENT;
    } else if (!strcmp(operation, "storage-status") || !strcmp(operation, "account-list") || !strcmp(operation, "drive-status")) {
        cJSON *rows = cJSON_AddArrayToObject(*response, "rows");
        int accounts = !strcmp(operation, "account-list"), count = 0;
        char next[65] = {0};
        const char *after = drop_json_string(request, "after", 64);
        if (accounts && cJSON_HasObjectItem(request, "after") && !after) return DROP_E_ARGUMENT;
        const char *query = accounts             ? "SELECT user_id,login,display_name,enabled,quota_bytes FROM accounts WHERE deleted_at=0 AND login>?1 ORDER BY login LIMIT 101"
            : !strcmp(operation, "drive-status") ? "SELECT task_id,state,error,(SELECT count(*) FROM cloud_files f WHERE f.task_id=b.task_id AND state='complete'),(SELECT "
                                                   "count(*) FROM cloud_files f WHERE f.task_id=b.task_id),completed_at FROM cloud_backups b ORDER BY task_id"
                                                 : "SELECT count(*),coalesce(sum(compressed=1),0) FROM messages";
        int step = SQLITE_ERROR;
        EnterCriticalSection(&server->database_lock);
        if (rows && sqlite3_prepare_v2(server->database, query, -1, &s, NULL) == SQLITE_OK) {
            if (accounts) sqlite3_bind_text(s, 1, after ? after : "", -1, SQLITE_TRANSIENT);
            while ((step = sqlite3_step(s)) == SQLITE_ROW) {
                cJSON *row;
                int column;
                if (accounts && count == 100) break;
                row = cJSON_CreateArray();
                if (!row) break;
                for (column = 0; column < sqlite3_column_count(s); ++column) {
                    const unsigned char *text = sqlite3_column_text(s, column);
                    cJSON *field = cJSON_CreateString(text ? (const char *)text : "");
                    if (!field || !cJSON_AddItemToArray(row, field)) {
                        cJSON_Delete(field);
                        break;
                    }
                }
                if (column != sqlite3_column_count(s) || !cJSON_AddItemToArray(rows, row)) {
                    cJSON_Delete(row);
                    break;
                }
                ++count;
                if (accounts) strcpy_s(next, sizeof(next), (const char *)sqlite3_column_text(s, 1));
            }
        }
        error = step == SQLITE_DONE || (accounts && step == SQLITE_ROW && count == 100) ? DROP_OK : DROP_E_DATABASE;
        if (accounts && error == DROP_OK &&
            (!cJSON_AddBoolToObject(*response, "more", step == SQLITE_ROW) || !cJSON_AddStringToObject(*response, "next", step == SQLITE_ROW ? next : "")))
            error = DROP_E_IO;
        sqlite3_finalize(s);
        s = NULL;
        LeaveCriticalSection(&server->database_lock);
    } else if (!strcmp(operation, "cleanup"))
        error = drop_upload_cleanup(server, (uint64_t)time(NULL));
    else if (!strcmp(operation, "backup") || !strcmp(operation, "recovery-export")) {
        char task[DROP_ID_CAP] = {0};
        value = drop_json_string(request, "target", 131071);
        path = malloc(32768 * sizeof(WCHAR));
        if (value && path && drop_to_wide(value, path, 32768)) {
            ok = !strcmp(operation, "backup") ? drop_backup_create(server, path, task) : password && drop_recovery_export(server, path, password);
            error = ok ? DROP_OK : DROP_E_IO;
            if (ok && *task && !cJSON_AddStringToObject(*response, "backup_id", task)) error = DROP_E_IO;
        }
        free(path);
    } else if (!strcmp(operation, "drive-list") || !strcmp(operation, "drive-upload") || !strcmp(operation, "drive-download") || !strcmp(operation, "drive-delete")) {
        drop_cloud *cloud = calloc(1, sizeof(*cloud));
        cJSON *files = NULL;
        if (!cloud) return DROP_E_IO;
        error = drop_cloud_open(cloud, server);
        if (error == DROP_OK) {
            const char *id = drop_json_string(request, "backup-id", 32);
            value = drop_json_string(request, !strcmp(operation, "drive-upload") ? "backup" : "target", 131071);
            path = malloc(32768 * sizeof(WCHAR));
            if (!strcmp(operation, "drive-list")) {
                error = drop_cloud_list(cloud, NULL, 1, &files);
                if (error == DROP_OK && !cJSON_AddItemToObject(*response, "files", files)) {
                    cJSON_Delete(files);
                    error = DROP_E_IO;
                }
            } else if (!strcmp(operation, "drive-upload"))
                error = value && path && drop_to_wide(value, path, 32768) ? drop_cloud_upload(cloud, path) : DROP_E_ARGUMENT;
            else if (!drop_id_valid(id))
                error = DROP_E_ARGUMENT;
            else if (!strcmp(operation, "drive-delete"))
                error = drop_cloud_delete(cloud, id);
            else
                error = value && path && drop_to_wide(value, path, 32768) ? drop_cloud_download(cloud, id, path) : DROP_E_ARGUMENT;
            free(path);
        }
        drop_cloud_close(cloud);
        free(cloud);
    }
    if (error == DROP_OK) {
        const char *message = !strcmp(operation, "account-delete")                        ? "Account deleted; login name released, history retained."
            : !strcmp(operation, "account-display")                                       ? "Account display name updated; login unchanged."
            : !strcmp(operation, "cleanup")                                               ? "Expired unreferenced uploads processed; history retained."
            : !strcmp(operation, "quota-set")                                             ? "Account quota updated; existing history retained."
            : (!strncmp(operation, "account-", 8) || !strcmp(operation, "device-revoke")) ? "Account/device management committed; existing history retained."
                                                                                          : "Management operation committed.";
        if (!cJSON_AddStringToObject(*response, "message", message)) return DROP_E_IO;
        if ((!strncmp(operation, "account-", 8) && strcmp(operation, "account-list")) || !strcmp(operation, "device-revoke")) drop_notify_accounts(server);
    }
    return error;
}
/* 收紧数据树权限并将运行密钥及云盘秘密转为机器 DPAPI，供 Windows 服务使用；失败不宣称转换完成。 */
int drop_runtime_machine(drop_server *server) {
    WCHAR *path = malloc(32768 * sizeof(WCHAR)), *old = malloc(32768 * sizeof(WCHAR)), *cloud_old = malloc(32768 * sizeof(WCHAR)), *cloud_new = malloc(32768 * sizeof(WCHAR));
    unsigned char *secret = NULL;
    DWORD size = 0;
    int ok = 0, has_cloud = 0;
    if (!path || !old || !drop_path_join(server->directory, L"runtime-machine.dpapi", path, 32768) || !drop_path_join(server->directory, L"runtime-user.dpapi", old, 32768))
        goto done;
    if (!cloud_old || !cloud_new || !drop_path_join(server->directory, L"drive-user.dpapi", cloud_old, 32768) ||
        !drop_path_join(server->directory, L"drive-machine.dpapi", cloud_new, 32768))
        goto done;
    has_cloud = GetFileAttributesW(cloud_old) != INVALID_FILE_ATTRIBUTES;
    if (has_cloud && !drop_secret_load(cloud_old, &secret, &size)) goto done;
    if (!drop_machine_tree(server->directory)) goto done;
    if (has_cloud && !drop_machine_secret_replace(cloud_new, secret, size)) goto done;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && !drop_machine_secret_save(path, &server->keys, sizeof(server->keys))) goto done;
    if (!drop_machine_tree(server->directory) || (has_cloud && !DeleteFileW(cloud_old))) goto done;
    ok = GetFileAttributesW(old) == INVALID_FILE_ATTRIBUTES || DeleteFileW(old) != 0;
done:
    if (secret) {
        sodium_memzero(secret, size);
        free(secret);
    }
    free(path);
    free(old);
    free(cloud_old);
    free(cloud_new);
    return ok;
}

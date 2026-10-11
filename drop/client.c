/* 客户端持久同步核心：同一数据目录只有一个拥有者，正文事务和附件任务共同推进，网络失败保留队列。 */
#include "client.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 执行无返回行的本机 SQL，成功返回 1；事务和数据库错误由调用者处理。 */
static int sql(sqlite3 *db, const char *text) {
    return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK;
}
/* 以 SQLITE_TRANSIENT 绑定 UTF-8 文本，使语句不依赖调用者缓冲的生命周期。 */
static void bind(sqlite3_stmt *s, int i, const char *text) {
    sqlite3_bind_text(s, i, text, -1, SQLITE_TRANSIENT);
}
/* 将本机 SQLite 错误映射为项目错误，单独报告存储空间不足。 */
drop_error drop_client_database_error(drop_client *c) {
    return sqlite3_errcode(c->database) == SQLITE_FULL ? DROP_E_SPACE : DROP_E_DATABASE;
}
/* 使用客户端当前地址、凭据和 CA 发送 JSON 请求，并将 HTTP 状态转换为业务错误。 */
static drop_error http(drop_client *c, const WCHAR *path, cJSON *body, cJSON **out) {
    DWORD status;
    drop_error error = drop_http_json_ca(c->url, L"POST", path, c->token, body, out, &status, c->ca_file);
    if (error != DROP_OK) return error;
    if (status == 200) return DROP_OK;
    cJSON_Delete(*out);
    *out = NULL;
    return status == 401 ? DROP_E_AUTH
        : status == 409  ? DROP_E_CONFLICT
        : status == 413  ? DROP_E_LIMIT
        : status == 400  ? DROP_E_ARGUMENT
        : status == 507  ? DROP_E_SPACE
                         : DROP_E_DATABASE;
}
/* 将调用者已校验的服务器或账号 ID 转为子目录名，并创建或收紧该目录的用户访问权限。 */
static int directory_id(const WCHAR *parent, const char *id, WCHAR *out) {
    WCHAR wide[DROP_ID_CAP];
    return drop_to_wide(id, wide, DROP_ID_CAP) && drop_path_join(parent, wide, out, 32768) && drop_private_directory(out);
}
/* 为客户端数据目录获取独占文件锁，确保同一目录只由一个同步核心写入。 */
static int lock_root(const WCHAR *root, WCHAR *absolute, HANDLE *lock) {
    WCHAR path[32768];
    DWORD n = GetFullPathNameW(root, 32768, absolute, NULL);
    if (!n || n >= 32768 || !drop_private_directory(absolute) || !drop_path_join(absolute, L"client.lock", path, 32768)) return 0;
    *lock = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return *lock != INVALID_HANDLE_VALUE;
}
/* 按所选 TLS 或 PAKE 模式登录并持久保存 DPAPI 凭据；检查服务器身份，失败不建立可用会话。 */
int drop_client_login(const WCHAR *root, const WCHAR *url, const char *login, const char *password, const WCHAR *ca_file) {
    WCHAR absolute[32768], path[32768];
    HANDLE lock = INVALID_HANDLE_VALUE;
    unsigned char *bytes = NULL;
    DWORD size;
    char device[DROP_ID_CAP], url_utf8[8193], *serialized = NULL;
    cJSON *body = NULL, *response = NULL, *secret = NULL;
    DWORD status;
    int ok = 0;
    if (!lock_root(root, absolute, &lock) || !drop_path_join(absolute, L"device.id", path, 32768)) goto done;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        drop_id_new(device);
        if (!drop_file_write(path, device, 32, 0)) goto done;
    } else {
        if (!drop_file_read(path, &bytes, &size, 32) || size != 32 || !drop_id_valid((char *)bytes)) goto done;
        strcpy_s(device, sizeof(device), (char *)bytes);
        free(bytes);
        bytes = NULL;
    }
    body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, "login", login) || !cJSON_AddStringToObject(body, "password", password) || !cJSON_AddStringToObject(body, "device_id", device) ||
        !cJSON_AddStringToObject(body, "device_name", "Drop Windows"))
        goto done;
    if (drop_http_json_ca(url, L"POST", L"/api/v1/login", NULL, body, &response, &status, ca_file) != DROP_OK || status != 200 ||
        !drop_id_valid(drop_json_string(response, "user_id", 32)) || !drop_id_valid(drop_json_string(response, "server_id", 32)) ||
        !drop_id_valid(drop_json_string(response, "generation", 32)) ||
        strcmp(device, drop_json_string(response, "device_id", 32) ? drop_json_string(response, "device_id", 32) : "") || !drop_json_string(response, "token", 64) ||
        strlen(drop_json_string(response, "token", 64)) != 64)
        goto done;
    if (!drop_to_utf8(url, url_utf8, sizeof(url_utf8)) || !cJSON_AddStringToObject(response, "url", url_utf8) || !drop_path_join(absolute, L"session.dpapi", path, 32768))
        goto done;
    if (ca_file && *ca_file) {
        WCHAR full_ca[32768];
        DWORD n = GetFullPathNameW(ca_file, 32768, full_ca, NULL);
        char *ca_utf8 = malloc(131072);
        if (!n || n >= 32768 || !ca_utf8 || !drop_to_utf8(full_ca, ca_utf8, 131072) || strlen(ca_utf8) > 32768 || !cJSON_AddStringToObject(response, "ca_file", ca_utf8)) {
            free(ca_utf8);
            goto done;
        }
        free(ca_utf8);
    }
    serialized = cJSON_PrintUnformatted(response);
    if (!serialized || !drop_secret_replace(path, serialized, (DWORD)strlen(serialized))) goto done;
    ok = 1;
done:
    if (body) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(body, "password");
        if (cJSON_IsString(p)) sodium_memzero(p->valuestring, strlen(p->valuestring));
    }
    if (response) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(response, "token"), *k = cJSON_GetObjectItemCaseSensitive(response, "pake_secret");
        if (cJSON_IsString(p)) sodium_memzero(p->valuestring, strlen(p->valuestring));
        if (cJSON_IsString(k)) sodium_memzero(k->valuestring, strlen(k->valuestring));
    }
    if (serialized) {
        sodium_memzero(serialized, strlen(serialized));
        cJSON_free(serialized);
    }
    cJSON_Delete(body);
    cJSON_Delete(response);
    cJSON_Delete(secret);
    free(bytes);
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    return ok;
}
/* 锁定并打开本机数据目录，加载受保护凭据、数据库及迁移状态；失败统一释放已取得资源。 */
int drop_client_open(drop_client *c, const WCHAR *root) {
    WCHAR path[32768], server_dir[32768], account_dir[32768];
    unsigned char *bytes = NULL;
    DWORD size = 0;
    cJSON *secret = NULL;
    char *utf8 = NULL;
    sqlite3_stmt *s = NULL;
    int version = -1, ok = 0;
    const char *user, *device, *server, *generation, *token, *url;
    drop_url address;
    memset(c, 0, sizeof(*c));
    c->lock = INVALID_HANDLE_VALUE;
    c->zstd_level = 3;
    c->heartbeat_seconds = 30;
    c->connection_timeout_seconds = 90;
    c->reconcile_seconds = 300;
    if (!lock_root(root, c->root, &c->lock) || !drop_path_join(c->root, L"session.dpapi", path, 32768) || !drop_secret_load(path, &bytes, &size) ||
        !(secret = drop_json_parse((char *)bytes, size)))
        goto done;
    user = drop_json_string(secret, "user_id", 32);
    device = drop_json_string(secret, "device_id", 32);
    server = drop_json_string(secret, "server_id", 32);
    generation = drop_json_string(secret, "generation", 32);
    token = drop_json_string(secret, "token", 64);
    url = drop_json_string(secret, "url", 8192);
    if (!drop_id_valid(user) || !drop_id_valid(device) || !drop_id_valid(server) || !drop_id_valid(generation) || !token || strlen(token) != 64 || !url ||
        !drop_to_wide(url, c->url, 2049) || !drop_url_parse(c->url, &address))
        goto done;
    strcpy_s(c->user, sizeof(c->user), user);
    strcpy_s(c->device, sizeof(c->device), device);
    strcpy_s(c->server, sizeof(c->server), server);
    strcpy_s(c->generation, sizeof(c->generation), generation);
    strcpy_s(c->token, sizeof(c->token), token);
    if (!drop_pake_client_bind(c->url, secret)) goto done;
    if (cJSON_GetObjectItemCaseSensitive(secret, "ca_file")) {
        const char *ca = drop_json_string(secret, "ca_file", 32768);
        if (!ca || !drop_to_wide(ca, c->ca_file, 32768)) goto done;
    }
    if (!drop_path_join(c->root, L"client.ini", path, 32768)) goto done;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        drop_config *config = malloc(sizeof(*config));
        int valid = config && drop_config_load(path, config) && !wcscmp(config->server_url, c->url);
        if (valid) {
            c->use_zstd = config->use_zstd;
            c->zstd_level = config->zstd_level;
            c->heartbeat_seconds = config->heartbeat_seconds;
            c->connection_timeout_seconds = config->connection_timeout_seconds;
            c->reconcile_seconds = config->reconcile_seconds;
        }
        free(config);
        if (!valid) goto done;
    }
    if (!drop_path_join(c->root, L"ui.json", path, 32768)) goto done;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        unsigned char *settings = NULL;
        DWORD settings_size;
        cJSON *json = NULL, *level;
        const char *algorithm;
        int valid = 0;
        if (drop_file_read(path, &settings, &settings_size, 65536) && (json = drop_json_parse((char *)settings, settings_size))) {
            algorithm = drop_json_string(json, "text_compression", 8);
            level = cJSON_GetObjectItemCaseSensitive(json, "zstd_level");
            valid = !algorithm && !level;
            if (algorithm && level && (!strcmp(algorithm, "none") || !strcmp(algorithm, "zstd")) && cJSON_IsNumber(level) && level->valueint >= 1 && level->valueint <= 19 &&
                level->valuedouble == (double)level->valueint) {
                c->use_zstd = !strcmp(algorithm, "zstd");
                c->zstd_level = level->valueint;
                valid = 1;
            }
        }
        cJSON_Delete(json);
        free(settings);
        if (!valid) goto done;
    }
    if (!directory_id(c->root, server, server_dir) || !directory_id(server_dir, user, account_dir) || !drop_path_join(account_dir, L"client.db", path, 32768)) goto done;
    utf8 = malloc(131072);
    if (!utf8 || !drop_to_utf8(path, utf8, 131072) || sqlite3_open_v2(utf8, &c->database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK)
        goto done;
    sqlite3_busy_timeout(c->database, 5000);
    if (!sql(c->database, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;") ||
        sqlite3_prepare_v2(c->database, "PRAGMA user_version", -1, &s, NULL) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW)
        goto done;
    version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    s = NULL;
    if (version < 0 || version > 4) goto done;
    if (version == 0 &&
        !sql(c->database,
            "BEGIN IMMEDIATE; CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
            "INSERT INTO meta VALUES('cursor','0'),('generation','');"
            "CREATE TABLE pending(request_id TEXT PRIMARY KEY,recipient TEXT NOT NULL,body BLOB NOT NULL,compressed INTEGER NOT NULL,"
            "original_length INTEGER NOT NULL,state TEXT NOT NULL DEFAULT 'queued',message_id TEXT,error TEXT NOT NULL DEFAULT '');"
            "CREATE TABLE messages(message_id TEXT PRIMARY KEY,request_id TEXT NOT NULL,conversation_id TEXT NOT NULL,sender TEXT NOT NULL,recipient TEXT NOT NULL,"
            "device TEXT NOT NULL,created_at INTEGER NOT NULL,body BLOB NOT NULL,compressed INTEGER NOT NULL,original_length INTEGER NOT NULL);"
            "PRAGMA user_version=1; COMMIT;"))
        goto done;
    if (sqlite3_prepare_v2(c->database, "UPDATE meta SET value=?1 WHERE key='generation' AND value=''", -1, &s, NULL) != SQLITE_OK) goto done;
    bind(s, 1, generation);
    if (sqlite3_step(s) != SQLITE_DONE) goto done;
    sqlite3_finalize(s);
    s = NULL;
    if (!drop_path_join(account_dir, L"files", c->files, 32768) || !drop_private_directory(c->files) || !drop_client_files_schema(c) || !drop_client_ui_schema(c) ||
        !drop_client_receipts_schema(c) || !drop_client_verify_files(c))
        goto done;
    if (sqlite3_prepare_v2(c->database, "SELECT value FROM meta WHERE key='generation'", -1, &s, NULL) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW ||
        !drop_id_valid((const char *)sqlite3_column_text(s, 0)))
        goto done;
    strcpy_s(c->generation, sizeof(c->generation), (const char *)sqlite3_column_text(s, 0));
    ok = 1;
done:
    sqlite3_finalize(s);
    free(utf8);
    if (secret) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(secret, "token"), *k = cJSON_GetObjectItemCaseSensitive(secret, "pake_secret");
        if (cJSON_IsString(p)) sodium_memzero(p->valuestring, strlen(p->valuestring));
        if (cJSON_IsString(k)) sodium_memzero(k->valuestring, strlen(k->valuestring));
    }
    cJSON_Delete(secret);
    if (bytes) {
        sodium_memzero(bytes, size);
        free(bytes);
    }
    if (!ok) drop_client_close(c);
    return ok;
}
/* 关闭数据库和目录锁，撤销进程内凭据绑定并清零客户端状态。 */
void drop_client_close(drop_client *c) {
    drop_pake_client_forget(c->url, c->token);
    if (c->database) sqlite3_close(c->database);
    if (c->lock != INVALID_HANDLE_VALUE) CloseHandle(c->lock);
    sodium_memzero(c, sizeof(*c));
    c->lock = INVALID_HANDLE_VALUE;
}
/* 在本机结构升级前保存对应版本的数据库快照，为失败后的手动恢复保留依据。 */
int drop_client_snapshot(drop_client *c, int version) {
    const char *source = sqlite3_db_filename(c->database, "main");
    char id[DROP_ID_CAP], *utf8 = NULL;
    WCHAR *wide = NULL, *path = NULL, wide_id[DROP_ID_CAP];
    sqlite3 *copy = NULL;
    sqlite3_backup *backup = NULL;
    sqlite3_stmt *s = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    int ok = 0;
    if (!source || !*source) return 0;
    wide = malloc(32768 * sizeof(WCHAR));
    path = malloc(32768 * sizeof(WCHAR));
    utf8 = malloc(131072);
    drop_id_new(id);
    if (!wide || !path || !utf8 || !drop_to_wide(source, wide, 32768) || !drop_to_wide(id, wide_id, DROP_ID_CAP) ||
        swprintf_s(path, 32768, L"%s.migration-v%d.%s.db", wide, version, wide_id) < 0 || !drop_to_utf8(path, utf8, 131072))
        goto done;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (sqlite3_open_v2(utf8, &copy, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK || !sql(copy, "PRAGMA synchronous=FULL;")) goto done;
    backup = sqlite3_backup_init(copy, "main", c->database, "main");
    if (!backup || sqlite3_backup_step(backup, -1) != SQLITE_DONE) goto done;
    if (sqlite3_backup_finish(backup) != SQLITE_OK) {
        backup = NULL;
        goto done;
    }
    backup = NULL;
    if (sqlite3_prepare_v2(copy, "PRAGMA integrity_check", -1, &s, NULL) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW || strcmp((const char *)sqlite3_column_text(s, 0), "ok"))
        goto done;
    sqlite3_finalize(s);
    s = NULL;
    if (sqlite3_close(copy) != SQLITE_OK) goto done;
    copy = NULL;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ok = file != INVALID_HANDLE_VALUE && FlushFileBuffers(file);
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    sqlite3_finalize(s);
    if (backup) sqlite3_backup_finish(backup);
    if (copy) sqlite3_close(copy);
    free(wide);
    free(path);
    free(utf8);
    return ok;
}
/* 将纯文本消息加入持久发送队列并生成稳定请求 ID，后续重试沿用同一 ID。 */
drop_error drop_client_queue(drop_client *c, const char *body, const char *recipient, char request[DROP_ID_CAP]) {
    drop_text packed = {0};
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_DATABASE;
    if (!body || !*body || (recipient && !drop_id_valid(recipient)) || !drop_text_pack(body, c->use_zstd, c->zstd_level ? c->zstd_level : 3, &packed)) return DROP_E_ARGUMENT;
    drop_id_new(request);
    if (sqlite3_prepare_v2(c->database, "INSERT INTO pending(request_id,recipient,body,compressed,original_length) VALUES(?1,?2,?3,?4,?5)", -1, &s, NULL) != SQLITE_OK) goto done;
    bind(s, 1, request);
    bind(s, 2, recipient ? recipient : c->user);
    sqlite3_bind_blob(s, 3, packed.data, (int)packed.size, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 4, packed.compressed);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)packed.original_size);
    if (sqlite3_step(s) == SQLITE_DONE) error = DROP_OK;
done:
    if (error == DROP_E_DATABASE && sqlite3_errcode(c->database) == SQLITE_FULL) error = DROP_E_SPACE;
    sqlite3_finalize(s);
    drop_text_free(&packed);
    return error;
}
/* 检查服务器身份和同步代次；恢复旧快照时保留本机历史，并将不确定发送任务转为待人工确认。 */
drop_error drop_client_reconcile(drop_client *c) {
    cJSON *info = NULL;
    DWORD status = 0;
    sqlite3_stmt *s = NULL;
    const char *server, *generation;
    drop_error error;
    error = drop_http_json_ca(c->url, L"GET", L"/api/v1/server-info", c->token, NULL, &info, &status, c->ca_file);
    if (error != DROP_OK) goto done;
    server = drop_json_string(info, "server_id", 32);
    generation = drop_json_string(info, "generation", 32);
    if (status != 200 || !drop_id_valid(server) || !drop_id_valid(generation) || strcmp(server, c->server)) {
        error = DROP_E_PROTOCOL;
        goto done;
    }
    if (!strcmp(generation, c->generation)) goto done;
    if (!sql(c->database,
            "BEGIN IMMEDIATE; UPDATE meta SET value='0' WHERE key IN('cursor','state_cursor','saved_ack_cursor'); DELETE FROM device_states; DELETE FROM object_states; DELETE "
            "FROM account_reads; DELETE FROM pending_reads; DELETE FROM message_orders; UPDATE downloads SET saved_ack=0,state=CASE WHEN state='complete' THEN 'complete' ELSE "
            "'restore_review' END; UPDATE pending SET state='restore_review',error='restore_review' WHERE state='queued';")) {
        error = drop_client_database_error(c);
        goto rollback;
    }
    if (sqlite3_prepare_v2(c->database, "UPDATE meta SET value=?1 WHERE key='generation'", -1, &s, NULL) != SQLITE_OK) {
        error = drop_client_database_error(c);
        goto rollback;
    }
    bind(s, 1, generation);
    if (sqlite3_step(s) != SQLITE_DONE || !sql(c->database, "COMMIT")) {
        error = drop_client_database_error(c);
        goto rollback;
    }
    strcpy_s(c->generation, sizeof(c->generation), generation);
    c->restored = 1;
    goto done;
rollback:
    sql(c->database, "ROLLBACK");
done:
    sqlite3_finalize(s);
    cJSON_Delete(info);
    return error;
}
/* 显式恢复经用户确认的待复核发送任务，使其重新进入发送流程。 */
drop_error drop_client_retry_review(drop_client *c) {
    drop_error error = drop_client_reconcile(c);
    if (error != DROP_OK) return error;
    return sql(c->database, "UPDATE pending SET state='queued',error='' WHERE state='restore_review'") ? DROP_OK : drop_client_database_error(c);
}
/* 依次上传持久待发送消息及附件；服务器确认后才更新状态，失败保留任务及原请求 ID。 */
drop_error drop_client_flush(drop_client *c) {
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_OK;
    int step;
    error = drop_client_reconcile(c);
    if (error != DROP_OK) return error;
    if (sqlite3_prepare_v2(c->database, "SELECT request_id,recipient,body,compressed,original_length FROM pending WHERE state='queued' ORDER BY rowid", -1, &s, NULL) != SQLITE_OK)
        return DROP_E_DATABASE;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        char request[DROP_ID_CAP], *body = NULL;
        drop_text packed;
        cJSON *input = cJSON_CreateObject(), *out = NULL;
        sqlite3_stmt *update = NULL;
        sqlite3_int64 original = sqlite3_column_int64(s, 4);
        const char *message;
        strcpy_s(request, sizeof(request), (const char *)sqlite3_column_text(s, 0));
        packed.data = (unsigned char *)sqlite3_column_blob(s, 2);
        packed.size = (size_t)sqlite3_column_bytes(s, 2);
        packed.compressed = sqlite3_column_int(s, 3);
        packed.original_size = original >= 0 ? (size_t)original : SIZE_MAX;
        if (!input || !drop_text_unpack(&packed, &body) || !cJSON_AddStringToObject(input, "request_id", request) ||
            !cJSON_AddStringToObject(input, "recipient_user_id", (const char *)sqlite3_column_text(s, 1)) || !cJSON_AddStringToObject(input, "body", body))
            error = DROP_E_DATABASE;
        else {
            cJSON *files = cJSON_AddArrayToObject(input, "attachments");
            error = files ? drop_client_uploads(c, request, files) : DROP_E_IO;
            if (error == DROP_OK) error = http(c, L"/api/v1/messages", input, &out);
        }
        free(body);
        cJSON_Delete(input);
        message = drop_json_string(out, "message_id", 32);
        if (error == DROP_OK && !drop_id_valid(message)) error = DROP_E_PROTOCOL;
        if (sqlite3_prepare_v2(c->database, "UPDATE pending SET state=?1,message_id=?2,error=?3 WHERE request_id=?4", -1, &update, NULL) != SQLITE_OK)
            error = DROP_E_DATABASE;
        else {
            bind(update, 1, error == DROP_OK ? "accepted" : "queued");
            if (error == DROP_OK)
                bind(update, 2, message);
            else
                sqlite3_bind_null(update, 2);
            bind(update, 3, error == DROP_OK ? "" : drop_error_name(error));
            bind(update, 4, request);
            if (sqlite3_step(update) != SQLITE_DONE) error = DROP_E_DATABASE;
        }
        sqlite3_finalize(update);
        cJSON_Delete(out);
        if (error != DROP_OK) break;
    }
    if (error == DROP_OK && step != SQLITE_DONE) error = DROP_E_DATABASE;
    sqlite3_finalize(s);
    return error;
}
/* 在同步事务内保存一条正文和附件任务；幂等处理已有消息，失败由外层回滚整页。 */
static drop_error save_message(drop_client *c, const cJSON *message) {
    static const char *fields[] = {"message_id", "request_id", "conversation_id", "sender_user_id", "recipient_user_id", "sender_device_id"};
    const char *ids[6], *body;
    uint64_t created;
    drop_text packed = {0};
    sqlite3_stmt *s = NULL;
    int i;
    drop_error error = DROP_E_PROTOCOL;
    for (i = 0; i < 6; ++i) {
        ids[i] = drop_json_string(message, fields[i], 32);
        if (!drop_id_valid(ids[i])) return DROP_E_PROTOCOL;
    }
    if (strcmp(ids[3], c->user) && strcmp(ids[4], c->user)) return DROP_E_PROTOCOL;
    if (!drop_u64_parse(drop_json_string(message, "created_at", 20), &created) || created > INT64_MAX || !(body = drop_json_string(message, "body", DROP_MAX_BODY)) ||
        !drop_text_pack(body, c->use_zstd, c->zstd_level, &packed))
        return DROP_E_PROTOCOL;
    if (sqlite3_prepare_v2(c->database, "SELECT request_id,conversation_id,sender,recipient,device,created_at,body,compressed,original_length FROM messages WHERE message_id=?1",
            -1, &s, NULL) != SQLITE_OK) {
        error = drop_client_database_error(c);
        goto done;
    }
    bind(s, 1, ids[0]);
    if (sqlite3_step(s) == SQLITE_ROW) {
        drop_text existing;
        char *plain = NULL;
        sqlite3_int64 original = sqlite3_column_int64(s, 8);
        int same;
        for (i = 1; i < 6; ++i)
            if (strcmp(ids[i], (const char *)sqlite3_column_text(s, i - 1))) goto done;
        if (sqlite3_column_int64(s, 5) != (sqlite3_int64)created || original < 0 || original > DROP_MAX_BODY) goto done;
        existing.data = (unsigned char *)sqlite3_column_blob(s, 6);
        existing.size = (size_t)sqlite3_column_bytes(s, 6);
        existing.compressed = sqlite3_column_int(s, 7);
        existing.original_size = (size_t)original;
        same = drop_text_unpack(&existing, &plain) && !strcmp(plain, body);
        free(plain);
        if (!same) goto done;
    }
    sqlite3_finalize(s);
    s = NULL;
    if (sqlite3_prepare_v2(c->database, "INSERT INTO messages VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) ON CONFLICT(message_id) DO NOTHING", -1, &s, NULL) != SQLITE_OK) {
        error = drop_client_database_error(c);
        goto done;
    }
    for (i = 0; i < 6; ++i)
        bind(s, i + 1, ids[i]);
    sqlite3_bind_int64(s, 7, (sqlite3_int64)created);
    sqlite3_bind_blob(s, 8, packed.data, (int)packed.size, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 9, packed.compressed);
    sqlite3_bind_int64(s, 10, (sqlite3_int64)packed.original_size);
    error = sqlite3_step(s) == SQLITE_DONE ? drop_client_register_files(c, message) : drop_client_database_error(c);
    if (error == DROP_OK) error = drop_client_message_order(c, message);
done:
    sqlite3_finalize(s);
    drop_text_free(&packed);
    return error;
}
/* 拉取一页增量记录，在同一事务中保存正文、附件任务及游标；失败不跳过记录，附件下载另行推进。 */
drop_error drop_client_sync(drop_client *c) {
    sqlite3_stmt *s = NULL;
    char cursor[32], generation[DROP_ID_CAP];
    drop_error error = DROP_E_DATABASE;
    int more = 1, pages = 0;
    error = drop_client_reconcile(c);
    if (error != DROP_OK) return error;
    if (sqlite3_prepare_v2(c->database, "SELECT (SELECT value FROM meta WHERE key='cursor'),(SELECT value FROM meta WHERE key='generation')", -1, &s, NULL) != SQLITE_OK ||
        sqlite3_step(s) != SQLITE_ROW)
        goto done;
    strcpy_s(cursor, sizeof(cursor), (const char *)sqlite3_column_text(s, 0));
    strcpy_s(generation, sizeof(generation), (const char *)sqlite3_column_text(s, 1));
    sqlite3_finalize(s);
    s = NULL;
    while (more && ++pages <= 10000) {
        cJSON *input = cJSON_CreateObject(), *out = NULL, *event, *events, *flag;
        const char *next;
        uint64_t expected, received;
        if (!input || !cJSON_AddStringToObject(input, "generation", generation) || !cJSON_AddStringToObject(input, "after", cursor) ||
            !cJSON_AddNumberToObject(input, "limit", 50)) {
            cJSON_Delete(input);
            error = DROP_E_IO;
            goto done;
        }
        error = http(c, L"/api/v1/sync", input, &out);
        cJSON_Delete(input);
        if (error != DROP_OK) {
            cJSON_Delete(out);
            goto done;
        }
        next = drop_json_string(out, "next", 20);
        events = cJSON_GetObjectItemCaseSensitive(out, "events");
        flag = cJSON_GetObjectItemCaseSensitive(out, "more");
        if (!drop_u64_parse(cursor, &expected) || !drop_u64_parse(next, &received) || !cJSON_IsArray(events) || !cJSON_IsBool(flag) ||
            strcmp(drop_json_string(out, "server_id", 32) ? drop_json_string(out, "server_id", 32) : "", c->server) ||
            strcmp(drop_json_string(out, "generation", 32) ? drop_json_string(out, "generation", 32) : "", generation)) {
            cJSON_Delete(out);
            error = DROP_E_PROTOCOL;
            goto done;
        }
        if (!sql(c->database, "BEGIN IMMEDIATE")) {
            cJSON_Delete(out);
            error = DROP_E_DATABASE;
            goto done;
        }
        cJSON_ArrayForEach(event, events) {
            uint64_t seq;
            if (!drop_u64_parse(drop_json_string(event, "seq", 20), &seq) || seq != expected + 1) {
                error = DROP_E_PROTOCOL;
                goto rollback;
            }
            error = save_message(c, cJSON_GetObjectItemCaseSensitive(event, "message"));
            if (error != DROP_OK) goto rollback;
            expected = seq;
        }
        more = cJSON_IsTrue(flag);
        if (expected != received || received > INT64_MAX || (more && !cJSON_GetArraySize(events))) {
            error = DROP_E_PROTOCOL;
            goto rollback;
        }
        if (sqlite3_prepare_v2(c->database, "UPDATE meta SET value=?1 WHERE key='cursor'", -1, &s, NULL) != SQLITE_OK) {
            error = DROP_E_DATABASE;
            goto rollback;
        }
        bind(s, 1, next);
        if (sqlite3_step(s) != SQLITE_DONE) {
            error = DROP_E_DATABASE;
            goto rollback;
        }
        sqlite3_finalize(s);
        s = NULL;
        if (!sql(c->database, "COMMIT")) {
            error = DROP_E_DATABASE;
            goto rollback;
        }
        strcpy_s(cursor, sizeof(cursor), next);
        cJSON_Delete(out);
        if (more && c->sync_page_limit && (unsigned)pages >= c->sync_page_limit) break;
        continue;
    rollback:
        if (error == DROP_E_DATABASE && sqlite3_errcode(c->database) == SQLITE_FULL) error = DROP_E_SPACE;
        sqlite3_finalize(s);
        s = NULL;
        sql(c->database, "ROLLBACK");
        cJSON_Delete(out);
        goto done;
    }
    c->more_bodies = more;
    error = drop_client_profiles(c);
    if (error != DROP_OK) goto done;
    if (more)
        error = c->sync_page_limit ? drop_client_receipts(c, 0) : DROP_E_LIMIT;
    else {
        drop_error receipt = drop_client_receipts(c, 0), download;
        if (receipt == DROP_E_AUTH || receipt == DROP_E_CONFLICT || receipt == DROP_E_PROTOCOL) {
            error = receipt;
            goto done;
        }
        download = drop_client_downloads(c);
        error = drop_client_receipts(c, 1);
        if (download != DROP_OK)
            error = download;
        else if (receipt != DROP_OK)
            error = receipt;
    }
done:
    sqlite3_finalize(s);
    return error;
}
#if DROP_DIAGNOSTICS
/* 在隔离目录验证本机队列、迁移和容量错误处理，保持已有待发送记录可恢复。 */
int drop_client_test(const WCHAR *root) {
    sqlite3 *db = NULL;
    drop_client c = {0};
    char request[DROP_ID_CAP];
    sqlite3_stmt *s = NULL;
    int ok = 0;
    (void)root;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK ||
        !sql(db,
            "CREATE TABLE pending(request_id TEXT PRIMARY KEY,recipient TEXT NOT NULL,body BLOB,compressed INTEGER,original_length INTEGER,state TEXT DEFAULT 'queued',message_id "
            "TEXT,error TEXT DEFAULT '');"))
        goto done;
    c.database = db;
    drop_id_new(c.user);
    if (drop_client_queue(&c, "离线正文与 emoji 🎈", NULL, request) != DROP_OK || !drop_id_valid(request) ||
        sqlite3_prepare_v2(db, "SELECT count(*) FROM pending WHERE state='queued'", -1, &s, NULL) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW || sqlite3_column_int(s, 0) != 1)
        goto done;
    sqlite3_finalize(s);
    s = NULL;
    {
        char *large = malloc(DROP_MAX_BODY + 1);
        drop_error error;
        if (!large || !sql(db, "PRAGMA max_page_count=3")) {
            free(large);
            goto done;
        }
        memset(large, 'x', DROP_MAX_BODY);
        large[DROP_MAX_BODY] = 0;
        error = drop_client_queue(&c, large, NULL, request);
        free(large);
        if (error != DROP_E_SPACE || sqlite3_prepare_v2(db, "SELECT count(*) FROM pending", -1, &s, NULL) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW ||
            sqlite3_column_int(s, 0) != 1)
            goto done;
    }
    ok = 1;
    puts("PASS client: queue schema default-self UTF-8 SQLite-full retains earlier queue");
done:
    sqlite3_finalize(s);
    if (db) sqlite3_close(db);
    return ok;
}
#endif
/* 使用当前凭据连接事件通道，处理变化提示并补拉持久消息；用于诊断客户端。 */
int drop_client_watch(drop_client *c) {
    drop_ws ws;
    unsigned char buffer[4096];
    DWORD count;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    int events = 0, ok = 0;
    drop_error error = drop_ws_open(c->url, c->token, &ws, c->ca_file);
    if (error != DROP_OK) {
        printf("Watch: %s\n", drop_error_name(error));
        return 0;
    }
    while (events < 2) {
        cJSON *object;
        const char *kind;
        if (drop_ws_receive(&ws, buffer, sizeof(buffer), &count, &type, NULL) != NO_ERROR || type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            !(object = drop_json_parse((char *)buffer, count)))
            break;
        kind = drop_json_string(object, "type", 32);
        if (!kind || strcmp(kind, "changed")) {
            cJSON_Delete(object);
            break;
        }
        cJSON_Delete(object);
        if (++events == 1) {
            puts("Watch: ready");
            fflush(stdout);
        } else {
            ok = drop_client_sync(c) == DROP_OK;
            if (ok) puts("Watch: synchronized");
        }
    }
    drop_ws_close(&ws);
    return ok;
}

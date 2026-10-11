/* Google Drive 应用私有空间中的密文备份、持久续传和完整校验，上传内容保持服务器存储格式。 */
#include "cloud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* 提供云盘命令使用的 URL 参数编码入口，结果由调用者 free。 */
char *drop_cloud_encode(const char *text);
/* 复制绑定云盘任务文本，避免语句引用随后释放的响应内容。 */
static void bind(sqlite3_stmt *s, int n, const char *text) {
    sqlite3_bind_text(s, n, text, -1, SQLITE_TRANSIENT);
}
/* 执行云盘状态 SQL，数据库锁和事务由调用者维护。 */
static int exec(drop_server *s, const char *sql) {
    return sqlite3_exec(s->database, sql, NULL, NULL, NULL) == SQLITE_OK;
}
/* 建立云盘备份、文件映射和续传会话结构，持久记录失败及已确认偏移。 */
int drop_cloud_schema(drop_server *server) {
    sqlite3_stmt *s = NULL;
    int version = -1;
    if (sqlite3_prepare_v2(server->database, "PRAGMA user_version", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (version >= 9 && version <= DROP_SCHEMA_VERSION) return 1;
    if (version != 8 || !drop_schema_snapshot(server, 8)) return 0;
    if (exec(server,
            "BEGIN IMMEDIATE; CREATE TABLE cloud_backups(task_id TEXT PRIMARY KEY,directory TEXT NOT NULL,state TEXT NOT NULL DEFAULT 'queued',error TEXT NOT NULL DEFAULT "
            "'',completed_at INTEGER NOT NULL DEFAULT 0);"
            "CREATE TABLE cloud_files(task_id TEXT NOT NULL REFERENCES cloud_backups(task_id),entry TEXT NOT NULL,bytes INTEGER NOT NULL,sha256 TEXT NOT NULL,drive_id TEXT NOT "
            "NULL DEFAULT '',session TEXT NOT NULL DEFAULT '',offset INTEGER NOT NULL DEFAULT 0,state TEXT NOT NULL DEFAULT 'queued',PRIMARY KEY(task_id,entry)); PRAGMA "
            "user_version=9; COMMIT;"))
        return 1;
    exec(server, "ROLLBACK");
    return 0;
}
/* 只接受快照、完成标记及固定对象目录下的清单或块文件名，拒绝路径穿越和未知文件类型。 */
static int entry_valid(const char *entry) {
    size_t n = strlen(entry), i;
    char id[33];
    if (!strcmp(entry, "snapshot.db") || !strcmp(entry, "complete.bin")) return 1;
    if (n < 42 || strncmp(entry, "objects/", 8) || entry[40] != '/') return 0;
    memcpy(id, entry + 8, 32);
    id[32] = 0;
    if (!drop_id_valid(id)) return 0;
    if (!strcmp(entry + 41, "manifest.bin")) return 1;
    if (n != 63 || strcmp(entry + 57, ".chunk")) return 0;
    for (i = 41; i < 57; i++)
        if (!((entry[i] >= '0' && entry[i] <= '9') || (entry[i] >= 'a' && entry[i] <= 'f'))) return 0;
    return 1;
}
/* 检查云盘返回的文件 ID 字符及长度，防止拼接非法请求路径。 */
static int file_id_valid(const char *text) {
    size_t i, n = text ? strlen(text) : 0;
    if (!n || n > 128) return 0;
    for (i = 0; i < n; i++)
        if (!((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z') || (text[i] >= '0' && text[i] <= '9') || text[i] == '_' || text[i] == '-')) return 0;
    return 1;
}
/* 将已校验清单条目映射到备份内固定路径，拒绝逃逸或不允许的文件类型。 */
static int local_path(const WCHAR *root, const char *entry, WCHAR *path) {
    WCHAR name[256];
    size_t i;
    if (!entry_valid(entry) || !drop_to_wide(entry, name, 256)) return 0;
    for (i = 0; name[i]; i++)
        if (name[i] == L'/') name[i] = L'\\';
    return swprintf_s(path, 32768, L"%s\\%s", root, name) >= 0;
}
/* 从二进制备份清单按小端字节序读取 64 位整数，调用者保证缓冲长度已检查。 */
static uint64_t u64(const unsigned char *p) {
    uint64_t n = 0;
    int i;
    for (i = 0; i < 8; i++)
        n |= (uint64_t)p[i] << (8 * i);
    return n;
}
/* 核查本地密文格式并持久登记文件长度及摘要；已有任务必须与当前文件一致，才能继续上传。 */
static int register_file(drop_cloud *cloud, const WCHAR *root, const char *task, const char *entry) {
    WCHAR *path = malloc(32768 * sizeof(WCHAR));
    unsigned char hash[32], header[8];
    char sha[65];
    uint64_t size;
    sqlite3_stmt *s = NULL;
    int ok = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD read;
    if (!path || !local_path(root, entry, path) || (GetFileAttributesW(path) & FILE_ATTRIBUTE_REPARSE_POINT) || !drop_hash_file(path, hash, &size) || size > INT64_MAX) goto done;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !ReadFile(file, header, 8, &read, NULL) || read != 8) goto done;
    if ((strstr(entry, ".chunk") && memcmp(header, "DROPCHK1", 8)) || (strstr(entry, "manifest.bin") && memcmp(header, "DROPOBJ1", 8)) ||
        (!strcmp(entry, "complete.bin") && memcmp(header, "DROPBAK1", 8)) || (!strcmp(entry, "snapshot.db") && !memcmp(header, "SQLite f", 8)))
        goto done;
    drop_hash_hex(hash, sha);
    EnterCriticalSection(&cloud->server->database_lock);
    if (sqlite3_prepare_v2(cloud->server->database, "INSERT INTO cloud_files(task_id,entry,bytes,sha256) VALUES(?1,?2,?3,?4) ON CONFLICT DO NOTHING", -1, &s, NULL) == SQLITE_OK) {
        bind(s, 1, task);
        bind(s, 2, entry);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)size);
        bind(s, 4, sha);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    s = NULL;
    if (ok && sqlite3_prepare_v2(cloud->server->database, "SELECT bytes,sha256 FROM cloud_files WHERE task_id=?1 AND entry=?2", -1, &s, NULL) == SQLITE_OK) {
        bind(s, 1, task);
        bind(s, 2, entry);
        ok = sqlite3_step(s) == SQLITE_ROW && (uint64_t)sqlite3_column_int64(s, 0) == size && !strcmp((const char *)sqlite3_column_text(s, 1), sha);
    } else
        ok = 0;
    sqlite3_finalize(s);
    s = NULL;
    LeaveCriticalSection(&cloud->server->database_lock);
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(path);
    return ok;
}
/* 枚举备份数据库、密文对象及完成清单，建立需镜像的文件集合。 */
static int enumerate(drop_cloud *cloud, const WCHAR *root, const char *task, const char *prefix, unsigned depth, unsigned *count) {
    WCHAR *base = malloc(32768 * sizeof(WCHAR)), *search = malloc(32768 * sizeof(WCHAR)), wide[256];
    HANDLE find = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW item;
    char name[256], entry[256];
    int ok = 0;
    if (!base || !search || depth > 2 || !drop_to_wide(prefix, wide, 256) || swprintf_s(base, 32768, L"%s\\%s", root, wide) < 0 || !drop_path_join(base, L"*", search, 32768))
        goto done;
    find = FindFirstFileW(search, &item);
    if (find == INVALID_HANDLE_VALUE) goto done;
    do {
        if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L"..")) continue;
        if ((item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !drop_to_utf8(item.cFileName, name, sizeof(name)) ||
            sprintf_s(entry, sizeof(entry), "%s%s%s", prefix, *prefix ? "/" : "", name) < 0)
            goto done;
        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if ((depth == 0 && strcmp(name, "objects")) || (depth == 1 && !drop_id_valid(name)) || depth == 2 || !enumerate(cloud, root, task, entry, depth + 1, count)) goto done;
        } else if (++*count > 100000 || !entry_valid(entry) || !register_file(cloud, root, task, entry))
            goto done;
    } while (FindNextFileW(find, &item));
    ok = GetLastError() == ERROR_NO_MORE_FILES;
done:
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
    free(base);
    free(search);
    return ok;
}
/* 一个密文文件的稳定远端身份及续传位置；与加密数据库中的任务记录互相核对后才能继续。 */
typedef struct upload_item {
    char task[33], entry[256], id[129], sha[65];
    WCHAR session[4097];
    uint64_t size, offset;
} upload_item;
/* 更新云盘文件任务的状态、续传地址及偏移，供失败后继续同一任务。 */
static int update(drop_cloud *cloud, const upload_item *item, int complete) {
    sqlite3_stmt *s = NULL;
    char *session = malloc(16389);
    int ok = 0;
    if (!session || !drop_to_utf8(item->session, session, 16389)) goto done;
    EnterCriticalSection(&cloud->server->database_lock);
    if (sqlite3_prepare_v2(cloud->server->database, "UPDATE cloud_files SET drive_id=?1,session=?2,offset=?3,state=?4 WHERE task_id=?5 AND entry=?6", -1, &s, NULL) == SQLITE_OK) {
        bind(s, 1, item->id);
        bind(s, 2, session);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)item->offset);
        bind(s, 4, complete ? "complete" : "uploading");
        bind(s, 5, item->task);
        bind(s, 6, item->entry);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    LeaveCriticalSection(&cloud->server->database_lock);
done:
    if (session) {
        sodium_memzero(session, strlen(session));
        free(session);
    }
    return ok;
}
/* 从续传回应解析服务端已保存位置，检查其范围，不将本地发送量直接当作已确认进度。 */
static int position(const cloud_reply *reply, uint64_t total, uint64_t *offset) {
    char text[256];
    uint64_t end;
    if (!*reply->range) {
        *offset = 0;
        return 1;
    }
    if (!drop_to_utf8(reply->range, text, sizeof(text)) || strncmp(text, "bytes=0-", 8) || !drop_u64_parse(text + 8, &end) || end >= total) return 0;
    *offset = end + 1;
    return *offset <= total;
}
/* 核对远端文件长度及 SHA-256；服务端缺少摘要时回读密文进行校验。 */
static drop_error verify_remote(drop_cloud *cloud, const upload_item *item) {
    WCHAR path[512], url[4097], id[129], headers[256];
    cJSON *out = NULL;
    const char *sha;
    uint64_t size, offset = 0;
    cloud_reply reply = {0};
    drop_error error = DROP_E_ARGUMENT;
    crypto_hash_sha256_state state;
    unsigned char hash[32], expected[32];
    if (!file_id_valid(item->id) || !drop_to_wide(item->id, id, 129) || !drop_hash_decode(item->sha, expected)) return error;
    swprintf_s(path, 512, L"/drive/v3/files/%s?fields=id,size,sha256Checksum", id);
    error = drop_cloud_json(cloud, L"GET", path, NULL, &out);
    if (error != DROP_OK) goto done;
    if (!drop_u64_parse(drop_json_string(out, "size", 20), &size) || size != item->size) {
        error = DROP_E_CONFLICT;
        goto done;
    }
    sha = drop_json_string(out, "sha256Checksum", 64);
    if (sha) {
        error = !strcmp(sha, item->sha) ? DROP_OK : DROP_E_CRYPTO;
        goto done;
    }
    /* Google 返回的 SHA-256 字段可能缺失；没有摘要时回读密文并在本机核验。 */
    crypto_hash_sha256_init(&state);
    swprintf_s(url, 4097, L"%s/drive/v3/files/%s?alt=media", cloud->origin, id);
    while (offset < size) {
        uint64_t amount = size - offset > DROP_CHUNK_SIZE ? DROP_CHUNK_SIZE : size - offset;
        swprintf_s(headers, 256, L"Range: bytes=%llu-%llu\r\n", (unsigned long long)offset, (unsigned long long)(offset + amount - 1));
        error = drop_cloud_http(cloud, L"GET", url, headers, NULL, 0, 1, &reply);
        if (error != DROP_OK) goto done;
        if ((reply.status != 206 && !(reply.status == 200 && !offset && amount == size)) || reply.size != amount) {
            error = DROP_E_PROTOCOL;
            goto done;
        }
        crypto_hash_sha256_update(&state, reply.data, reply.size);
        offset += reply.size;
        drop_cloud_reply_free(&reply);
    }
    crypto_hash_sha256_final(&state, hash);
    error = sodium_memcmp(hash, expected, 32) ? DROP_E_CRYPTO : DROP_OK;
done:
    cJSON_Delete(out);
    drop_cloud_reply_free(&reply);
    return error;
}
/* 创建或恢复续传会话，只发送缺失密文范围；使用固定远端 ID 处理最终响应不确定和会话过期。 */
static drop_error transfer(drop_cloud *cloud, const WCHAR *root, upload_item *item) {
    WCHAR *source = malloc(32768 * sizeof(WCHAR)), url[4097], headers[512], id[129];
    cJSON *out = NULL, *meta = NULL, *properties, *parents, *ids;
    char *body = NULL;
    cloud_reply reply = {0};
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned char *buffer = NULL;
    DWORD got, amount;
    LARGE_INTEGER offset;
    drop_error error = DROP_E_IO;
    int recreate = 0;
    if (!source || !local_path(root, item->entry, source)) goto done;
    if (!*item->id) {
        error = drop_cloud_json(cloud, L"GET", L"/drive/v3/files/generateIds?count=1&space=appDataFolder&type=files", NULL, &out);
        if (error != DROP_OK) goto done;
        ids = cJSON_GetObjectItemCaseSensitive(out, "ids");
        if (!cJSON_IsArray(ids) || cJSON_GetArraySize(ids) != 1 || !cJSON_IsString(cJSON_GetArrayItem(ids, 0)) || !file_id_valid(cJSON_GetArrayItem(ids, 0)->valuestring)) {
            error = DROP_E_PROTOCOL;
            goto done;
        }
        strcpy_s(item->id, sizeof(item->id), cJSON_GetArrayItem(ids, 0)->valuestring);
        cJSON_Delete(out);
        out = NULL;
        if (!update(cloud, item, 0)) {
            error = DROP_E_DATABASE;
            goto done;
        }
    }
    if (!drop_to_wide(item->id, id, 129)) goto done;
query_session:
    if (*item->session) {
        swprintf_s(headers, 512, L"Content-Length: 0\r\nContent-Range: bytes */%llu\r\n", (unsigned long long)item->size);
        error = drop_cloud_http(cloud, L"PUT", item->session, headers, NULL, 0, 1, &reply);
        if (error != DROP_OK) goto done;
        if (reply.status == 200 || reply.status == 201) {
            item->offset = item->size;
            error = verify_remote(cloud, item);
            if (error == DROP_OK && !update(cloud, item, 1)) error = DROP_E_DATABASE;
            goto done;
        }
        if (reply.status == 404 || reply.status == 410) {
            *item->session = 0;
            item->offset = 0;
            if (!update(cloud, item, 0)) {
                error = DROP_E_DATABASE;
                goto done;
            }
        } else if (reply.status != 308 || !position(&reply, item->size, &item->offset)) {
            error = drop_cloud_status(&reply);
            if (error == DROP_OK) error = DROP_E_PROTOCOL;
            goto done;
        }
        drop_cloud_reply_free(&reply);
    }
    if (!*item->session) {
        /* 固定远端 ID 使最终响应丢失或续传会话过期后仍能核对同一文件，避免重复创建。 */
        swprintf_s(url, 4097, L"/drive/v3/files/%s?fields=id,size,appProperties", id);
        error = drop_cloud_json(cloud, L"GET", url, NULL, &out);
        if (error == DROP_OK) {
            uint64_t size;
            const char *sha = drop_json_string(cJSON_GetObjectItemCaseSensitive(out, "appProperties"), "sha256", 64);
            if (!drop_u64_parse(drop_json_string(out, "size", 20), &size) || size != item->size || !sha || strcmp(sha, item->sha)) {
                error = DROP_E_CONFLICT;
                goto done;
            }
            item->offset = size;
            error = verify_remote(cloud, item);
            if (error == DROP_OK && !update(cloud, item, 1)) error = DROP_E_DATABASE;
            goto done;
        }
        if (error != DROP_E_CONFLICT) goto done;
        cJSON_Delete(out);
        out = NULL;
        meta = cJSON_CreateObject();
        properties = cJSON_AddObjectToObject(meta, "appProperties");
        parents = cJSON_AddArrayToObject(meta, "parents");
        if (!meta || !properties || !parents || !cJSON_AddItemToArray(parents, cJSON_CreateString("appDataFolder")) || !cJSON_AddStringToObject(meta, "id", item->id) ||
            !cJSON_AddStringToObject(meta, "name", item->id) || !cJSON_AddStringToObject(meta, "mimeType", "application/octet-stream") ||
            !cJSON_AddStringToObject(properties, "dropBackup", item->task) || !cJSON_AddStringToObject(properties, "entry", item->entry) ||
            !cJSON_AddStringToObject(properties, "sha256", item->sha) || !cJSON_AddStringToObject(properties, "complete", !strcmp(item->entry, "complete.bin") ? "1" : "0") ||
            !(body = cJSON_PrintUnformatted(meta))) {
            error = DROP_E_IO;
            goto done;
        }
        swprintf_s(url, 4097, L"%s/upload/drive/v3/files?uploadType=resumable&fields=id,size", cloud->origin);
        swprintf_s(headers, 512, L"Content-Type: application/json\r\nX-Upload-Content-Type: application/octet-stream\r\nX-Upload-Content-Length: %llu\r\n",
            (unsigned long long)item->size);
        error = drop_cloud_http(cloud, L"POST", url, headers, body, (DWORD)strlen(body), 1, &reply);
        if (error != DROP_OK) goto done;
        if (reply.status == 409 && !recreate++) {
            drop_cloud_reply_free(&reply);
            cJSON_free(body);
            body = NULL;
            cJSON_Delete(meta);
            meta = NULL;
            goto query_session;
        }
        if (reply.status != 200 || !*reply.location) {
            error = drop_cloud_status(&reply);
            if (error == DROP_OK) error = DROP_E_PROTOCOL;
            goto done;
        }
        wcscpy_s(item->session, 4097, reply.location);
        item->offset = 0;
        if (!update(cloud, item, 0)) {
            error = DROP_E_DATABASE;
            goto done;
        }
        drop_cloud_reply_free(&reply);
    }
    file = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    buffer = malloc(DROP_CHUNK_SIZE);
    if (file == INVALID_HANDLE_VALUE || !buffer) {
        error = DROP_E_IO;
        goto done;
    }
    while (item->offset < item->size) {
        uint64_t remaining = item->size - item->offset;
        amount = (DWORD)(remaining > DROP_CHUNK_SIZE ? DROP_CHUNK_SIZE : remaining);
        offset.QuadPart = (LONGLONG)item->offset;
        if (!SetFilePointerEx(file, offset, NULL, FILE_BEGIN) || !ReadFile(file, buffer, amount, &got, NULL) || got != amount) {
            error = drop_io_error();
            goto done;
        }
        swprintf_s(headers, 512, L"Content-Type: application/octet-stream\r\nContent-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)item->offset,
            (unsigned long long)(item->offset + amount - 1), (unsigned long long)item->size);
        error = drop_cloud_http(cloud, L"PUT", item->session, headers, buffer, amount, 1, &reply);
        if (error != DROP_OK) goto done;
        if (reply.status == 200 || reply.status == 201) {
            if (item->offset + amount != item->size) {
                error = DROP_E_PROTOCOL;
                goto done;
            }
            item->offset = item->size;
        } else if (reply.status == 308) {
            uint64_t next;
            if (!position(&reply, item->size, &next) || next <= item->offset || next > item->offset + amount || (next % 262144 && next != item->size)) {
                error = DROP_E_PROTOCOL;
                goto done;
            }
            item->offset = next;
        } else {
            error = drop_cloud_status(&reply);
            goto done;
        }
        if (item->offset == item->size) {
            error = verify_remote(cloud, item);
            if (error != DROP_OK) goto done;
        }
        if (!update(cloud, item, item->offset == item->size)) {
            error = DROP_E_DATABASE;
            goto done;
        }
        drop_cloud_reply_free(&reply);
    }
    error = DROP_OK;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(buffer);
    free(source);
    cJSON_free(body);
    cJSON_Delete(meta);
    cJSON_Delete(out);
    drop_cloud_reply_free(&reply);
    return error;
}
/* 验证本地完成备份并上传其密文文件，完整核查后发布远端完成标记；持久状态支持中断续作。 */
drop_error drop_cloud_upload(drop_cloud *cloud, const WCHAR *backup) {
    unsigned char *manifest = NULL, payload[128], hash[32];
    DWORD size;
    unsigned long long plain_size;
    uint64_t bytes;
    WCHAR *path = malloc(32768 * sizeof(WCHAR));
    char task[33], *directory = malloc(131072);
    sqlite3_stmt *s = NULL;
    upload_item *item = calloc(1, sizeof(*item));
    unsigned count = 0;
    drop_error error = DROP_E_CRYPTO;
    int step = SQLITE_DONE;
    if (!path || !directory || !item || !drop_to_utf8(backup, directory, 131072) || !drop_path_join(backup, L"complete.bin", path, 32768) ||
        !drop_file_read(path, &manifest, &size, 176) || size != 176 || memcmp(manifest, "DROPBAK1", 8) ||
        crypto_aead_xchacha20poly1305_ietf_decrypt(payload, &plain_size, NULL, manifest + 32, 144, manifest, 8, manifest + 8, cloud->server->keys.object_master_key) ||
        plain_size != 128 || memcmp(payload, "DROPBM01", 8) || u64(payload + 8) < 7 || u64(payload + 8) > DROP_SCHEMA_VERSION ||
        memcmp(payload + 16, cloud->server->keys.server_id, 32))
        goto done;
    memcpy(task, payload + 48, 32);
    task[32] = 0;
    if (!drop_id_valid(task) || !drop_path_join(backup, L"snapshot.db", path, 32768) || !drop_hash_file(path, hash, &bytes) || sodium_memcmp(hash, payload + 80, 32)) goto done;
    EnterCriticalSection(&cloud->server->database_lock);
    error = DROP_E_DATABASE;
    if (sqlite3_prepare_v2(cloud->server->database, "INSERT INTO cloud_backups(task_id,directory) VALUES(?1,?2) ON CONFLICT DO NOTHING", -1, &s, NULL) == SQLITE_OK) {
        bind(s, 1, task);
        bind(s, 2, directory);
        if (sqlite3_step(s) == SQLITE_DONE) error = DROP_OK;
    }
    sqlite3_finalize(s);
    s = NULL;
    LeaveCriticalSection(&cloud->server->database_lock);
    if (error != DROP_OK) goto done;
    if (!enumerate(cloud, backup, task, "", 0, &count)) {
        error = DROP_E_CONFLICT;
        goto done;
    }
    for (;;) {
        EnterCriticalSection(&cloud->server->database_lock);
        if (sqlite3_prepare_v2(cloud->server->database,
                "SELECT entry,bytes,sha256,drive_id,session,offset FROM cloud_files WHERE task_id=?1 AND state!='complete' ORDER BY (entry='complete.bin'),entry LIMIT 1", -1, &s,
                NULL) != SQLITE_OK)
            error = DROP_E_DATABASE;
        else {
            bind(s, 1, task);
            step = sqlite3_step(s);
            if (step == SQLITE_ROW) {
                memset(item, 0, sizeof(*item));
                strcpy_s(item->task, 33, task);
                strcpy_s(item->entry, 256, (const char *)sqlite3_column_text(s, 0));
                item->size = (uint64_t)sqlite3_column_int64(s, 1);
                strcpy_s(item->sha, 65, (const char *)sqlite3_column_text(s, 2));
                strcpy_s(item->id, 129, (const char *)sqlite3_column_text(s, 3));
                if (!drop_to_wide((const char *)sqlite3_column_text(s, 4), item->session, 4097)) error = DROP_E_DATABASE;
                item->offset = (uint64_t)sqlite3_column_int64(s, 5);
            } else if (step != SQLITE_DONE)
                error = DROP_E_DATABASE;
        }
        sqlite3_finalize(s);
        s = NULL;
        LeaveCriticalSection(&cloud->server->database_lock);
        if (error != DROP_OK || step == SQLITE_DONE) break;
        error = transfer(cloud, backup, item);
        if (error != DROP_OK) break;
    }
    EnterCriticalSection(&cloud->server->database_lock);
    if (sqlite3_prepare_v2(cloud->server->database, "UPDATE cloud_backups SET state=?1,error=?2,completed_at=?3 WHERE task_id=?4", -1, &s, NULL) == SQLITE_OK) {
        bind(s, 1, error == DROP_OK ? "complete" : "retry");
        bind(s, 2, drop_error_name(error));
        sqlite3_bind_int64(s, 3, error == DROP_OK ? (sqlite3_int64)time(NULL) : 0);
        bind(s, 4, task);
        if (sqlite3_step(s) != SQLITE_DONE) error = DROP_E_DATABASE;
    } else
        error = DROP_E_DATABASE;
    sqlite3_finalize(s);
    s = NULL;
    LeaveCriticalSection(&cloud->server->database_lock);
done:
    free(manifest);
    free(path);
    free(directory);
    if (item) {
        sodium_memzero(item, sizeof(*item));
        free(item);
    }
    return error;
}
/* 分页查询应用私有云盘空间，可按任务 ID 和完成标记过滤；成功数组由调用者 cJSON_Delete。 */
drop_error drop_cloud_list(drop_cloud *cloud, const char *task, int complete_only, cJSON **files) {
    char query[512], *encoded = NULL, *page = NULL;
    WCHAR path[4097], wide[2049];
    cJSON *out = NULL, *entries, *entry;
    drop_error error = DROP_E_IO;
    unsigned total = 0;
    *files = cJSON_CreateArray();
    if (!*files || (task && !drop_id_valid(task))) return DROP_E_ARGUMENT;
    if (task)
        sprintf_s(query, sizeof(query), "trashed=false and appProperties has { key='dropBackup' and value='%s' }%s", task,
            complete_only ? " and appProperties has { key='complete' and value='1' }" : "");
    else
        strcpy_s(query, sizeof(query), "trashed=false and appProperties has { key='complete' and value='1' }");
    encoded = drop_cloud_encode(query);
    if (!encoded || !drop_to_wide(encoded, wide, 2049)) goto done;
    do {
        WCHAR page_wide[2049] = L"";
        if (page && !drop_to_wide(page, page_wide, 2049)) {
            error = DROP_E_PROTOCOL;
            goto done;
        }
        if (swprintf_s(path, 4097, L"/drive/v3/files?spaces=appDataFolder&pageSize=50&fields=nextPageToken,files(id,size,appProperties)&q=%s%s%s", wide,
                page ? L"&pageToken=" : L"", page_wide) < 0)
            goto done;
        error = drop_cloud_json(cloud, L"GET", path, NULL, &out);
        if (error != DROP_OK) goto done;
        entries = cJSON_GetObjectItemCaseSensitive(out, "files");
        if (!cJSON_IsArray(entries) || cJSON_GetArraySize(entries) > 50) {
            error = DROP_E_PROTOCOL;
            goto done;
        }
        cJSON_ArrayForEach(entry, entries) {
            if (++total > 100000 || !cJSON_AddItemToArray(*files, cJSON_Duplicate(entry, 1))) {
                error = DROP_E_LIMIT;
                goto done;
            }
        }
        free(page);
        page = NULL;
        {
            const char *next = drop_json_string(out, "nextPageToken", 512);
            if (next) {
                if (!*next) {
                    error = DROP_E_PROTOCOL;
                    goto done;
                }
                page = drop_cloud_encode(next);
                if (!page) {
                    error = DROP_E_IO;
                    goto done;
                }
            }
        }
        cJSON_Delete(out);
        out = NULL;
    } while (page);
    error = DROP_OK;
done:
    free(page);
    free(encoded);
    cJSON_Delete(out);
    if (error != DROP_OK) {
        cJSON_Delete(*files);
        *files = NULL;
    }
    return error;
}
/* 为备份内部文件创建受限父目录，保持下载目标和清单路径边界一致。 */
static int parents(const WCHAR *target, const char *entry) {
    WCHAR *path = malloc(32768 * sizeof(WCHAR)), name[64];
    int ok = 0;
    if (!path) return 0;
    if (strncmp(entry, "objects/", 8)) {
        ok = 1;
        goto done;
    }
    if (!drop_path_join(target, L"objects", path, 32768) || !drop_private_directory(path)) goto done;
    {
        char id[33];
        memcpy(id, entry + 8, 32);
        id[32] = 0;
        if (!drop_to_wide(id, name, 64) || swprintf_s(path, 32768, L"%s\\objects\\%s", target, name) < 0 || !drop_private_directory(path)) goto done;
    }
    ok = 1;
done:
    free(path);
    return ok;
}
/* 下载一个云盘密文文件并核对长度及摘要，失败不将该文件视为已完成。 */
static drop_error download_file(drop_cloud *cloud, const WCHAR *target, const cJSON *entry) {
    const char *id = drop_json_string(entry, "id", 128), *name, *sha;
    const cJSON *properties = cJSON_GetObjectItemCaseSensitive(entry, "appProperties");
    uint64_t total, offset = 0, existing;
    unsigned char hash[32], expected[32];
    WCHAR *path = malloc(32768 * sizeof(WCHAR)), *partial = malloc(32768 * sizeof(WCHAR)), url[4097], headers[256], wide[129];
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER length;
    cloud_reply reply = {0};
    drop_error error = DROP_E_PROTOCOL;
    DWORD written;
    name = drop_json_string(properties, "entry", 255);
    sha = drop_json_string(properties, "sha256", 64);
    if (!path || !partial || !file_id_valid(id) || !name || !entry_valid(name) || !drop_hash_decode(sha, expected) ||
        !drop_u64_parse(drop_json_string(entry, "size", 20), &total) || total > UINT64_C(107374182400) || !drop_to_wide(id, wide, 129) || !parents(target, name) ||
        !local_path(target, name, path) || swprintf_s(partial, 32768, L"%s.part", path) < 0)
        goto done;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        error = drop_hash_file(path, hash, &existing) && existing == total && !sodium_memcmp(hash, expected, 32) ? DROP_OK : DROP_E_CRYPTO;
        goto done;
    }
    file = CreateFileW(partial, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &length) || length.QuadPart < 0 || (uint64_t)length.QuadPart > total) {
        error = drop_io_error();
        goto done;
    }
    {
        BY_HANDLE_FILE_INFORMATION information;
        if (!GetFileInformationByHandle(file, &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = DROP_E_ARGUMENT;
            goto done;
        }
    }
    offset = (uint64_t)length.QuadPart;
    if (!SetFilePointerEx(file, length, NULL, FILE_BEGIN)) {
        error = drop_io_error();
        goto done;
    }
    swprintf_s(url, 4097, L"%s/drive/v3/files/%s?alt=media", cloud->origin, wide);
    while (offset < total) {
        uint64_t remaining = total - offset, amount = remaining > DROP_CHUNK_SIZE ? DROP_CHUNK_SIZE : remaining;
        swprintf_s(headers, 256, L"Range: bytes=%llu-%llu\r\n", (unsigned long long)offset, (unsigned long long)(offset + amount - 1));
        error = drop_cloud_http(cloud, L"GET", url, headers, NULL, 0, 1, &reply);
        if (error != DROP_OK) goto done;
        if ((reply.status != 206 && !(reply.status == 200 && offset == 0 && amount == total)) || reply.size != amount) {
            error = drop_cloud_status(&reply);
            if (error == DROP_OK) error = DROP_E_PROTOCOL;
            goto done;
        }
        if (!WriteFile(file, reply.data, reply.size, &written, NULL) || written != reply.size || !FlushFileBuffers(file)) {
            error = drop_io_error();
            goto done;
        }
        offset += reply.size;
        drop_cloud_reply_free(&reply);
    }
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (!drop_hash_file(partial, hash, &existing) || existing != total || sodium_memcmp(hash, expected, 32)) {
        error = DROP_E_CRYPTO;
        goto done;
    }
    error = MoveFileExW(partial, path, MOVEFILE_WRITE_THROUGH) ? DROP_OK : drop_io_error();
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(path);
    free(partial);
    drop_cloud_reply_free(&reply);
    return error;
}
/* 读取已发布的远端备份并逐项校验下载，全部成功后才接受完成清单；不在云端或下载时解密。 */
drop_error drop_cloud_download(drop_cloud *cloud, const char *task, const WCHAR *target) {
    cJSON *files = NULL, *entry, *complete = NULL;
    drop_error error;
    unsigned count = 0;
    WCHAR marker[32768];
    if (!drop_id_valid(task)) return DROP_E_ARGUMENT;
    error = drop_cloud_list(cloud, task, 0, &files);
    if (error != DROP_OK) goto done;
    cJSON_ArrayForEach(entry, files) {
        const cJSON *properties = cJSON_GetObjectItemCaseSensitive(entry, "appProperties");
        const char *name = drop_json_string(properties, "entry", 255), *backup = drop_json_string(properties, "dropBackup", 32),
                   *published = drop_json_string(properties, "complete", 1);
        cJSON *prior;
        if (!backup || strcmp(backup, task) || !name || !entry_valid(name)) {
            error = DROP_E_PROTOCOL;
            goto done;
        }
        for (prior = files->child; prior != entry; prior = prior->next) {
            const char *previous = drop_json_string(cJSON_GetObjectItemCaseSensitive(prior, "appProperties"), "entry", 255);
            if (previous && !strcmp(previous, name)) {
                error = DROP_E_CONFLICT;
                goto done;
            }
        }
        if (!strcmp(name, "complete.bin")) {
            if (!published || strcmp(published, "1")) {
                error = DROP_E_PROTOCOL;
                goto done;
            }
            complete = entry;
        }
        count++;
    }
    if (!complete || count < 2) {
        error = DROP_E_CONFLICT;
        goto done;
    }
    if (!drop_private_directory(target) || !drop_path_join(target, L"download.incomplete", marker, 32768) || !drop_file_write(marker, "DROP", 4, 1)) {
        error = drop_io_error();
        goto done;
    }
    cJSON_ArrayForEach(entry, files) if (entry != complete) {
        error = download_file(cloud, target, entry);
        if (error != DROP_OK) goto done;
    }
    error = download_file(cloud, target, complete);
    if (error == DROP_OK && !DeleteFileW(marker)) error = drop_io_error();
done:
    cJSON_Delete(files);
    return error;
}
/* 显式删除指定远端备份，先移除完成标记，并记录本机 deleted 状态以避免后台重新镜像。 */
drop_error drop_cloud_delete(drop_cloud *cloud, const char *task) {
    cJSON *files = NULL, *entry;
    WCHAR path[512], wide[129];
    drop_error error;
    if (!drop_id_valid(task)) return DROP_E_ARGUMENT;
    error = drop_cloud_list(cloud, task, 0, &files);
    if (error != DROP_OK) goto done;
    /* 先删除远端完成标记，避免删除中断后仍把残缺备份显示为可恢复。 */
    {
        int pass;
        for (pass = 0; pass < 2; pass++)
            cJSON_ArrayForEach(entry, files) {
                const char *id = drop_json_string(entry, "id", 128), *name = drop_json_string(cJSON_GetObjectItemCaseSensitive(entry, "appProperties"), "entry", 255);
                if (!name || !file_id_valid(id)) {
                    error = DROP_E_PROTOCOL;
                    goto done;
                }
                if ((!strcmp(name, "complete.bin")) != (pass == 0)) continue;
                if (!drop_to_wide(id, wide, 129) || swprintf_s(path, 512, L"/drive/v3/files/%s", wide) < 0) {
                    error = DROP_E_ARGUMENT;
                    goto done;
                }
                error = drop_cloud_json(cloud, L"DELETE", path, NULL, NULL);
                if (error != DROP_OK && error != DROP_E_CONFLICT) goto done;
            }
    }
    /* 显式删除还须持久记录本机任务状态，阻止后台再次镜像同一备份。 */
    {
        sqlite3_stmt *s = NULL;
        EnterCriticalSection(&cloud->server->database_lock);
        if (sqlite3_prepare_v2(cloud->server->database, "UPDATE cloud_backups SET state='deleted',error='' WHERE task_id=?1", -1, &s, NULL) == SQLITE_OK) {
            bind(s, 1, task);
            sqlite3_step(s);
        }
        sqlite3_finalize(s);
        LeaveCriticalSection(&cloud->server->database_lock);
    }
    error = DROP_OK;
done:
    cJSON_Delete(files);
    return error;
}

/* 客户端稳定附件副本、分块上传和自动完整下载；网络和进程中断依靠持久任务续作。 */
#include "client.h"
#include "object.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_FILE_SIZE UINT64_C(10737418240)

/* 执行附件队列 SQL，成功返回 1，失败交由外层事务或任务状态处理。 */
static int sql(sqlite3 *db, const char *text) {
    return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK;
}
/* 复制绑定附件元数据文本，语句不保留外部缓冲引用。 */
static void bind(sqlite3_stmt *s, int i, const char *text) {
    sqlite3_bind_text(s, i, text, -1, SQLITE_TRANSIENT);
}
/* 把附件接口的 HTTP 状态映射为项目错误，保留待处理和权限等区别。 */
static drop_error status_error(DWORD status) {
    return status == 200 ? DROP_OK
        : status == 401  ? DROP_E_AUTH
        : status == 409  ? DROP_E_CONFLICT
        : status == 413  ? DROP_E_LIMIT
        : status == 400  ? DROP_E_ARGUMENT
        : status == 507  ? DROP_E_SPACE
                         : DROP_E_DATABASE;
}
/* 将 64 位长度或偏移写成十进制 JSON 字符串，避免接收端以浮点数解析时丢失精度。 */
static int number(cJSON *o, const char *key, uint64_t value) {
    char text[32];
    sprintf_s(text, sizeof(text), "%llu", (unsigned long long)value);
    return cJSON_AddStringToObject(o, key, text) != NULL;
}
/* 使用当前客户端凭据和 CA 请求附件 JSON 接口，并检查业务状态。 */
static drop_error json(drop_client *c, const WCHAR *method, const WCHAR *path, cJSON *input, cJSON **out) {
    DWORD status;
    drop_error error = drop_http_json_ca(c->url, method, path, c->token, input, out, &status, c->ca_file);
    return error == DROP_OK ? status_error(status) : error;
}
/* 将已校验的随机对象 ID 转为本机附件路径，原始文件名不参与内部路径构造。 */
static int file_path(drop_client *c, const char *id, const WCHAR *extension, WCHAR *path) {
    WCHAR name[80], wide[DROP_ID_CAP];
    return drop_id_valid(id) && drop_to_wide(id, wide, DROP_ID_CAP) && swprintf_s(name, 80, L"%s.%s", wide, extension) >= 0 && drop_path_join(c->files, name, path, 32768);
}
/* 建立或迁移本机附件上传、下载和消息关联结构，保留持久续传进度。 */
int drop_client_files_schema(drop_client *c) {
    sqlite3_stmt *s = NULL;
    int version = -1;
    if (sqlite3_prepare_v2(c->database, "PRAGMA user_version", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (version >= 2 && version <= 5) return 1;
    if (version != 1) return 0;
    if (!sql(c->database,
            "BEGIN IMMEDIATE; CREATE TABLE pending_files(local_id TEXT PRIMARY KEY,request_id TEXT NOT NULL REFERENCES pending(request_id),"
            "object_id TEXT,filename TEXT NOT NULL,total INTEGER NOT NULL,sha256 TEXT NOT NULL);"
            "CREATE TABLE downloads(object_id TEXT PRIMARY KEY,message_id TEXT NOT NULL REFERENCES messages(message_id),filename TEXT NOT NULL,"
            "total INTEGER NOT NULL,sha256 TEXT NOT NULL,state TEXT NOT NULL DEFAULT 'queued',saved_bytes INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT '');"
            "PRAGMA user_version=2; COMMIT;")) {
        sql(c->database, "ROLLBACK");
        return 0;
    }
    return 1;
}
/* 禁止源文件写入和删除后复制到管理目录，同时计算长度与摘要；失败清理未完成副本，禁止用硬链接代替。 */
static int stable_copy(drop_client *c, const WCHAR *source, const char *local, char name[1025], uint64_t *size, char sha[65]) {
    HANDLE input = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE;
    WCHAR temporary[32768], final[32768];
    LARGE_INTEGER length;
    unsigned char *buffer = NULL, hash[32];
    crypto_hash_sha256_state state;
    DWORD read, written;
    uint64_t copied = 0;
    int ok = 0;
    const WCHAR *base = wcsrchr(source, L'\\'), *slash = wcsrchr(source, L'/');
    if (slash && (!base || slash > base)) base = slash;
    base = base ? base + 1 : source;
    if (!drop_to_utf8(base, name, 1025) || !*name || !file_path(c, local, L"copying", temporary) || !file_path(c, local, L"source", final)) return 0;
    /* 复制期间共享模式禁止写入和删除；队列保存独立副本，不能仅保留源路径或硬链接。 */
    input = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (input == INVALID_HANDLE_VALUE || !GetFileSizeEx(input, &length) || length.QuadPart < 0 || (uint64_t)length.QuadPart > MAX_FILE_SIZE) goto done;
    output = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) goto done;
    buffer = malloc(DROP_CHUNK_SIZE);
    if (!buffer) goto done;
    crypto_hash_sha256_init(&state);
    for (;;) {
        if (!ReadFile(input, buffer, DROP_CHUNK_SIZE, &read, NULL)) goto done;
        if (!read) break;
        if (!WriteFile(output, buffer, read, &written, NULL) || written != read) goto done;
        crypto_hash_sha256_update(&state, buffer, read);
        copied += read;
    }
    if (copied != (uint64_t)length.QuadPart || !FlushFileBuffers(output)) goto done;
    CloseHandle(output);
    output = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temporary, final, MOVEFILE_WRITE_THROUGH)) goto done;
    crypto_hash_sha256_final(&state, hash);
    drop_hash_hex(hash, sha);
    *size = copied;
    ok = 1;
done:
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (buffer) {
        sodium_memzero(buffer, DROP_CHUNK_SIZE);
        free(buffer);
    }
    if (!ok) DeleteFileW(temporary);
    return ok;
}
/* 先创建正文及附件的稳定本机副本，再以事务记录发送任务；失败不将未保存附件标记为成功。 */
drop_error drop_client_queue_files(drop_client *c, const char *body, const char *recipient, const WCHAR *const *paths, int count, char request[DROP_ID_CAP]) {
    char ids[DROP_MAX_ATTACHMENTS][DROP_ID_CAP], names[DROP_MAX_ATTACHMENTS][1025], hashes[DROP_MAX_ATTACHMENTS][65];
    uint64_t sizes[DROP_MAX_ATTACHMENTS];
    sqlite3_stmt *s = NULL;
    drop_text packed = {0};
    drop_error error = DROP_E_IO;
    int i, copied = 0;
    if (count < 1 || count > DROP_MAX_ATTACHMENTS || !body || (recipient && !drop_id_valid(recipient)) || !drop_text_pack(body, c->use_zstd, c->zstd_level, &packed))
        return DROP_E_ARGUMENT;
    for (i = 0; i < count; ++i) {
        drop_id_new(ids[i]);
        if (!stable_copy(c, paths[i], ids[i], names[i], &sizes[i], hashes[i])) goto done;
        ++copied;
    }
    drop_id_new(request);
    error = DROP_E_DATABASE;
    if (!sql(c->database, "BEGIN IMMEDIATE") ||
        sqlite3_prepare_v2(c->database, "INSERT INTO pending(request_id,recipient,body,compressed,original_length) VALUES(?1,?2,?3,?4,?5)", -1, &s, NULL) != SQLITE_OK)
        goto rollback;
    bind(s, 1, request);
    bind(s, 2, recipient ? recipient : c->user);
    sqlite3_bind_blob(s, 3, packed.data, (int)packed.size, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 4, packed.compressed);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)packed.original_size);
    if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
    sqlite3_finalize(s);
    s = NULL;
    if (sqlite3_prepare_v2(c->database, "INSERT INTO pending_files(local_id,request_id,filename,total,sha256) VALUES(?1,?2,?3,?4,?5)", -1, &s, NULL) != SQLITE_OK) goto rollback;
    for (i = 0; i < count; ++i) {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        bind(s, 1, ids[i]);
        bind(s, 2, request);
        bind(s, 3, names[i]);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)sizes[i]);
        bind(s, 5, hashes[i]);
        if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
    }
    if (!sql(c->database, "COMMIT")) goto rollback;
    error = DROP_OK;
    goto done;
rollback:
    sql(c->database, "ROLLBACK");
done:
    sqlite3_finalize(s);
    drop_text_free(&packed);
    if (error != DROP_OK)
        for (i = 0; i < copied; ++i) {
            WCHAR path[32768];
            if (file_path(c, ids[i], L"source", path)) DeleteFileW(path);
        }
    return error;
}
/* 查询或创建上传对象，只发送服务端缺失块并申请完整校验；持久保存 ID 和进度以支持中断恢复。 */
static drop_error upload_one(drop_client *c, const char *local, char object[DROP_ID_CAP], const char *name, uint64_t total, const char *sha) {
    cJSON *input = NULL, *out = NULL;
    WCHAR path[160], source[32768], final[32768];
    unsigned char *buffer = NULL, *bitmap = NULL, expected[32], hash[32];
    HANDLE file = INVALID_HANDLE_VALUE;
    crypto_hash_sha256_state hash_state;
    uint64_t chunks = total ? 1 + (total - 1) / DROP_CHUNK_SIZE : 0, index;
    DWORD got, status, length;
    unsigned char *reply = NULL;
    drop_error error = DROP_E_IO;
    size_t bitmap_bytes = (size_t)((chunks + 7) / 8), decoded;
    const char *bits, *state;
    sqlite3_stmt *s = NULL;
    uint64_t server_total, server_chunks;
    int recreated = 0;
create_upload:
    if (!*object) {
        input = cJSON_CreateObject();
        if (!input || !cJSON_AddStringToObject(input, "request_id", local) || !cJSON_AddStringToObject(input, "filename", name) || !number(input, "total_size", total) ||
            !cJSON_AddStringToObject(input, "sha256", sha))
            goto done;
        error = json(c, L"POST", L"/api/v1/uploads", input, &out);
        if (error != DROP_OK) goto done;
        if (!drop_id_valid(drop_json_string(out, "object_id", 32))) {
            error = DROP_E_PROTOCOL;
            goto done;
        }
        strcpy_s(object, DROP_ID_CAP, drop_json_string(out, "object_id", 32));
        cJSON_Delete(input);
        input = NULL;
        cJSON_Delete(out);
        out = NULL;
        if (sqlite3_prepare_v2(c->database, "UPDATE pending_files SET object_id=?1 WHERE local_id=?2", -1, &s, NULL) != SQLITE_OK) {
            error = DROP_E_DATABASE;
            goto done;
        }
        bind(s, 1, object);
        bind(s, 2, local);
        if (sqlite3_step(s) != SQLITE_DONE) {
            error = DROP_E_DATABASE;
            goto done;
        }
        sqlite3_finalize(s);
        s = NULL;
    }
    {
        WCHAR wide[DROP_ID_CAP];
        if (!drop_to_wide(object, wide, DROP_ID_CAP) || swprintf_s(path, 160, L"/api/v1/uploads/%s", wide) < 0) goto done;
    }
    error = json(c, L"GET", path, NULL, &out);
    if (error == DROP_E_AUTH && !recreated) {
        uint64_t size;
        if (!file_path(c, local, L"source", source) || !file_path(c, object, L"file", final) || !drop_hash_decode(sha, expected)) goto done;
        if (GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(final) != INVALID_FILE_ATTRIBUTES) {
            if (!drop_hash_file(final, hash, &size) || size != total || sodium_memcmp(expected, hash, 32)) {
                error = DROP_E_CRYPTO;
                goto done;
            }
            if (!CopyFileW(final, source, TRUE)) {
                error = drop_io_error();
                goto done;
            }
        }
        cJSON_Delete(out);
        out = NULL;
        *object = 0;
        recreated = 1;
        goto create_upload;
    }
    if (error != DROP_OK) goto done;
    bits = drop_json_string(out, "received_bitmap", 3000);
    state = drop_json_string(out, "state", 16);
    if (!bits || !state || (strcmp(state, "uploading") && strcmp(state, "validating") && strcmp(state, "complete")) || strlen(bits) != bitmap_bytes * 2 ||
        !drop_u64_parse(drop_json_string(out, "total_size", 20), &server_total) || server_total != total ||
        !drop_u64_parse(drop_json_string(out, "chunk_count", 20), &server_chunks) || server_chunks != chunks || !drop_json_string(out, "object_id", 32) ||
        strcmp(drop_json_string(out, "object_id", 32), object) || !drop_json_string(out, "sha256", 64) || strcmp(drop_json_string(out, "sha256", 64), sha) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(out, "chunk_size")) || cJSON_GetObjectItemCaseSensitive(out, "chunk_size")->valuedouble != DROP_CHUNK_SIZE ||
        !drop_hash_decode(sha, expected)) {
        error = DROP_E_PROTOCOL;
        goto done;
    }
    if (!strcmp(state, "validating")) {
        error = DROP_E_PENDING;
        goto done;
    }
    {
        const char *failure = drop_json_string(out, "finalize_error", 32);
        if (failure && *failure && strcmp(failure, "interrupted") && strcmp(failure, "network_error")) {
            error = !strcmp(failure, "space_unavailable") ? DROP_E_SPACE : !strcmp(failure, "database_error") ? DROP_E_DATABASE : DROP_E_CRYPTO;
            goto done;
        }
    }
    /* 服务端对象完成后不再在每次查询时重复读取多 GiB 的本机副本。 */
    if (!strcmp(state, "complete")) {
        if (!file_path(c, local, L"source", source) || !file_path(c, object, L"file", final)) {
            error = DROP_E_IO;
            goto done;
        }
        goto publish_local;
    }
    bitmap = malloc(bitmap_bytes ? bitmap_bytes : 1);
    buffer = malloc(DROP_CHUNK_SIZE);
    if (!bitmap || !buffer || sodium_hex2bin(bitmap, bitmap_bytes, bits, strlen(bits), NULL, &decoded, NULL) != 0 || decoded != bitmap_bytes ||
        !file_path(c, local, L"source", source) || !file_path(c, object, L"file", final)) {
        error = DROP_E_IO;
        goto done;
    }
    file = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) file = CreateFileW(final, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        error = DROP_E_IO;
        goto done;
    }
    {
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || (uint64_t)size.QuadPart != total) {
            error = DROP_E_CRYPTO;
            goto done;
        }
    }
    crypto_hash_sha256_init(&hash_state);
    for (index = 0; index < chunks; ++index) {
        /* 已上传块由服务器持久记录；交互到达时让出线程，下轮核对位图后安全续传。 */
        if (c->work_pending && WaitForSingleObject(c->work_pending, 0) == WAIT_OBJECT_0) {
            error = DROP_E_PENDING;
            goto done;
        }
        uint64_t remaining = total - index * DROP_CHUNK_SIZE;
        DWORD amount = (DWORD)(remaining > DROP_CHUNK_SIZE ? DROP_CHUNK_SIZE : remaining);
        if (!ReadFile(file, buffer, amount, &got, NULL) || got != amount) {
            error = DROP_E_IO;
            goto done;
        }
        crypto_hash_sha256_update(&hash_state, buffer, amount);
        if (!(bitmap[(size_t)index / 8] & (1u << (index % 8)))) {
            WCHAR wide[DROP_ID_CAP];
            drop_to_wide(object, wide, DROP_ID_CAP);
            swprintf_s(path, 160, L"/api/v1/uploads/%s/chunks/%llu", wide, (unsigned long long)index);
            error = drop_http_bytes(c->url, L"PUT", path, c->token, buffer, amount, 1, 0, &reply, &length, &status, c->ca_file);
            if (reply) {
                free(reply);
                reply = NULL;
            }
            if (error != DROP_OK) goto done;
            error = status_error(status);
            if (error != DROP_OK) goto done;
        }
    }
    crypto_hash_sha256_final(&hash_state, hash);
    if (sodium_memcmp(expected, hash, 32)) {
        error = DROP_E_CRYPTO;
        goto done;
    }
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    cJSON_Delete(out);
    out = NULL;
    {
        WCHAR wide[DROP_ID_CAP];
        drop_to_wide(object, wide, DROP_ID_CAP);
        swprintf_s(path, 160, L"/api/v1/uploads/%s/complete", wide);
    }
    error = json(c, L"POST", path, NULL, &out);
    if (error != DROP_OK) goto done;
    state = drop_json_string(out, "state", 16);
    if (!state || (strcmp(state, "complete") && strcmp(state, "validating") && strcmp(state, "uploading"))) {
        error = DROP_E_PROTOCOL;
        goto done;
    }
    if (strcmp(state, "complete")) {
        error = DROP_E_PENDING;
        goto done;
    }
publish_local:
    if (GetFileAttributesW(source) != INVALID_FILE_ATTRIBUTES && !MoveFileExW(source, final, MOVEFILE_WRITE_THROUGH)) {
        uint64_t existing_size;
        if (!drop_hash_file(final, hash, &existing_size) || existing_size != total || sodium_memcmp(hash, expected, 32)) {
            error = DROP_E_IO;
            goto done;
        }
        DeleteFileW(source);
    }
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    sqlite3_finalize(s);
    free(bitmap);
    free(reply);
    if (buffer) {
        sodium_memzero(buffer, DROP_CHUNK_SIZE);
        free(buffer);
    }
    cJSON_Delete(input);
    cJSON_Delete(out);
    return error;
}
/* 按发送任务顺序推进全部附件上传，并构造提交消息所需的附件对象列表。 */
drop_error drop_client_uploads(drop_client *c, const char *request, cJSON *array) {
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_OK;
    int step;
    if (sqlite3_prepare_v2(c->database, "SELECT local_id,object_id,filename,total,sha256 FROM pending_files WHERE request_id=?1 ORDER BY rowid", -1, &s, NULL) != SQLITE_OK)
        return DROP_E_DATABASE;
    bind(s, 1, request);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        char local[DROP_ID_CAP], object[DROP_ID_CAP] = {0}, name[1025], sha[65];
        sqlite3_int64 total = sqlite3_column_int64(s, 3);
        if (total < 0 || (uint64_t)total > MAX_FILE_SIZE) {
            error = DROP_E_DATABASE;
            break;
        }
        strcpy_s(local, sizeof(local), (const char *)sqlite3_column_text(s, 0));
        strcpy_s(name, sizeof(name), (const char *)sqlite3_column_text(s, 2));
        strcpy_s(sha, sizeof(sha), (const char *)sqlite3_column_text(s, 4));
        if (sqlite3_column_type(s, 1) != SQLITE_NULL) strcpy_s(object, sizeof(object), (const char *)sqlite3_column_text(s, 1));
        error = upload_one(c, local, object, name, (uint64_t)total, sha);
        if (error != DROP_OK) break;
        if (!cJSON_AddItemToArray(array, cJSON_CreateString(object))) {
            error = DROP_E_IO;
            break;
        }
    }
    if (error == DROP_OK && step != SQLITE_DONE) error = DROP_E_DATABASE;
    sqlite3_finalize(s);
    return error;
}
/* 从已校验消息登记自动下载任务；在正文同步事务内调用，确保游标与任务一起提交。 */
drop_error drop_client_register_files(drop_client *c, const cJSON *message) {
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(message, "attachments"), *item;
    const char *message_id = drop_json_string(message, "message_id", 32);
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_PROTOCOL;
    if (!array || !cJSON_IsArray(array) || cJSON_GetArraySize(array) > DROP_MAX_ATTACHMENTS) return DROP_E_PROTOCOL;
    if (sqlite3_prepare_v2(c->database,
            "INSERT INTO downloads(object_id,message_id,filename,total,sha256) VALUES(?1,?2,?3,?4,?5) ON CONFLICT(object_id) DO UPDATE SET state='queued' WHERE "
            "downloads.state='restore_review'",
            -1, &s, NULL) != SQLITE_OK)
        return drop_client_database_error(c);
    cJSON_ArrayForEach(item, array) {
        const char *id = drop_json_string(item, "object_id", 32), *name = drop_json_string(item, "filename", 1024), *sha = drop_json_string(item, "sha256", 64);
        uint64_t total;
        if (!drop_id_valid(id) || !name || !drop_hash_valid(sha) || !drop_u64_parse(drop_json_string(item, "total_size", 20), &total) || total > MAX_FILE_SIZE) goto done;
        {
            sqlite3_stmt *existing = NULL;
            int step, valid = 1;
            if (sqlite3_prepare_v2(c->database, "SELECT message_id,filename,total,sha256 FROM downloads WHERE object_id=?1", -1, &existing, NULL) != SQLITE_OK) {
                error = drop_client_database_error(c);
                goto done;
            }
            bind(existing, 1, id);
            step = sqlite3_step(existing);
            if (step == SQLITE_ROW)
                valid = !strcmp(message_id, (const char *)sqlite3_column_text(existing, 0)) && !strcmp(name, (const char *)sqlite3_column_text(existing, 1)) &&
                    total == (uint64_t)sqlite3_column_int64(existing, 2) && !strcmp(sha, (const char *)sqlite3_column_text(existing, 3));
            else
                valid = step == SQLITE_DONE;
            sqlite3_finalize(existing);
            if (!valid) goto done;
        }
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        bind(s, 1, id);
        bind(s, 2, message_id);
        bind(s, 3, name);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)total);
        bind(s, 5, sha);
        if (sqlite3_step(s) != SQLITE_DONE) {
            error = drop_client_database_error(c);
            goto done;
        }
    }
    error = DROP_OK;
done:
    sqlite3_finalize(s);
    return error;
}
/* 持久记录已落盘的下载位置和任务状态，供后续从 .part 续传。 */
static int progress(drop_client *c, const char *id, const char *state, uint64_t bytes, drop_error error) {
    sqlite3_stmt *s = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(c->database, "UPDATE downloads SET state=?1,saved_bytes=?2,error=?3 WHERE object_id=?4", -1, &s, NULL) != SQLITE_OK) return 0;
    bind(s, 1, state);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)bytes);
    bind(s, 3, error == DROP_OK ? "" : drop_error_name(error));
    bind(s, 4, id);
    ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}
/* 按块续传到临时文件，完成长度和 SHA-256 校验后发布正式文件；失败保留可恢复进度，不确认完整保存。 */
static drop_error download_one(drop_client *c, const char *id, uint64_t total, const char *sha) {
    WCHAR final[32768], part[32768], path[160], wide[DROP_ID_CAP];
    unsigned char expected[32], hash[32], *bytes = NULL;
    uint64_t actual = 0, position = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER length, offset;
    drop_error error = DROP_E_IO;
    DWORD got, status, written;
    if (!file_path(c, id, L"file", final) || !file_path(c, id, L"part", part) || !drop_hash_decode(sha, expected) || !drop_to_wide(id, wide, DROP_ID_CAP)) return DROP_E_ARGUMENT;
    if (drop_hash_file(final, hash, &actual) && actual == total && !sodium_memcmp(hash, expected, 32))
        return progress(c, id, "complete", total, DROP_OK) ? DROP_OK : DROP_E_DATABASE;
    if (GetFileAttributesW(final) != INVALID_FILE_ATTRIBUTES) {
        WCHAR preserved[32768], suffix[96], random_wide[DROP_ID_CAP];
        char random[DROP_ID_CAP];
        drop_id_new(random);
        if (!drop_to_wide(random, random_wide, DROP_ID_CAP) || swprintf_s(suffix, 96, L"corrupt.%s", random_wide) < 0 || !file_path(c, id, suffix, preserved) ||
            !MoveFileExW(final, preserved, MOVEFILE_WRITE_THROUGH))
            return drop_io_error();
    }
    if (drop_hash_file(part, hash, &actual) && actual == total && !sodium_memcmp(hash, expected, 32)) {
        if (!MoveFileExW(part, final, MOVEFILE_WRITE_THROUGH)) return DROP_E_IO;
        return progress(c, id, "complete", total, DROP_OK) ? DROP_OK : DROP_E_DATABASE;
    }
    file = CreateFileW(part, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0) goto done;
    actual = (uint64_t)length.QuadPart;
    position = actual < total ? (actual / DROP_CHUNK_SIZE) * DROP_CHUNK_SIZE : 0;
    offset.QuadPart = (LONGLONG)position;
    if (!SetFilePointerEx(file, offset, NULL, FILE_BEGIN) || !SetEndOfFile(file)) goto done;
    while (position < total) {
        uint64_t remaining = total - position;
        DWORD expected_length = (DWORD)(remaining > DROP_CHUNK_SIZE ? DROP_CHUNK_SIZE : remaining);
        swprintf_s(path, 160, L"/api/v1/attachments/%s/chunks/%llu", wide, (unsigned long long)(position / DROP_CHUNK_SIZE));
        error = drop_http_bytes(c->url, L"GET", path, c->token, NULL, 0, 0, 1, &bytes, &got, &status, c->ca_file);
        if (error != DROP_OK) goto done;
        error = status_error(status);
        if (error != DROP_OK) goto done;
        if (got != expected_length) {
            error = DROP_E_PROTOCOL;
            goto done;
        }
        c->downloaded_bytes += got;
        if (!WriteFile(file, bytes, got, &written, NULL) || written != got || !FlushFileBuffers(file)) {
            error = drop_io_error();
            goto done;
        }
        free(bytes);
        bytes = NULL;
        position += got;
        if (!progress(c, id, "partial", position, DROP_OK)) {
            error = DROP_E_DATABASE;
            goto done;
        }
        if (position < total &&
            ((c->download_quantum && c->downloaded_bytes >= c->download_quantum) || (c->work_pending && WaitForSingleObject(c->work_pending, 0) == WAIT_OBJECT_0))) {
            error = DROP_OK;
            goto done;
        }
    }
    if (!FlushFileBuffers(file)) {
        error = drop_io_error();
        goto done;
    }
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (!drop_hash_file(part, hash, &actual) || actual != total || sodium_memcmp(hash, expected, 32)) {
        error = DROP_E_CRYPTO;
        goto done;
    }
    if (!MoveFileExW(part, final, MOVEFILE_WRITE_THROUGH)) {
        error = DROP_E_IO;
        goto done;
    }
    error = progress(c, id, "complete", total, DROP_OK) ? DROP_OK : DROP_E_DATABASE;
done:
    free(bytes);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (error != DROP_OK) progress(c, id, "partial", position, error);
    return error;
}
/* 按设定工作量推进待下载附件，记录是否仍有任务，避免一次循环长期占用同步线程。 */
drop_error drop_client_downloads(drop_client *c) {
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_OK;
    int step;
    if (sqlite3_prepare_v2(c->database, "SELECT object_id,total,sha256 FROM downloads WHERE state NOT IN('complete','restore_review') ORDER BY rowid", -1, &s, NULL) != SQLITE_OK)
        return DROP_E_DATABASE;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        char id[DROP_ID_CAP], sha[65];
        sqlite3_int64 total = sqlite3_column_int64(s, 1);
        if ((c->download_quantum && c->downloaded_bytes >= c->download_quantum) || (c->work_pending && WaitForSingleObject(c->work_pending, 0) == WAIT_OBJECT_0)) {
            step = SQLITE_DONE;
            break;
        }
        if (total < 0 || (uint64_t)total > MAX_FILE_SIZE) {
            error = DROP_E_DATABASE;
            break;
        }
        strcpy_s(id, sizeof(id), (const char *)sqlite3_column_text(s, 0));
        strcpy_s(sha, sizeof(sha), (const char *)sqlite3_column_text(s, 2));
        {
            drop_error current = download_one(c, id, (uint64_t)total, sha);
            if (error == DROP_OK && current != DROP_OK) error = current;
        }
    }
    if (error == DROP_OK && step != SQLITE_DONE) error = DROP_E_DATABASE;
    sqlite3_finalize(s);
    s = NULL;
    if (sqlite3_prepare_v2(c->database, "SELECT count(*) FROM downloads WHERE state NOT IN('complete','restore_review')", -1, &s, NULL) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_ROW)
        c->more_downloads = sqlite3_column_int(s, 0) > 0;
    else if (error == DROP_OK)
        error = DROP_E_DATABASE;
    sqlite3_finalize(s);
    return error;
}
/* 重新核查完成缓存，缺失或损坏时恢复下载任务，避免错误地继续报告附件已保存。 */
int drop_client_verify_files(drop_client *c) {
    sqlite3_stmt *s = NULL, *update = NULL;
    int step, ok = 0;
    if (sqlite3_prepare_v2(c->database, "SELECT object_id,total,sha256 FROM downloads WHERE state='complete'", -1, &s, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(c->database, "UPDATE downloads SET state='queued',saved_bytes=0,saved_ack=0,error='crypto_error' WHERE object_id=?1", -1, &update, NULL) != SQLITE_OK)
        goto done;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(s, 0);
        WCHAR path[32768];
        unsigned char hash[32], expected[32];
        uint64_t size;
        sqlite3_int64 total = sqlite3_column_int64(s, 1);
        if (total < 0 || !file_path(c, id, L"file", path) || !drop_hash_decode((const char *)sqlite3_column_text(s, 2), expected)) goto done;
        if (drop_hash_file(path, hash, &size) && size == (uint64_t)total && !sodium_memcmp(hash, expected, 32)) continue;
        sqlite3_reset(update);
        bind(update, 1, id);
        if (sqlite3_step(update) != SQLITE_DONE) goto done;
    }
    ok = step == SQLITE_DONE;
done:
    sqlite3_finalize(s);
    sqlite3_finalize(update);
    return ok;
}

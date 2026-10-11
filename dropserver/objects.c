/* 服务器附件对象和分块接口；权限先行，临时上传也只保存认证密文。 */
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define MAX_FILE_SIZE UINT64_C(10737418240)

/* 执行对象元数据 SQL，成功返回 1，由上层维护事务和并发锁。 */
static int sql(sqlite3 *db, const char *text) {
    return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK;
}
/* 复制绑定对象和账号文本，防止查询引用随后释放的请求数据。 */
static void bind(sqlite3_stmt *s, int i, const char *text) {
    sqlite3_bind_text(s, i, text, -1, SQLITE_TRANSIENT);
}
/* 将 64 位对象长度或块数量写成十进制 JSON 字符串，避免浮点数精度损失。 */
static int number(cJSON *o, const char *key, uint64_t value) {
    char text[32];
    sprintf_s(text, sizeof(text), "%llu", (unsigned long long)value);
    return cJSON_AddStringToObject(o, key, text) != NULL;
}
/* 使用合法随机对象 ID 构造服务器对象目录，原始文件名不参与内部存储路径。 */
static int object_path(drop_server *server, const char *id, const WCHAR *file, WCHAR *path, int create) {
    WCHAR root[32768], directory[32768], wide[DROP_ID_CAP];
    if (!drop_id_valid(id) || !drop_to_wide(id, wide, DROP_ID_CAP) || !drop_path_join(server->directory, L"objects", root, 32768) || !drop_path_join(root, wide, directory, 32768))
        return 0;
    if (create && (!drop_private_directory(root) || !drop_private_directory(directory))) return 0;
    return drop_path_join(directory, file, path, 32768);
}
/* 构造已验证对象的分块密文路径，块编号使用 64 位表示。 */
static int chunk_path(drop_server *server, const char *id, uint64_t index, WCHAR *path) {
    WCHAR name[64];
    if (swprintf_s(name, 64, L"%016llx.chunk", (unsigned long long)index) < 0) return 0;
    return object_path(server, id, name, path, 0);
}
/* 建立或迁移加密对象、分块进度和消息附件关系表，保持已有上传任务可恢复。 */
int drop_objects_schema(drop_server *server) {
    sqlite3_stmt *s = NULL;
    int version = -1;
    if (sqlite3_prepare_v2(server->database, "PRAGMA user_version", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (version >= 3 && version <= DROP_SCHEMA_VERSION) return 1;
    if (version != 2 || !drop_schema_snapshot(server, 2)) return 0;
    if (!sql(server->database,
            "BEGIN IMMEDIATE;"
            "CREATE TABLE objects(object_id TEXT PRIMARY KEY,owner TEXT NOT NULL REFERENCES accounts(user_id),request_id TEXT NOT NULL,"
            "filename TEXT NOT NULL,total INTEGER NOT NULL,sha256 TEXT NOT NULL,header BLOB NOT NULL,state TEXT NOT NULL,created_at INTEGER NOT NULL,UNIQUE(owner,request_id));"
            "CREATE TABLE chunks(object_id TEXT NOT NULL REFERENCES objects(object_id),idx INTEGER NOT NULL,sha256 BLOB NOT NULL,PRIMARY KEY(object_id,idx));"
            "CREATE TABLE attachments(message_id TEXT NOT NULL REFERENCES messages(message_id),position INTEGER NOT NULL,object_id TEXT NOT NULL UNIQUE REFERENCES "
            "objects(object_id),PRIMARY KEY(message_id,position));"
            "PRAGMA user_version=3; COMMIT;")) {
        sql(server->database, "ROLLBACK");
        return 0;
    }
    return 1;
}
/* 在数据库和文件约束下读取对象记录及受保护对象头，校验身份和完整性后交付密钥状态。 */
static drop_error load(drop_server *server, const char *user, const char *id, int attachment, drop_object *object, char state[16]) {
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_AUTH;
    const char *query = attachment ? "SELECT o.header,o.state FROM objects o JOIN attachments a USING(object_id) JOIN messages m USING(message_id) WHERE o.object_id=?1 AND "
                                     "(m.sender=?2 OR m.recipient=?2) AND o.state='complete'"
                                   : "SELECT header,state FROM objects WHERE object_id=?1 AND owner=?2";
    if (!drop_id_valid(id)) return DROP_E_ARGUMENT;
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, query, -1, &s, NULL) != SQLITE_OK) {
        error = DROP_E_DATABASE;
        goto done;
    }
    bind(s, 1, id);
    bind(s, 2, user);
    if (sqlite3_step(s) != SQLITE_ROW) goto done;
    if (!drop_object_open(server->keys.object_master_key, sqlite3_column_blob(s, 0), (size_t)sqlite3_column_bytes(s, 0), object) || strcmp(object->id, id) ||
        object->size > MAX_FILE_SIZE) {
        error = DROP_E_CRYPTO;
        goto done;
    }
    if (strcpy_s(state, 16, (const char *)sqlite3_column_text(s, 1))) {
        error = DROP_E_DATABASE;
        goto done;
    }
    error = DROP_OK;
done:
    if (error == DROP_OK && !attachment) {
        sqlite3_finalize(s);
        s = NULL;
        if (sqlite3_prepare_v2(server->database, "UPDATE objects SET last_activity=?1 WHERE object_id=?2 AND last_activity<?1-60", -1, &s, NULL) != SQLITE_OK)
            error = DROP_E_DATABASE;
        else {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)time(NULL));
            bind(s, 2, id);
            if (sqlite3_step(s) != SQLITE_DONE) error = DROP_E_DATABASE;
        }
    }
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    return error;
}
/* 向已有响应对象添加附件 ID、长度、块大小、摘要及状态；已保存块和校验错误由外层接口另行添加。 */
static int info(cJSON *out, const drop_object *object, const char *state) {
    char sha[65];
    drop_hash_hex(object->sha256, sha);
    return cJSON_AddStringToObject(out, "object_id", object->id) && number(out, "total_size", object->size) && number(out, "chunk_count", object->chunks) &&
        cJSON_AddNumberToObject(out, "chunk_size", DROP_CHUNK_SIZE) && cJSON_AddStringToObject(out, "sha256", sha) && cJSON_AddStringToObject(out, "state", state);
}
/* 校验附件描述和账号容量，创建独立加密对象及持久上传任务；失败不提供可引用对象。 */
drop_error drop_upload_create(drop_server *server, const char *user, const cJSON *input, cJSON **output) {
    const char *request = drop_json_string(input, "request_id", 32), *name = drop_json_string(input, "filename", 1024), *sha = drop_json_string(input, "sha256", 64);
    uint64_t total;
    unsigned char hash[32];
    char id[DROP_ID_CAP], state[16];
    drop_object object = {0};
    sqlite3_stmt *s = NULL;
    WCHAR path[32768];
    drop_error error = DROP_E_DATABASE;
    if (!drop_id_valid(request) || !name || !*name || !drop_hash_decode(sha, hash) || !drop_u64_parse(drop_json_string(input, "total_size", 20), &total) || total > MAX_FILE_SIZE)
        return DROP_E_ARGUMENT;
    EnterCriticalSection(&server->objects_lock);
    EnterCriticalSection(&server->database_lock);
    if (!sql(server->database, "BEGIN IMMEDIATE")) goto done;
    if (sqlite3_prepare_v2(server->database, "SELECT object_id,filename,total,sha256 FROM objects WHERE owner=?1 AND request_id=?2", -1, &s, NULL) != SQLITE_OK) goto rollback;
    bind(s, 1, user);
    bind(s, 2, request);
    if (sqlite3_step(s) == SQLITE_ROW) {
        if (strcmp(name, (const char *)sqlite3_column_text(s, 1)) || total != (uint64_t)sqlite3_column_int64(s, 2) || strcmp(sha, (const char *)sqlite3_column_text(s, 3))) {
            error = DROP_E_CONFLICT;
            goto rollback;
        }
        strcpy_s(id, sizeof(id), (const char *)sqlite3_column_text(s, 0));
        sqlite3_finalize(s);
        s = NULL;
        if (!sql(server->database, "COMMIT")) goto rollback;
        error = load(server, user, id, 0, &object, state);
        goto respond;
    }
    sqlite3_finalize(s);
    s = NULL;
    error = drop_account_capacity(server, user, total + 1024);
    if (error != DROP_OK) goto rollback;
    error = DROP_E_DATABASE;
    sqlite3_finalize(s);
    s = NULL;
    drop_id_new(id);
    if (!drop_object_create(server->keys.object_master_key, id, total, hash, &object) || !object_path(server, id, L"manifest.bin", path, 1) ||
        !drop_file_write(path, object.header, sizeof(object.header), 0)) {
        error = drop_io_error();
        goto rollback;
    }
    if (sqlite3_prepare_v2(server->database,
            "INSERT INTO objects(object_id,owner,request_id,filename,total,sha256,header,state,created_at,last_activity) VALUES(?1,?2,?3,?4,?5,?6,?7,'uploading',?8,?8)", -1, &s,
            NULL) != SQLITE_OK)
        goto rollback;
    bind(s, 1, id);
    bind(s, 2, user);
    bind(s, 3, request);
    bind(s, 4, name);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)total);
    bind(s, 6, sha);
    sqlite3_bind_blob(s, 7, object.header, sizeof(object.header), SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 8, (sqlite3_int64)time(NULL));
    if (sqlite3_step(s) != SQLITE_DONE || !sql(server->database, "COMMIT")) goto rollback;
    strcpy_s(state, sizeof(state), "uploading");
    error = DROP_OK;
respond:
    if (error == DROP_OK) {
        *output = cJSON_CreateObject();
        if (!*output || !info(*output, &object, state)) error = DROP_E_IO;
    }
    goto done;
rollback:
    sql(server->database, "ROLLBACK");
done:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    LeaveCriticalSection(&server->objects_lock);
    sodium_memzero(&object, sizeof(object));
    if (error != DROP_OK) {
        cJSON_Delete(*output);
        *output = NULL;
    }
    return error;
}
/* 检查对象归属并返回持久上传进度，供客户端恢复缺失分块及等待完整校验。 */
drop_error drop_upload_status(drop_server *server, const char *user, const char *id, cJSON **output) {
    drop_object object = {0};
    char state[16];
    sqlite3_stmt *s = NULL;
    unsigned char *bits = NULL;
    char *hex = NULL;
    size_t bytes;
    drop_error error;
    int step;
    EnterCriticalSection(&server->objects_lock);
    error = load(server, user, id, 0, &object, state);
    if (error != DROP_OK) goto done;
    bytes = (size_t)((object.chunks + 7) / 8);
    bits = calloc(bytes ? bytes : 1, 1);
    hex = malloc(bytes * 2 + 1);
    if (!bits || !hex) {
        error = DROP_E_IO;
        goto done;
    }
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, "SELECT idx FROM chunks WHERE object_id=?1", -1, &s, NULL) != SQLITE_OK) {
        error = DROP_E_DATABASE;
        goto unlock;
    }
    bind(s, 1, id);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        sqlite3_int64 i = sqlite3_column_int64(s, 0);
        if (i < 0 || (uint64_t)i >= object.chunks) {
            error = DROP_E_DATABASE;
            goto unlock;
        }
        bits[(size_t)i / 8] |= (unsigned char)(1u << (i % 8));
    }
    if (step != SQLITE_DONE) {
        error = DROP_E_DATABASE;
        goto unlock;
    }
    sodium_bin2hex(hex, bytes * 2 + 1, bits, bytes);
    *output = cJSON_CreateObject();
    if (!*output || !info(*output, &object, state) || !cJSON_AddStringToObject(*output, "received_bitmap", hex)) error = DROP_E_IO;
    sqlite3_finalize(s);
    s = NULL;
    if (error == DROP_OK) {
        if (sqlite3_prepare_v2(server->database, "SELECT finalize_error FROM objects WHERE object_id=?1", -1, &s, NULL) != SQLITE_OK)
            error = DROP_E_DATABASE;
        else {
            bind(s, 1, id);
            if (sqlite3_step(s) != SQLITE_ROW || !cJSON_AddStringToObject(*output, "finalize_error", (const char *)sqlite3_column_text(s, 0))) error = DROP_E_DATABASE;
        }
    }
unlock:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
done:
    free(bits);
    free(hex);
    sodium_memzero(&object, sizeof(object));
    LeaveCriticalSection(&server->objects_lock);
    if (error != DROP_OK) {
        cJSON_Delete(*output);
        *output = NULL;
    }
    return error;
}
/* 核对归属、块编号和长度后在内存中加密并落盘；重复块必须内容一致，进度持久保存。 */
drop_error drop_upload_chunk(drop_server *server, const char *user, const char *id, uint64_t index, const unsigned char *data, DWORD length) {
    drop_object object = {0};
    char state[16];
    sqlite3_stmt *s = NULL;
    unsigned char hash[32], *encrypted = NULL, *existing = NULL, *plain = NULL;
    DWORD encrypted_length = 0, existing_length = 0, plain_length = 0;
    WCHAR path[32768];
    drop_error error;
    int exists = 0;
    EnterCriticalSection(&server->objects_lock);
    error = load(server, user, id, 0, &object, state);
    if (error != DROP_OK) goto done;
    if (!length || length != drop_object_chunk_length(&object, index) || !chunk_path(server, id, index, path)) {
        error = DROP_E_ARGUMENT;
        goto done;
    }
    crypto_hash_sha256(hash, data, length);
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, "SELECT sha256 FROM chunks WHERE object_id=?1 AND idx=?2", -1, &s, NULL) != SQLITE_OK)
        error = DROP_E_DATABASE;
    else {
        bind(s, 1, id);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)index);
        if (sqlite3_step(s) == SQLITE_ROW) {
            exists = 1;
            if (sqlite3_column_bytes(s, 0) != 32 || sodium_memcmp(hash, sqlite3_column_blob(s, 0), 32)) error = DROP_E_CONFLICT;
        }
    }
    sqlite3_finalize(s);
    s = NULL;
    LeaveCriticalSection(&server->database_lock);
    if (error != DROP_OK) goto done;
    if (exists && drop_file_read(path, &existing, &existing_length, DROP_CHUNK_SIZE + DROP_CHUNK_OVERHEAD) &&
        drop_chunk_decrypt(&object, index, existing, existing_length, &plain, &plain_length) && plain_length == length && !sodium_memcmp(plain, data, length))
        goto done;
    if (strcmp(state, "uploading")) {
        error = DROP_E_CRYPTO;
        goto done;
    }
    if (!drop_chunk_encrypt(&object, index, data, length, &encrypted, &encrypted_length) || !drop_file_write(path, encrypted, encrypted_length, 1)) {
        error = DROP_E_IO;
        goto done;
    }
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, "INSERT INTO chunks VALUES(?1,?2,?3) ON CONFLICT(object_id,idx) DO NOTHING", -1, &s, NULL) != SQLITE_OK)
        error = DROP_E_DATABASE;
    else {
        bind(s, 1, id);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)index);
        sqlite3_bind_blob(s, 3, hash, 32, SQLITE_TRANSIENT);
        if (sqlite3_step(s) != SQLITE_DONE) error = DROP_E_DATABASE;
    }
    sqlite3_finalize(s);
    s = NULL;
    LeaveCriticalSection(&server->database_lock);
done:
    free(encrypted);
    free(existing);
    if (plain) {
        sodium_memzero(plain, plain_length);
        free(plain);
    }
    sodium_memzero(&object, sizeof(object));
    LeaveCriticalSection(&server->objects_lock);
    return error;
}
/* 读取并核验对象完整清单，使下载与完成校验使用已认证的对象摘要。 */
static int manifest_read(drop_server *server, const drop_object *object) {
    WCHAR path[32768];
    unsigned char *data = NULL;
    DWORD size;
    int ok;
    ok = object_path(server, object->id, L"manifest.bin", path, 0) && drop_file_read(path, &data, &size, DROP_OBJECT_HEADER_SIZE) && size == DROP_OBJECT_HEADER_SIZE &&
        !sodium_memcmp(data, object->header, size);
    free(data);
    return ok;
}
/* 验证全部密文块、明文摘要及完整清单后发布 complete；后台模式响应退出，失败不允许消息引用。 */
drop_error drop_upload_finish_verify(drop_server *server, const char *user, const char *id, cJSON **output, int asynchronous) {
    drop_object object = {0};
    char state[16];
    WCHAR path[32768];
    unsigned char plain_hash[32], cipher_hash[32];
    crypto_hash_sha256_state plain_state, cipher_state;
    sqlite3_stmt *s = NULL;
    uint64_t index;
    drop_error error;
    int locked = 1;
    EnterCriticalSection(&server->objects_lock);
    error = load(server, user, id, 0, &object, state);
    if (error != DROP_OK) goto done;
    if (!strcmp(state, "complete")) {
        if (!manifest_read(server, &object)) error = DROP_E_CRYPTO;
        goto respond;
    }
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, "SELECT count(*) FROM chunks WHERE object_id=?1", -1, &s, NULL) != SQLITE_OK)
        error = DROP_E_DATABASE;
    else {
        bind(s, 1, id);
        if (sqlite3_step(s) != SQLITE_ROW || (uint64_t)sqlite3_column_int64(s, 0) != object.chunks) error = DROP_E_CONFLICT;
    }
    sqlite3_finalize(s);
    s = NULL;
    LeaveCriticalSection(&server->database_lock);
    if (error != DROP_OK) goto done;
    if (asynchronous) {
        LeaveCriticalSection(&server->objects_lock);
        locked = 0;
    }
    crypto_hash_sha256_init(&plain_state);
    crypto_hash_sha256_init(&cipher_state);
    for (index = 0; index < object.chunks; ++index) {
        unsigned char *encrypted = NULL, *plain = NULL, digest[32];
        DWORD length = 0, plain_length = 0;
        if (WaitForSingleObject(server->stop_event, 0) == WAIT_OBJECT_0) {
            error = DROP_E_NETWORK;
            goto done;
        }
        if (!chunk_path(server, id, index, path) || !drop_file_read(path, &encrypted, &length, DROP_CHUNK_SIZE + DROP_CHUNK_OVERHEAD) ||
            !drop_chunk_decrypt(&object, index, encrypted, length, &plain, &plain_length)) {
            free(encrypted);
            error = DROP_E_CRYPTO;
            goto done;
        }
        crypto_hash_sha256_update(&plain_state, plain, plain_length);
        crypto_hash_sha256(digest, encrypted, length);
        crypto_hash_sha256_update(&cipher_state, digest, 32);
        sodium_memzero(plain, plain_length);
        free(plain);
        free(encrypted);
    }
    crypto_hash_sha256_final(&plain_state, plain_hash);
    crypto_hash_sha256_final(&cipher_state, cipher_hash);
    if (sodium_memcmp(plain_hash, object.sha256, 32)) {
        error = DROP_E_CONFLICT;
        goto done;
    }
    if (!locked) {
        EnterCriticalSection(&server->objects_lock);
        locked = 1;
    }
    if (!drop_object_seal(&object, cipher_hash) || !object_path(server, id, L"manifest.bin", path, 0) || !drop_file_write(path, object.header, sizeof(object.header), 1)) {
        error = DROP_E_IO;
        goto done;
    }
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, "UPDATE objects SET header=?1,state='complete' WHERE object_id=?2 AND owner=?3", -1, &s, NULL) != SQLITE_OK)
        error = DROP_E_DATABASE;
    else {
        sqlite3_bind_blob(s, 1, object.header, sizeof(object.header), SQLITE_TRANSIENT);
        bind(s, 2, id);
        bind(s, 3, user);
        if (sqlite3_step(s) != SQLITE_DONE) error = DROP_E_DATABASE;
    }
    sqlite3_finalize(s);
    s = NULL;
    LeaveCriticalSection(&server->database_lock);
    strcpy_s(state, sizeof(state), "complete");
respond:
    if (error == DROP_OK) {
        *output = cJSON_CreateObject();
        if (!*output || !info(*output, &object, state)) error = DROP_E_IO;
    }
done:
    sodium_memzero(&object, sizeof(object));
    if (locked) LeaveCriticalSection(&server->objects_lock);
    if (error != DROP_OK) {
        cJSON_Delete(*output);
        *output = NULL;
    }
    return error;
}
/* 检查消息参与权限和对象完成状态，认证并解密指定块；成功明文由调用者清零和 free。 */
drop_error drop_attachment_chunk(drop_server *server, const char *user, const char *id, uint64_t index, unsigned char **data, DWORD *length) {
    drop_object object = {0};
    char state[16];
    WCHAR path[32768];
    unsigned char *encrypted = NULL;
    DWORD size = 0;
    drop_error error;
    *data = NULL;
    *length = 0;
    EnterCriticalSection(&server->objects_lock);
    error = load(server, user, id, 1, &object, state);
    if (error != DROP_OK) goto done;
    if (index >= object.chunks) {
        error = DROP_E_ARGUMENT;
        goto done;
    }
    if (!manifest_read(server, &object)) {
        error = DROP_E_CRYPTO;
        goto done;
    }
    if (!chunk_path(server, id, index, path) || !drop_file_read(path, &encrypted, &size, DROP_CHUNK_SIZE + DROP_CHUNK_OVERHEAD) ||
        !drop_chunk_decrypt(&object, index, encrypted, size, data, length))
        error = DROP_E_CRYPTO;
    if (error == DROP_OK) {
        unsigned char hash[32];
        sqlite3_stmt *s = NULL;
        crypto_hash_sha256(hash, *data, *length);
        EnterCriticalSection(&server->database_lock);
        if (sqlite3_prepare_v2(server->database, "SELECT sha256 FROM chunks WHERE object_id=?1 AND idx=?2", -1, &s, NULL) != SQLITE_OK)
            error = DROP_E_DATABASE;
        else {
            bind(s, 1, id);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)index);
            if (sqlite3_step(s) != SQLITE_ROW || sqlite3_column_bytes(s, 0) != 32 || sodium_memcmp(hash, sqlite3_column_blob(s, 0), 32)) error = DROP_E_CRYPTO;
        }
        sqlite3_finalize(s);
        LeaveCriticalSection(&server->database_lock);
    }
done:
    if (error != DROP_OK && *data) {
        sodium_memzero(*data, *length);
        free(*data);
        *data = NULL;
        *length = 0;
    }
    free(encrypted);
    sodium_memzero(&object, sizeof(object));
    LeaveCriticalSection(&server->objects_lock);
    return error;
}
/* 按消息附件顺序读取元数据并填入调用者提供的数组，不把未完成上传当作可下载附件。 */
int drop_attachment_list(drop_server *server, const char *message, cJSON *array) {
    sqlite3_stmt *s = NULL;
    int ok = 0, step;
    if (sqlite3_prepare_v2(server->database,
            "SELECT o.object_id,o.filename,o.total,o.sha256 FROM attachments a JOIN objects o USING(object_id) WHERE message_id=?1 ORDER BY position", -1, &s, NULL) != SQLITE_OK)
        goto done;
    bind(s, 1, message);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddStringToObject(item, "object_id", (const char *)sqlite3_column_text(s, 0)) ||
            !cJSON_AddStringToObject(item, "filename", (const char *)sqlite3_column_text(s, 1)) || !number(item, "total_size", (uint64_t)sqlite3_column_int64(s, 2)) ||
            !cJSON_AddStringToObject(item, "sha256", (const char *)sqlite3_column_text(s, 3)) || !cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item);
            goto done;
        }
    }
    ok = step == SQLITE_DONE;
done:
    sqlite3_finalize(s);
    return ok;
}

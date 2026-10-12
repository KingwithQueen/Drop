/* 服务端持久消息、幂等提交和账号增量序列，正文与双方事件在同一事务提交。 */
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 执行消息事务 SQL，提交与回滚由业务函数维护。 */
static int sql(sqlite3 *db, const char *text) {
    return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK;
}
/* 先结束并清空旧语句，再准备服务器消息查询；首次调用时 *s 必须为空，成功语句由调用者 sqlite3_finalize。 */
static int prepare(sqlite3 *db, sqlite3_stmt **s, const char *text) {
    sqlite3_finalize(*s);
    *s = NULL;
    return sqlite3_prepare_v2(db, text, -1, s, NULL) == SQLITE_OK;
}
/* 复制绑定消息或账号文本，避免语句依赖临时 JSON 缓冲。 */
static void bind(sqlite3_stmt *s, int index, const char *text) {
    sqlite3_bind_text(s, index, text, -1, SQLITE_TRANSIENT);
}
/* 将 64 位数值写成十进制 JSON 字符串，避免 JSON 浮点数丢失精度。 */
static int add_u64(cJSON *o, const char *name, sqlite3_int64 value) {
    char number[32];
    if (value < 0) return 0;
    sprintf_s(number, sizeof(number), "%llu", (unsigned long long)value);
    return cJSON_AddStringToObject(o, name, number) != NULL;
}

/* 在启动独占锁下先检查点再复制加密数据库及匹配密钥，核查回退副本后才允许结构迁移。 */
int drop_schema_snapshot(drop_server *server, int version) {
    sqlite3_stmt *s = NULL;
    int ok = 0;
    WCHAR db_path[32768], backup_path[32768], name[80], wide[DROP_ID_CAP];
    HANDLE file;
    sqlite3 *verify = NULL;
    char snapshot_id[DROP_ID_CAP];
    char *utf8 = NULL;
    /* 启动锁排除其他写入者，复制加密页前先检查点；保留已验证的回退副本及匹配 DPAPI 密钥，不能仅原地迁移。 */
    if (!prepare(server->database, &s, "PRAGMA wal_checkpoint(TRUNCATE)") || sqlite3_step(s) != SQLITE_ROW || sqlite3_column_int(s, 0) != 0) goto done;
    sqlite3_finalize(s);
    s = NULL;
    drop_id_new(snapshot_id);
    if (!drop_to_wide(snapshot_id, wide, DROP_ID_CAP) || swprintf_s(name, 80, L"migration-v%d.%s.db", version, wide) < 0 ||
        !drop_path_join(server->directory, L"server.db", db_path, 32768) || !drop_path_join(server->directory, name, backup_path, 32768) || !CopyFileW(db_path, backup_path, TRUE))
        goto done;
    file = CreateFileW(backup_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    ok = FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) goto done;
    ok = 0;
    utf8 = malloc(131072);
    if (!utf8 || !drop_to_utf8(backup_path, utf8, 131072) || sqlite3_open_v2(utf8, &verify, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_key(verify, server->keys.database_key, 32) != SQLITE_OK || !prepare(verify, &s, "PRAGMA integrity_check") || sqlite3_step(s) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(s, 0), "ok"))
        goto done;
    sqlite3_finalize(s);
    s = NULL;
    sqlite3_close(verify);
    verify = NULL;
    ok = 1;
done:
    sqlite3_finalize(s);
    if (verify) sqlite3_close(verify);
    free(utf8);
    return ok;
}

/* 建立或迁移消息、幂等请求和同步序列结构，保留旧版正文指纹的重试语义。 */
int drop_text_schema(drop_server *server) {
    sqlite3_stmt *s = NULL;
    int version = 0, ok;
    if (!prepare(server->database, &s, "PRAGMA user_version") || sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return 0;
    }
    version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (version >= 2 && version <= DROP_SCHEMA_VERSION) return 1;
    if (version != 1 || !drop_schema_snapshot(server, 1)) return 0;
    ok = sql(server->database,
        "BEGIN IMMEDIATE;"
        "CREATE TABLE conversations(conversation_id TEXT PRIMARY KEY,lo TEXT NOT NULL REFERENCES accounts(user_id),"
        "hi TEXT NOT NULL REFERENCES accounts(user_id),UNIQUE(lo,hi));"
        "CREATE TABLE messages(message_id TEXT PRIMARY KEY,request_id TEXT NOT NULL,conversation_id TEXT NOT NULL REFERENCES conversations(conversation_id),"
        "sender TEXT NOT NULL REFERENCES accounts(user_id),recipient TEXT NOT NULL REFERENCES accounts(user_id),device TEXT NOT NULL,"
        "created_at INTEGER NOT NULL,body BLOB NOT NULL,compressed INTEGER NOT NULL,original_length INTEGER NOT NULL,"
        "fingerprint BLOB NOT NULL,UNIQUE(sender,request_id));"
        "CREATE TABLE account_sequences(user_id TEXT PRIMARY KEY REFERENCES accounts(user_id),last_seq INTEGER NOT NULL);"
        "CREATE TABLE events(user_id TEXT NOT NULL REFERENCES accounts(user_id),seq INTEGER NOT NULL,message_id TEXT NOT NULL REFERENCES messages(message_id),"
        "PRIMARY KEY(user_id,seq)); PRAGMA user_version=2; COMMIT;");
    if (!ok) sql(server->database, "ROLLBACK");
    return ok;
}

/* 按登录名查询可发送账号的固定 ID，不公开密码、设备凭据或管理信息。 */
drop_error drop_account_lookup(drop_server *server, const cJSON *input, cJSON **output) {
    sqlite3_stmt *s = NULL;
    drop_error result = DROP_E_ARGUMENT;
    const char *login = drop_json_string(input, "login", 64);
    if (!login) return result;
    EnterCriticalSection(&server->database_lock);
    if (!prepare(server->database, &s, "SELECT user_id,login,display_name FROM accounts WHERE login=?1 AND enabled=1")) {
        result = DROP_E_DATABASE;
        goto done;
    }
    bind(s, 1, login);
    if (sqlite3_step(s) != SQLITE_ROW) goto done;
    *output = cJSON_CreateObject();
    if (!*output || !cJSON_AddStringToObject(*output, "user_id", (const char *)sqlite3_column_text(s, 0)) ||
        !cJSON_AddStringToObject(*output, "login", (const char *)sqlite3_column_text(s, 1)) ||
        !cJSON_AddStringToObject(*output, "display_name", (const char *)sqlite3_column_text(s, 2)) || !cJSON_AddBoolToObject(*output, "enabled", 1) ||
        !cJSON_AddBoolToObject(*output, "deleted", 0)) {
        result = DROP_E_IO;
        goto done;
    }
    result = DROP_OK;
done:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    if (result != DROP_OK) {
        cJSON_Delete(*output);
        *output = NULL;
    }
    return result;
}
/* 为指定账号分配并写入下一个同步序号，在外层消息事务内调用。 */
static int event_add(sqlite3 *db, sqlite3_stmt **s, const char *user, const char *message) {
    if (!prepare(db, s, "INSERT INTO account_sequences VALUES(?1,1) ON CONFLICT(user_id) DO UPDATE SET last_seq=last_seq+1 WHERE last_seq<9223372036854775807")) return 0;
    bind(*s, 1, user);
    if (sqlite3_step(*s) != SQLITE_DONE || sqlite3_changes(db) != 1) return 0;
    if (!prepare(db, s, "INSERT INTO events SELECT ?1,last_seq,?2 FROM account_sequences WHERE user_id=?1")) return 0;
    bind(*s, 1, user);
    bind(*s, 2, message);
    return sqlite3_step(*s) == SQLITE_DONE;
}
/* 检查账号消息和存储配额，超限明确返回空间错误，不自动删除历史。 */
drop_error drop_account_capacity(drop_server *server, const char *user, uint64_t extra) {
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_DATABASE;
    sqlite3_int64 quota, used;
    if (!prepare(server->database, &s,
            "SELECT quota_bytes,"
            "(SELECT coalesce(sum(original_length+1024),0) FROM messages WHERE sender=?1 OR recipient=?1)+"
            "(SELECT coalesce(sum(total+1024),0) FROM objects WHERE owner=?1)+"
            "(SELECT coalesce(sum(o.total+1024),0) FROM objects o JOIN attachments a USING(object_id) JOIN messages m USING(message_id) WHERE m.recipient=?1 AND o.owner!=?1)"
            " FROM accounts WHERE user_id=?1 AND enabled=1"))
        goto done;
    bind(s, 1, user);
    if (sqlite3_step(s) != SQLITE_ROW) {
        error = DROP_E_AUTH;
        goto done;
    }
    quota = sqlite3_column_int64(s, 0);
    used = sqlite3_column_int64(s, 1);
    if (quota < 0 || used < 0) goto done;
    error = extra > (uint64_t)quota || (uint64_t)used > (uint64_t)quota - extra ? DROP_E_SPACE : DROP_OK;
done:
    sqlite3_finalize(s);
    return error;
}
/* 校验目标和完整附件，以账号与请求 ID 保证幂等；同事务保存消息及双方事件，提交后才返回接收成功。 */
drop_error drop_message_submit(drop_server *server, const char *user, const char *device, const cJSON *input, cJSON **output, char recipient[DROP_ID_CAP]) {
    const char *request = drop_json_string(input, "request_id", 32), *body = drop_json_string(input, "body", DROP_MAX_BODY);
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(input, "recipient_user_id");
    const cJSON *attachments = cJSON_GetObjectItemCaseSensitive(input, "attachments");
    const char *lo, *hi;
    char message[DROP_ID_CAP], conversation[DROP_ID_CAP], body_length[32];
    int attachment_count = attachments ? cJSON_GetArraySize(attachments) : 0, i, j;
    uint64_t recipient_bytes = 1024;
    unsigned char fingerprint[32];
    crypto_hash_sha256_state hash;
    sqlite3_stmt *s = NULL;
    drop_error result = DROP_E_DATABASE;
    drop_text packed = {0};
    int duplicate = 0;
    if (!drop_id_valid(request) || !body || (!*body && !attachment_count) || (to && (!cJSON_IsString(to) || !drop_id_valid(to->valuestring))) ||
        (attachments && !cJSON_IsArray(attachments)) || attachment_count > DROP_MAX_ATTACHMENTS)
        return DROP_E_ARGUMENT;
    for (i = 0; i < attachment_count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(attachments, i);
        if (!cJSON_IsString(item) || !drop_id_valid(item->valuestring)) return DROP_E_ARGUMENT;
        for (j = 0; j < i; ++j)
            if (!strcmp(item->valuestring, cJSON_GetArrayItem(attachments, j)->valuestring)) return DROP_E_ARGUMENT;
    }
    strcpy_s(recipient, DROP_ID_CAP, to ? to->valuestring : user);
    crypto_hash_sha256_init(&hash);
    crypto_hash_sha256_update(&hash, (const unsigned char *)recipient, 32);
    sprintf_s(body_length, sizeof(body_length), "%zu", strlen(body));
    crypto_hash_sha256_update(&hash, (const unsigned char *)body_length, strlen(body_length) + 1);
    crypto_hash_sha256_update(&hash, (const unsigned char *)body, strlen(body));
    {
        unsigned char count = (unsigned char)attachment_count;
        crypto_hash_sha256_update(&hash, &count, 1);
    }
    for (i = 0; i < attachment_count; ++i)
        crypto_hash_sha256_update(&hash, (const unsigned char *)cJSON_GetArrayItem(attachments, i)->valuestring, 32);
    crypto_hash_sha256_final(&hash, fingerprint);
    if (!drop_text_pack(body, server->use_zstd, server->zstd_level, &packed)) return DROP_E_ARGUMENT;
    EnterCriticalSection(&server->database_lock);
    if (!sql(server->database, "BEGIN IMMEDIATE")) goto done;
    if (!prepare(server->database, &s,
            "SELECT message_id,fingerprint,(SELECT count(*) FROM attachments a WHERE a.message_id=messages.message_id) FROM messages WHERE sender=?1 AND request_id=?2"))
        goto rollback;
    bind(s, 1, user);
    bind(s, 2, request);
    if (sqlite3_step(s) == SQLITE_ROW) {
        if (sqlite3_column_bytes(s, 1) != 32) {
            result = DROP_E_CONFLICT;
            goto rollback;
        }
        if (sodium_memcmp(sqlite3_column_blob(s, 1), fingerprint, 32)) {
            unsigned char legacy[32];
            crypto_hash_sha256_init(&hash);
            crypto_hash_sha256_update(&hash, (const unsigned char *)recipient, 32);
            crypto_hash_sha256_update(&hash, (const unsigned char *)body, strlen(body));
            crypto_hash_sha256_final(&hash, legacy);
            if (attachment_count || sqlite3_column_int(s, 2) || sodium_memcmp(sqlite3_column_blob(s, 1), legacy, 32)) {
                result = DROP_E_CONFLICT;
                goto rollback;
            }
        }
        strcpy_s(message, sizeof(message), (const char *)sqlite3_column_text(s, 0));
        duplicate = 1;
    } else {
        for (i = 0; i < attachment_count; ++i) {
            if (!prepare(server->database, &s,
                    "SELECT owner,state,(SELECT count(*) FROM attachments WHERE object_id=?1)+(SELECT count(*) FROM object_deletions WHERE object_id=?1),total FROM objects WHERE "
                    "object_id=?1"))
                goto rollback;
            bind(s, 1, cJSON_GetArrayItem(attachments, i)->valuestring);
            if (sqlite3_step(s) != SQLITE_ROW || strcmp((const char *)sqlite3_column_text(s, 0), user)) {
                result = DROP_E_AUTH;
                goto rollback;
            }
            if (strcmp((const char *)sqlite3_column_text(s, 1), "complete") || sqlite3_column_int(s, 2)) {
                result = DROP_E_CONFLICT;
                goto rollback;
            }
            if (sqlite3_column_int64(s, 3) < 0) goto rollback;
            recipient_bytes += (uint64_t)sqlite3_column_int64(s, 3) + 1024;
        }
        if (!prepare(server->database, &s, "SELECT 1 FROM accounts WHERE user_id=?1 AND enabled=1")) goto rollback;
        bind(s, 1, recipient);
        if (sqlite3_step(s) != SQLITE_ROW) {
            result = DROP_E_ARGUMENT;
            goto rollback;
        }
        result = drop_account_capacity(server, user, (uint64_t)packed.original_size + 1024);
        if (result != DROP_OK) goto rollback;
        if (strcmp(user, recipient)) {
            result = drop_account_capacity(server, recipient, recipient_bytes + (uint64_t)packed.original_size);
            if (result != DROP_OK) goto rollback;
        }
        result = DROP_E_DATABASE;
        lo = strcmp(user, recipient) <= 0 ? user : recipient;
        hi = strcmp(user, recipient) <= 0 ? recipient : user;
        drop_id_new(conversation);
        if (!prepare(server->database, &s, "INSERT INTO conversations VALUES(?1,?2,?3) ON CONFLICT(lo,hi) DO NOTHING")) goto rollback;
        bind(s, 1, conversation);
        bind(s, 2, lo);
        bind(s, 3, hi);
        if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
        if (!prepare(server->database, &s, "SELECT conversation_id FROM conversations WHERE lo=?1 AND hi=?2")) goto rollback;
        bind(s, 1, lo);
        bind(s, 2, hi);
        if (sqlite3_step(s) != SQLITE_ROW) goto rollback;
        strcpy_s(conversation, sizeof(conversation), (const char *)sqlite3_column_text(s, 0));
        drop_id_new(message);
        if (!prepare(server->database, &s,
                "INSERT INTO messages VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,(SELECT CASE WHEN coalesce(max(server_order),0)<9223372036854775807 THEN "
                "coalesce(max(server_order),0)+1 ELSE NULL END FROM messages))"))
            goto rollback;
        bind(s, 1, message);
        bind(s, 2, request);
        bind(s, 3, conversation);
        bind(s, 4, user);
        bind(s, 5, recipient);
        bind(s, 6, device);
        sqlite3_bind_int64(s, 7, (sqlite3_int64)time(NULL));
        sqlite3_bind_blob(s, 8, packed.data, (int)packed.size, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 9, packed.compressed);
        sqlite3_bind_int64(s, 10, (sqlite3_int64)packed.original_size);
        sqlite3_bind_blob(s, 11, fingerprint, 32, SQLITE_TRANSIENT);
        if (sqlite3_step(s) != SQLITE_DONE || !event_add(server->database, &s, user, message) || (strcmp(user, recipient) && !event_add(server->database, &s, recipient, message)))
            goto rollback;
        for (i = 0; i < attachment_count; ++i) {
            if (!prepare(server->database, &s, "INSERT INTO attachments VALUES(?1,?2,?3)")) goto rollback;
            bind(s, 1, message);
            sqlite3_bind_int(s, 2, i);
            bind(s, 3, cJSON_GetArrayItem(attachments, i)->valuestring);
            if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
        }
    }
    *output = cJSON_CreateObject();
    if (!*output || !cJSON_AddStringToObject(*output, "message_id", message) || !cJSON_AddBoolToObject(*output, "duplicate", duplicate)) {
        result = DROP_E_IO;
        goto rollback;
    }
    if (!sql(server->database, "COMMIT")) goto rollback;
    result = DROP_OK;
    goto done;
rollback:
    if (result == DROP_E_DATABASE && sqlite3_errcode(server->database) == SQLITE_FULL) result = DROP_E_SPACE;
    sql(server->database, "ROLLBACK");
done:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    drop_text_free(&packed);
    if (result != DROP_OK) {
        cJSON_Delete(*output);
        *output = NULL;
    }
    return result;
}

/* 统计 JSON 字符串外的结构逗号，供增量分页控制解析复杂度，不把正文中的逗号计入。 */
static size_t structural_commas(const cJSON *object) {
    const cJSON *child;
    size_t count = 0;
    int seen = 0;
    cJSON_ArrayForEach(child, object) {
        if (seen++) ++count;
        count += structural_commas(child);
    }
    return count;
}
/* 校验服务器代次和账号游标，按记录数及响应容量返回有序增量；正文及附件关联来自持久数据。 */
drop_error drop_sync_read(drop_server *server, const char *user, const cJSON *input, cJSON **output) {
    const char *generation = drop_json_string(input, "generation", 32), *after_text = drop_json_string(input, "after", 20);
    const cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(input, "limit");
    uint64_t after;
    int limit = 50, count = 0, step;
    size_t budget = 256, commas = 32;
    sqlite3_int64 next, highest = 0;
    sqlite3_stmt *s = NULL;
    cJSON *events = NULL;
    drop_error result = DROP_E_DATABASE;
    if (!generation || strcmp(generation, server->keys.generation)) return DROP_E_CONFLICT;
    if (!drop_u64_parse(after_text, &after) || after > INT64_MAX) return DROP_E_ARGUMENT;
    if (limit_item) {
        if (!cJSON_IsNumber(limit_item) || limit_item->valuedouble < 1 || limit_item->valuedouble > 100 || limit_item->valuedouble != (double)limit_item->valueint)
            return DROP_E_ARGUMENT;
        limit = limit_item->valueint;
    }
    next = (sqlite3_int64)after;
    EnterCriticalSection(&server->database_lock);
    if (!prepare(server->database, &s, "SELECT last_seq FROM account_sequences WHERE user_id=?1")) goto done;
    bind(s, 1, user);
    if (sqlite3_step(s) == SQLITE_ROW) highest = sqlite3_column_int64(s, 0);
    if (next > highest) {
        result = DROP_E_CONFLICT;
        goto done;
    }
    *output = cJSON_CreateObject();
    if (!*output) {
        result = DROP_E_IO;
        goto done;
    }
    events = cJSON_AddArrayToObject(*output, "events");
    if (!events || !cJSON_AddStringToObject(*output, "server_id", server->keys.server_id) || !cJSON_AddStringToObject(*output, "generation", generation)) {
        result = DROP_E_IO;
        goto done;
    }
    if (!prepare(server->database, &s,
            "SELECT e.seq,m.message_id,m.request_id,m.conversation_id,m.sender,m.recipient,m.device,m.created_at,"
            "m.body,m.compressed,m.original_length,m.server_order,coalesce(d.deleted_at,0) FROM events e JOIN messages m USING(message_id) LEFT JOIN message_deletions d "
            "USING(message_id) WHERE e.user_id=?1 AND e.seq>?2 ORDER BY e.seq LIMIT ?3"))
        goto done;
    bind(s, 1, user);
    sqlite3_bind_int64(s, 2, next);
    sqlite3_bind_int(s, 3, limit);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        cJSON *event = cJSON_CreateObject(), *message;
        char *body = NULL, *encoded = NULL;
        drop_text packed;
        sqlite3_int64 seq = sqlite3_column_int64(s, 0), original = sqlite3_column_int64(s, 10);
        if (!event || seq != next + 1 || original < 0 || original > DROP_MAX_BODY) {
            cJSON_Delete(event);
            goto done;
        }
        packed.data = (unsigned char *)sqlite3_column_blob(s, 8);
        packed.size = (size_t)sqlite3_column_bytes(s, 8);
        packed.compressed = sqlite3_column_int(s, 9);
        packed.original_size = (size_t)original;
        if (!drop_text_unpack(&packed, &body)) {
            cJSON_Delete(event);
            goto done;
        }
        message = cJSON_AddObjectToObject(event, "message");
        if (!message || !add_u64(event, "seq", seq) || !cJSON_AddStringToObject(message, "message_id", (const char *)sqlite3_column_text(s, 1)) ||
            !cJSON_AddStringToObject(message, "request_id", (const char *)sqlite3_column_text(s, 2)) ||
            !cJSON_AddStringToObject(message, "conversation_id", (const char *)sqlite3_column_text(s, 3)) ||
            !cJSON_AddStringToObject(message, "sender_user_id", (const char *)sqlite3_column_text(s, 4)) ||
            !cJSON_AddStringToObject(message, "recipient_user_id", (const char *)sqlite3_column_text(s, 5)) ||
            !cJSON_AddStringToObject(message, "sender_device_id", (const char *)sqlite3_column_text(s, 6)) || !add_u64(message, "created_at", sqlite3_column_int64(s, 7)) ||
            !add_u64(message, "server_order", sqlite3_column_int64(s, 11)) || !cJSON_AddStringToObject(message, "body", body) ||
            (sqlite3_column_int64(s, 12) && (!cJSON_AddBoolToObject(message, "deleted", 1) || !add_u64(message, "deleted_at", sqlite3_column_int64(s, 12))))) {
            free(body);
            cJSON_Delete(event);
            result = DROP_E_IO;
            goto done;
        }
        free(body);
        encoded = cJSON_PrintUnformatted(event);
        {
            cJSON *files = cJSON_AddArrayToObject(message, "attachments");
            if (!files || !drop_attachment_list(server, (const char *)sqlite3_column_text(s, 1), files)) {
                cJSON_free(encoded);
                cJSON_Delete(event);
                result = DROP_E_DATABASE;
                goto done;
            }
            cJSON_free(encoded);
            encoded = cJSON_PrintUnformatted(event);
        }
        if (!encoded) {
            cJSON_Delete(event);
            result = DROP_E_IO;
            goto done;
        }
        budget += strlen(encoded) + 1;
        cJSON_free(encoded);
        commas += structural_commas(event) + 1;
        if (budget >= DROP_MAX_JSON || commas > 4096) {
            cJSON_Delete(event);
            if (!count) result = DROP_E_LIMIT;
            break;
        }
        if (!cJSON_AddItemToArray(events, event)) {
            cJSON_Delete(event);
            result = DROP_E_IO;
            goto done;
        }
        next = seq;
        ++count;
    }
    if (step != SQLITE_ROW && step != SQLITE_DONE) goto done;
    if (result == DROP_E_LIMIT || !add_u64(*output, "next", next) || !cJSON_AddBoolToObject(*output, "more", next < highest)) goto done;
    result = DROP_OK;
done:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    if (result != DROP_OK) {
        cJSON_Delete(*output);
        *output = NULL;
    }
    return result;
}
#if DROP_DIAGNOSTICS
/* 在事务内构造受限模拟附件元数据验证真实分页序列化，结束回滚；不代表附件密文已验收。 */
int drop_pagination_test(drop_server *server, const char *user, const char *device) {
    sqlite3_stmt *message = NULL, *object = NULL, *attachment = NULL, *s = NULL;
    char conversation[DROP_ID_CAP], id[DROP_ID_CAP], child[DROP_ID_CAP], cursor[32] = "0";
    int i, j, total = 0, pages = 0, ok = 0;
    unsigned char hash[32] = {0};
    cJSON *input = NULL, *out = NULL, *parsed = NULL;
    char *encoded = NULL;
    /* 用模拟加密元数据验证实际同步序列化和解析；不代表附件密文已验证，结束后回滚测试事务。 */
    if (!sql(server->database, "SAVEPOINT pagination_fixture")) return 0;
    drop_id_new(conversation);
    if (!prepare(server->database, &s, "INSERT INTO conversations VALUES(?1,?2,?2)")) goto done;
    bind(s, 1, conversation);
    bind(s, 2, user);
    if (sqlite3_step(s) != SQLITE_DONE) goto done;
    if (!prepare(server->database, &s, "INSERT INTO account_sequences VALUES(?1,50)")) goto done;
    bind(s, 1, user);
    if (sqlite3_step(s) != SQLITE_DONE) goto done;
    if (!prepare(server->database, &message, "INSERT INTO messages VALUES(?1,?1,?2,?3,?3,?4,0,x'78',0,1,?5,?6)") ||
        !prepare(server->database, &object,
            "INSERT INTO objects(object_id,owner,request_id,filename,total,sha256,header,state,created_at,last_activity) "
            "VALUES(?1,?2,?1,'dense.bin',0,?3,zeroblob(260),'complete',0,0)") ||
        !prepare(server->database, &attachment, "INSERT INTO attachments VALUES(?1,?2,?3)"))
        goto done;
    for (i = 0; i < 50; ++i) {
        drop_id_new(id);
        sqlite3_reset(message);
        bind(message, 1, id);
        bind(message, 2, conversation);
        bind(message, 3, user);
        bind(message, 4, device);
        sqlite3_bind_blob(message, 5, hash, 32, SQLITE_TRANSIENT);
        sqlite3_bind_int(message, 6, i + 1);
        if (sqlite3_step(message) != SQLITE_DONE) goto done;
        if (!prepare(server->database, &s, "INSERT INTO events VALUES(?1,?2,?3)")) goto done;
        bind(s, 1, user);
        sqlite3_bind_int(s, 2, i + 1);
        bind(s, 3, id);
        if (sqlite3_step(s) != SQLITE_DONE) goto done;
        for (j = 0; j < DROP_MAX_ATTACHMENTS; ++j) {
            drop_id_new(child);
            sqlite3_reset(object);
            bind(object, 1, child);
            bind(object, 2, user);
            bind(object, 3, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
            if (sqlite3_step(object) != SQLITE_DONE) goto done;
            sqlite3_reset(attachment);
            bind(attachment, 1, id);
            sqlite3_bind_int(attachment, 2, j);
            bind(attachment, 3, child);
            if (sqlite3_step(attachment) != SQLITE_DONE) goto done;
        }
    }
    do {
        const char *next;
        int count;
        cJSON_Delete(input);
        cJSON_Delete(out);
        cJSON_Delete(parsed);
        cJSON_free(encoded);
        input = cJSON_CreateObject();
        out = NULL;
        parsed = NULL;
        encoded = NULL;
        if (!input || !cJSON_AddStringToObject(input, "generation", server->keys.generation) || !cJSON_AddStringToObject(input, "after", cursor) ||
            !cJSON_AddNumberToObject(input, "limit", 50) || drop_sync_read(server, user, input, &out) != DROP_OK)
            goto done;
        encoded = cJSON_PrintUnformatted(out);
        if (!encoded || !(parsed = drop_json_parse(encoded, strlen(encoded)))) goto done;
        count = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(parsed, "events"));
        next = drop_json_string(parsed, "next", 20);
        if (!count || !next || ++pages > 10 || (pages == 1 && count == 50)) goto done;
        total += count;
        strcpy_s(cursor, sizeof(cursor), next);
    } while (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(parsed, "more")));
    ok = total == 50 && pages > 1;
done:
    sqlite3_finalize(message);
    sqlite3_finalize(object);
    sqlite3_finalize(attachment);
    sqlite3_finalize(s);
    cJSON_Delete(input);
    cJSON_Delete(out);
    cJSON_Delete(parsed);
    cJSON_free(encoded);
    if (!sql(server->database, "ROLLBACK TO pagination_fixture; RELEASE pagination_fixture")) ok = 0;
    if (ok) puts("PASS sync: dense 50x32 attachment metadata paginates within strict JSON limits");
    return ok;
}

#endif

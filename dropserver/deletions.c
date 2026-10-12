/* 账号级消息删除和独立持久增量；保留身份与指纹阻止幂等重试复活正文，附件内容由原清理流程回收。 */
#include "server.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static int sql(sqlite3 *db, const char *text) {
    return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK;
}
/* 先释放旧语句再准备查询，成功语句由调用者结束；仅在持有数据库锁的事务中使用。 */
static int prepare(sqlite3 *db, sqlite3_stmt **s, const char *text) {
    sqlite3_finalize(*s);
    *s = NULL;
    return sqlite3_prepare_v2(db, text, -1, s, NULL) == SQLITE_OK;
}
static void bind(sqlite3_stmt *s, int index, const char *text) {
    sqlite3_bind_text(s, index, text, -1, SQLITE_TRANSIENT);
}
/* 协议中的时间、顺序和游标使用十进制字符串，避免浮点数舍入。 */
static int number(cJSON *object, const char *key, sqlite3_int64 value) {
    char text[32];
    if (value < 0) return 0;
    sprintf_s(text, sizeof(text), "%llu", (unsigned long long)value);
    return cJSON_AddStringToObject(object, key, text) != NULL;
}
/* 启动独占锁下迁移；结构重名或数据库异常必须回滚，不能跳过未核对的旧表。 */
int drop_deletions_schema(drop_server *server) {
    sqlite3_stmt *s = NULL;
    int version = -1;
    if (prepare(server->database, &s, "PRAGMA user_version") && sqlite3_step(s) == SQLITE_ROW) version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (version == 12) return 1;
    if (version != 11 || !drop_schema_snapshot(server, 11)) return 0;
    if (!sql(server->database,
            "BEGIN IMMEDIATE;"
            "CREATE TABLE message_deletions(message_id TEXT PRIMARY KEY REFERENCES messages(message_id),deleted_at INTEGER NOT NULL);"
            "CREATE TABLE object_deletions(object_id TEXT PRIMARY KEY,message_id TEXT NOT NULL REFERENCES messages(message_id));"
            "CREATE TABLE deletion_sequences(user_id TEXT PRIMARY KEY REFERENCES accounts(user_id),last_seq INTEGER NOT NULL);"
            "CREATE TABLE deletion_events(user_id TEXT NOT NULL REFERENCES accounts(user_id),seq INTEGER NOT NULL,"
            "message_id TEXT NOT NULL REFERENCES message_deletions(message_id),PRIMARY KEY(user_id,seq)); PRAGMA user_version=12; COMMIT;")) {
        sql(server->database, "ROLLBACK");
        return 0;
    }
    return 1;
}
/* 查询列以六个固定身份开头，随后为创建时间、消息顺序及删除时间；只序列化身份，不包含正文和文件名。 */
static cJSON *payload(sqlite3_stmt *s, int offset) {
    static const char *fields[] = {"message_id", "request_id", "conversation_id", "sender_user_id", "recipient_user_id", "sender_device_id"};
    cJSON *object = cJSON_CreateObject();
    if (!object) return NULL;
    for (int i = 0; i < 6; ++i)
        if (!cJSON_AddStringToObject(object, fields[i], (const char *)sqlite3_column_text(s, offset + i))) goto fail;
    if (!number(object, "created_at", sqlite3_column_int64(s, offset + 6)) || !number(object, "server_order", sqlite3_column_int64(s, offset + 7)) ||
        !number(object, "deleted_at", sqlite3_column_int64(s, offset + 8)) || !cJSON_AddBoolToObject(object, "deleted", 1))
        goto fail;
    return object;
fail:
    cJSON_Delete(object);
    return NULL;
}
/* 外层删除事务内为一个账号分配连续序号；自发只调用一次，双方删除通知在提交后由路由发送。 */
static int event_add(sqlite3 *db, sqlite3_stmt **s, const char *user, const char *id) {
    if (!prepare(db, s, "INSERT INTO deletion_sequences VALUES(?1,1) ON CONFLICT(user_id) DO UPDATE SET last_seq=last_seq+1 WHERE last_seq<9223372036854775807")) return 0;
    bind(*s, 1, user);
    if (sqlite3_step(*s) != SQLITE_DONE || sqlite3_changes(db) != 1 ||
        !prepare(db, s, "INSERT INTO deletion_events SELECT ?1,last_seq,?2 FROM deletion_sequences WHERE user_id=?1"))
        return 0;
    bind(*s, 1, user);
    bind(*s, 2, id);
    return sqlite3_step(*s) == SQLITE_DONE;
}
/* 权限比较 sender 与认证账号，不比较发送设备；已删除请求幂等，不额外生成事件或改变原删除时间。 */
drop_error drop_message_delete(drop_server *server, const char *user, const char *token, const cJSON *input, cJSON **output, char recipient[DROP_ID_CAP]) {
    const char *id = drop_json_string(input, "message_id", 32), *generation = drop_json_string(input, "generation", 32);
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_DATABASE;
    int changed = 0;
    if (!drop_id_valid(id) || !drop_id_valid(generation)) return DROP_E_ARGUMENT;
    if (strcmp(generation, server->keys.generation)) return DROP_E_CONFLICT;
    EnterCriticalSection(&server->objects_lock);
    EnterCriticalSection(&server->database_lock);
    /* 管理操作可能刚撤销设备或账号；在事务边界重新核对原令牌，避免旧认证结果跨越在线撤销。 */
    char authorized_user[DROP_ID_CAP], authorized_device[DROP_ID_CAP];
    if (!token || !drop_authorize(server, token, authorized_user, authorized_device) || strcmp(authorized_user, user)) {
        error = DROP_E_AUTH;
        goto done;
    }
    if (!sql(server->database, "BEGIN IMMEDIATE") ||
        !prepare(server->database, &s, "SELECT sender,recipient,EXISTS(SELECT 1 FROM message_deletions d WHERE d.message_id=m.message_id) FROM messages m WHERE message_id=?1"))
        goto rollback;
    bind(s, 1, id);
    if (sqlite3_step(s) != SQLITE_ROW) {
        error = DROP_E_ARGUMENT;
        goto rollback;
    }
    if (strcmp((const char *)sqlite3_column_text(s, 0), user)) {
        error = DROP_E_AUTH;
        goto rollback;
    }
    strcpy_s(recipient, DROP_ID_CAP, (const char *)sqlite3_column_text(s, 1));
    changed = !sqlite3_column_int(s, 2);
    if (changed) {
        if (!prepare(server->database, &s, "INSERT INTO message_deletions VALUES(?1,?2)")) goto rollback;
        bind(s, 1, id);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)time(NULL));
        if (sqlite3_step(s) != SQLITE_DONE || !prepare(server->database, &s, "INSERT INTO object_deletions SELECT object_id,message_id FROM attachments WHERE message_id=?1"))
            goto rollback;
        bind(s, 1, id);
        if (sqlite3_step(s) != SQLITE_DONE ||
            !prepare(server->database, &s, "DELETE FROM saved_objects WHERE object_id IN(SELECT object_id FROM object_deletions WHERE message_id=?1)"))
            goto rollback;
        bind(s, 1, id);
        if (sqlite3_step(s) != SQLITE_DONE || !prepare(server->database, &s, "DELETE FROM attachments WHERE message_id=?1")) goto rollback;
        bind(s, 1, id);
        if (sqlite3_step(s) != SQLITE_DONE || !prepare(server->database, &s, "UPDATE messages SET body=X'',compressed=0,original_length=0 WHERE message_id=?1")) goto rollback;
        bind(s, 1, id);
        if (sqlite3_step(s) != SQLITE_DONE || !event_add(server->database, &s, user, id) || (strcmp(user, recipient) && !event_add(server->database, &s, recipient, id)))
            goto rollback;
    }
    if (!prepare(server->database, &s,
            "SELECT m.message_id,m.request_id,m.conversation_id,m.sender,m.recipient,m.device,m.created_at,m.server_order,d.deleted_at FROM messages m JOIN message_deletions d "
            "USING(message_id) WHERE m.message_id=?1"))
        goto rollback;
    bind(s, 1, id);
    if (sqlite3_step(s) != SQLITE_ROW) goto rollback;
    *output = cJSON_CreateObject();
    cJSON *message = payload(s, 0);
    if (!*output || !message || !cJSON_AddBoolToObject(*output, "deleted", 1) || !cJSON_AddBoolToObject(*output, "changed", changed) ||
        !cJSON_AddItemToObject(*output, "message", message)) {
        cJSON_Delete(message);
        error = DROP_E_IO;
        goto rollback;
    }
    sqlite3_finalize(s);
    s = NULL;
    if (!sql(server->database, "COMMIT")) goto rollback;
    error = DROP_OK;
    goto done;
rollback:
    if (error == DROP_E_DATABASE && sqlite3_errcode(server->database) == SQLITE_FULL) error = DROP_E_SPACE;
    sql(server->database, "ROLLBACK");
done:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    LeaveCriticalSection(&server->objects_lock);
    if (error != DROP_OK) {
        recipient[0] = 0;
        cJSON_Delete(*output);
        *output = NULL;
    }
    return error;
}
/* 删除序列独立于正文和已读；固定小页保证严格 JSON 容量，旧设备长期离线仍能从零开始补齐。 */
drop_error drop_deletions_read(drop_server *server, const char *user, const cJSON *input, cJSON **output) {
    const char *generation = drop_json_string(input, "generation", 32);
    uint64_t after;
    sqlite3_int64 highest = 0, next;
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_DATABASE;
    int step;
    if (!drop_id_valid(generation) || !drop_u64_parse(drop_json_string(input, "after", 20), &after) || after > INT64_MAX) return DROP_E_ARGUMENT;
    if (strcmp(generation, server->keys.generation)) return DROP_E_CONFLICT;
    next = (sqlite3_int64)after;
    EnterCriticalSection(&server->database_lock);
    if (!prepare(server->database, &s, "SELECT last_seq FROM deletion_sequences WHERE user_id=?1")) goto done;
    bind(s, 1, user);
    if (sqlite3_step(s) == SQLITE_ROW) highest = sqlite3_column_int64(s, 0);
    if (next > highest) {
        error = DROP_E_CONFLICT;
        goto done;
    }
    *output = cJSON_CreateObject();
    cJSON *events = *output ? cJSON_AddArrayToObject(*output, "events") : NULL;
    if (!events || !cJSON_AddStringToObject(*output, "server_id", server->keys.server_id) || !cJSON_AddStringToObject(*output, "generation", generation)) {
        error = DROP_E_IO;
        goto done;
    }
    if (!prepare(server->database, &s,
            "SELECT e.seq,m.message_id,m.request_id,m.conversation_id,m.sender,m.recipient,m.device,m.created_at,m.server_order,d.deleted_at FROM deletion_events e JOIN messages "
            "m USING(message_id) JOIN message_deletions d USING(message_id) WHERE e.user_id=?1 AND e.seq>?2 ORDER BY e.seq LIMIT 50"))
        goto done;
    bind(s, 1, user);
    sqlite3_bind_int64(s, 2, next);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        cJSON *event = cJSON_CreateObject(), *message = payload(s, 1);
        sqlite3_int64 seq = sqlite3_column_int64(s, 0);
        if (!event || !message || seq != next + 1 || !number(event, "seq", seq) || !cJSON_AddItemToObject(event, "message", message)) {
            cJSON_Delete(message);
            cJSON_Delete(event);
            error = DROP_E_IO;
            goto done;
        }
        if (!cJSON_AddItemToArray(events, event)) {
            cJSON_Delete(event);
            error = DROP_E_IO;
            goto done;
        }
        next = seq;
    }
    if (step != SQLITE_DONE || !number(*output, "next", next) || !cJSON_AddBoolToObject(*output, "more", next < highest)) goto done;
    error = DROP_OK;
done:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    if (error != DROP_OK) {
        cJSON_Delete(*output);
        *output = NULL;
    }
    return error;
}

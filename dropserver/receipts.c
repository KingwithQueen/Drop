/* 设备正文及附件保存确认、账号级已读和独立状态增量，不把服务器接收当作设备保存。 */
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 执行服务端状态事件 SQL，事务由保存或已读业务函数负责。 */
static int sql(sqlite3 *db, const char *text) {
    return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK;
}
/* 先结束并清空旧语句，再准备设备状态查询；首次调用时 *s 必须为空，成功语句由调用者 sqlite3_finalize。 */
static int prepare(sqlite3 *db, sqlite3_stmt **s, const char *text) {
    sqlite3_finalize(*s);
    *s = NULL;
    return sqlite3_prepare_v2(db, text, -1, s, NULL) == SQLITE_OK;
}
/* 复制绑定设备、消息或账号标识。 */
static void bind(sqlite3_stmt *s, int index, const char *text) {
    sqlite3_bind_text(s, index, text, -1, SQLITE_TRANSIENT);
}
/* 拒绝负状态位置，并将合法 64 位位置写成十进制 JSON 字符串，避免浮点数精度损失。 */
static int number(cJSON *out, const char *key, sqlite3_int64 value) {
    char text[32];
    if (value < 0) return 0;
    sprintf_s(text, sizeof(text), "%llu", (unsigned long long)value);
    return cJSON_AddStringToObject(out, key, text) != NULL;
}
/* 建立设备保存、账号已读及独立状态序列表，供跨设备复制进度。 */
int drop_receipts_schema(drop_server *server) {
    sqlite3_stmt *s = NULL;
    int version = -1;
    if (prepare(server->database, &s, "PRAGMA user_version") && sqlite3_step(s) == SQLITE_ROW) version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (version >= 5 && version <= DROP_SCHEMA_VERSION) return 1;
    if (version == 4) {
        if (!drop_schema_snapshot(server, 4)) return 0;
        if (!sql(server->database,
                "BEGIN IMMEDIATE; ALTER TABLE messages ADD COLUMN server_order INTEGER NOT NULL DEFAULT 0;"
                "UPDATE messages SET server_order=rowid; CREATE UNIQUE INDEX messages_server_order ON messages(server_order); PRAGMA user_version=5; COMMIT;")) {
            sql(server->database, "ROLLBACK");
            return 0;
        }
        return 1;
    }
    if (version != 3 || !drop_schema_snapshot(server, 3)) return 0;
    if (!sql(server->database,
            "BEGIN IMMEDIATE;"
            "CREATE TABLE saved_bodies(user_id TEXT NOT NULL,device_id TEXT NOT NULL,generation TEXT NOT NULL,seq INTEGER NOT NULL,PRIMARY KEY(user_id,device_id),FOREIGN "
            "KEY(user_id,device_id) REFERENCES devices(user_id,device_id));"
            "CREATE TABLE saved_objects(user_id TEXT NOT NULL,device_id TEXT NOT NULL,object_id TEXT NOT NULL REFERENCES objects(object_id),PRIMARY "
            "KEY(user_id,device_id,object_id),FOREIGN KEY(user_id,device_id) REFERENCES devices(user_id,device_id));"
            "CREATE TABLE read_positions(user_id TEXT NOT NULL REFERENCES accounts(user_id),conversation_id TEXT NOT NULL REFERENCES conversations(conversation_id),last_order "
            "INTEGER NOT NULL,PRIMARY KEY(user_id,conversation_id));"
            "CREATE TABLE state_sequences(user_id TEXT PRIMARY KEY REFERENCES accounts(user_id),last_seq INTEGER NOT NULL);"
            "CREATE TABLE state_events(user_id TEXT NOT NULL REFERENCES accounts(user_id),seq INTEGER NOT NULL,kind TEXT NOT NULL,payload TEXT NOT NULL,PRIMARY KEY(user_id,seq));"
            "PRAGMA user_version=4; COMMIT;")) {
        sql(server->database, "ROLLBACK");
        return 0;
    }
    return drop_receipts_schema(server);
}
/* 在当前事务中为一个账号分配状态序号并写入事件。 */
static int state_add(sqlite3 *db, const char *user, const char *kind, const char *payload) {
    sqlite3_stmt *s = NULL;
    int ok = 0;
    if (!prepare(db, &s, "INSERT INTO state_sequences VALUES(?1,1) ON CONFLICT(user_id) DO UPDATE SET last_seq=last_seq+1 WHERE last_seq<9223372036854775807")) goto done;
    bind(s, 1, user);
    if (sqlite3_step(s) != SQLITE_DONE || sqlite3_changes(db) != 1) goto done;
    if (!prepare(db, &s, "INSERT INTO state_events SELECT ?1,last_seq,?2,?3 FROM state_sequences WHERE user_id=?1")) goto done;
    bind(s, 1, user);
    bind(s, 2, kind);
    bind(s, 3, payload);
    ok = sqlite3_step(s) == SQLITE_DONE;
done:
    sqlite3_finalize(s);
    return ok;
}
/* 向消息双方写入关联状态事件，同账号自发消息按逻辑身份处理。 */
static int state_pair(drop_server *server, const char *conversation, const char *kind, const cJSON *payload) {
    sqlite3_stmt *s = NULL;
    char lo[DROP_ID_CAP], hi[DROP_ID_CAP], *serialized = NULL;
    int ok = 0;
    if (!prepare(server->database, &s, "SELECT lo,hi FROM conversations WHERE conversation_id=?1")) goto done;
    bind(s, 1, conversation);
    if (sqlite3_step(s) != SQLITE_ROW) goto done;
    strcpy_s(lo, sizeof(lo), (const char *)sqlite3_column_text(s, 0));
    strcpy_s(hi, sizeof(hi), (const char *)sqlite3_column_text(s, 1));
    serialized = cJSON_PrintUnformatted(payload);
    if (!serialized) goto done;
    ok = state_add(server->database, lo, kind, serialized) && (!strcmp(lo, hi) || state_add(server->database, hi, kind, serialized));
done:
    cJSON_free(serialized);
    sqlite3_finalize(s);
    return ok;
}
/* 计算消息正文保存状态，仅依据已授权设备提交的持久确认。 */
static int body_state(drop_server *server, const char *user, const char *device, const char *conversation, sqlite3_int64 order) {
    cJSON *payload = cJSON_CreateObject();
    int ok = payload && cJSON_AddStringToObject(payload, "conversation_id", conversation) && cJSON_AddStringToObject(payload, "user_id", user) &&
        cJSON_AddStringToObject(payload, "device_id", device) && number(payload, "server_order", order) && state_pair(server, conversation, "body_saved", payload);
    cJSON_Delete(payload);
    return ok;
}
/* 检查消息和附件权限，幂等保存设备确认并生成状态事件；失败回滚，未完成附件不可确认。 */
drop_error drop_saved_submit(drop_server *server, const char *user, const char *device, const cJSON *input, cJSON **output) {
    const char *generation = drop_json_string(input, "generation", 32);
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(input, "objects"), *item;
    sqlite3_stmt *s = NULL, *rows = NULL;
    sqlite3_int64 previous = 0, highest = 0;
    uint64_t after;
    drop_error error = DROP_E_DATABASE;
    int changed = 0;
    if (!generation || strcmp(generation, server->keys.generation)) return DROP_E_CONFLICT;
    if (!drop_u64_parse(drop_json_string(input, "body_after", 20), &after) || after > INT64_MAX ||
        (objects && (!cJSON_IsArray(objects) || cJSON_GetArraySize(objects) > DROP_MAX_ATTACHMENTS)))
        return DROP_E_ARGUMENT;
    cJSON_ArrayForEach(item, objects) if (!drop_id_valid(drop_json_string(item, "object_id", 32)) || !drop_hash_valid(drop_json_string(item, "sha256", 64))) return DROP_E_ARGUMENT;
    EnterCriticalSection(&server->database_lock);
    if (!sql(server->database, "BEGIN IMMEDIATE")) goto done;
    if (!prepare(server->database, &s, "SELECT last_seq FROM account_sequences WHERE user_id=?1")) goto rollback;
    bind(s, 1, user);
    if (sqlite3_step(s) == SQLITE_ROW) highest = sqlite3_column_int64(s, 0);
    if ((sqlite3_int64)after > highest) {
        error = DROP_E_CONFLICT;
        goto rollback;
    }
    if (!prepare(server->database, &s, "SELECT seq FROM saved_bodies WHERE user_id=?1 AND device_id=?2 AND generation=?3")) goto rollback;
    bind(s, 1, user);
    bind(s, 2, device);
    bind(s, 3, generation);
    if (sqlite3_step(s) == SQLITE_ROW) previous = sqlite3_column_int64(s, 0);
    if ((sqlite3_int64)after > previous) {
        changed = 1;
        if (!prepare(
                server->database, &s, "INSERT INTO saved_bodies VALUES(?1,?2,?3,?4) ON CONFLICT(user_id,device_id) DO UPDATE SET generation=excluded.generation,seq=excluded.seq"))
            goto rollback;
        bind(s, 1, user);
        bind(s, 2, device);
        bind(s, 3, generation);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)after);
        if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
        if (!prepare(server->database, &rows,
                "SELECT m.conversation_id,max(m.server_order) FROM events e JOIN messages m USING(message_id) WHERE e.user_id=?1 AND e.seq>?2 AND e.seq<=?3 GROUP BY "
                "m.conversation_id"))
            goto rollback;
        bind(rows, 1, user);
        sqlite3_bind_int64(rows, 2, previous);
        sqlite3_bind_int64(rows, 3, (sqlite3_int64)after);
        {
            int step;
            while ((step = sqlite3_step(rows)) == SQLITE_ROW)
                if (!body_state(server, user, device, (const char *)sqlite3_column_text(rows, 0), sqlite3_column_int64(rows, 1))) goto rollback;
            if (step != SQLITE_DONE) goto rollback;
        }
        sqlite3_finalize(rows);
        rows = NULL;
    }
    cJSON_ArrayForEach(item, objects) {
        const char *id = drop_json_string(item, "object_id", 32), *sha = drop_json_string(item, "sha256", 64);
        char conversation[DROP_ID_CAP];
        cJSON *payload;
        if (!prepare(server->database, &s,
                "SELECT m.conversation_id,o.sha256 FROM objects o JOIN attachments a USING(object_id) JOIN messages m USING(message_id) WHERE o.object_id=?1 AND "
                "o.state='complete' AND (m.sender=?2 OR m.recipient=?2)"))
            goto rollback;
        bind(s, 1, id);
        bind(s, 2, user);
        if (sqlite3_step(s) != SQLITE_ROW) {
            /* 删除与旧保存确认交错时，已认证参与者的旧确认不重新建立附件状态，也不误报整账号凭据失效。 */
            if (!prepare(server->database, &s, "SELECT 1 FROM object_deletions d JOIN messages m USING(message_id) WHERE d.object_id=?1 AND (m.sender=?2 OR m.recipient=?2)"))
                goto rollback;
            bind(s, 1, id);
            bind(s, 2, user);
            if (sqlite3_step(s) == SQLITE_ROW) continue;
            error = DROP_E_AUTH;
            goto rollback;
        }
        if (strcmp((const char *)sqlite3_column_text(s, 1), sha)) {
            error = DROP_E_CONFLICT;
            goto rollback;
        }
        strcpy_s(conversation, sizeof(conversation), (const char *)sqlite3_column_text(s, 0));
        if (!prepare(server->database, &s, "INSERT INTO saved_objects VALUES(?1,?2,?3) ON CONFLICT DO NOTHING")) goto rollback;
        bind(s, 1, user);
        bind(s, 2, device);
        bind(s, 3, id);
        if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
        if (!sqlite3_changes(server->database)) continue;
        changed = 1;
        payload = cJSON_CreateObject();
        if (!payload || !cJSON_AddStringToObject(payload, "conversation_id", conversation) || !cJSON_AddStringToObject(payload, "user_id", user) ||
            !cJSON_AddStringToObject(payload, "device_id", device) || !cJSON_AddStringToObject(payload, "object_id", id) ||
            !state_pair(server, conversation, "object_saved", payload)) {
            cJSON_Delete(payload);
            goto rollback;
        }
        cJSON_Delete(payload);
    }
    *output = cJSON_CreateObject();
    if (!*output || !cJSON_AddBoolToObject(*output, "saved", 1) || !cJSON_AddBoolToObject(*output, "changed", changed)) {
        error = DROP_E_IO;
        goto rollback;
    }
    if (!sql(server->database, "COMMIT")) goto rollback;
    error = DROP_OK;
    goto done;
rollback:
    sql(server->database, "ROLLBACK");
done:
    sqlite3_finalize(s);
    sqlite3_finalize(rows);
    LeaveCriticalSection(&server->database_lock);
    if (error != DROP_OK) {
        cJSON_Delete(*output);
        *output = NULL;
    }
    return error;
}
/* 校验账号可见的消息顺序，只向前推进账号已读位置，并以事务写入跨设备状态事件。 */
drop_error drop_read_submit(drop_server *server, const char *user, const cJSON *input, cJSON **output) {
    const char *generation = drop_json_string(input, "generation", 32), *conversation = drop_json_string(input, "conversation_id", 32),
               *message = drop_json_string(input, "message_id", 32);
    sqlite3_stmt *s = NULL;
    sqlite3_int64 order;
    drop_error error = DROP_E_DATABASE;
    cJSON *payload = NULL;
    int changed = 0;
    if (!generation || strcmp(generation, server->keys.generation)) return DROP_E_CONFLICT;
    if (!drop_id_valid(conversation) || !drop_id_valid(message)) return DROP_E_ARGUMENT;
    EnterCriticalSection(&server->database_lock);
    if (!sql(server->database, "BEGIN IMMEDIATE")) goto done;
    if (!prepare(server->database, &s, "SELECT server_order FROM messages WHERE message_id=?1 AND conversation_id=?2 AND (sender=?3 OR recipient=?3)")) goto rollback;
    bind(s, 1, message);
    bind(s, 2, conversation);
    bind(s, 3, user);
    if (sqlite3_step(s) != SQLITE_ROW) {
        error = DROP_E_AUTH;
        goto rollback;
    }
    order = sqlite3_column_int64(s, 0);
    if (!prepare(server->database, &s,
            "INSERT INTO read_positions VALUES(?1,?2,?3) ON CONFLICT(user_id,conversation_id) DO UPDATE SET last_order=excluded.last_order WHERE excluded.last_order>last_order"))
        goto rollback;
    bind(s, 1, user);
    bind(s, 2, conversation);
    sqlite3_bind_int64(s, 3, order);
    if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
    if (sqlite3_changes(server->database)) {
        changed = 1;
        payload = cJSON_CreateObject();
        if (!payload || !cJSON_AddStringToObject(payload, "conversation_id", conversation) || !cJSON_AddStringToObject(payload, "user_id", user) ||
            !number(payload, "server_order", order) || !state_pair(server, conversation, "read", payload))
            goto rollback;
    }
    *output = cJSON_CreateObject();
    if (!*output || !cJSON_AddBoolToObject(*output, "read", 1) || !cJSON_AddBoolToObject(*output, "changed", changed)) {
        error = DROP_E_IO;
        goto rollback;
    }
    if (!sql(server->database, "COMMIT")) goto rollback;
    error = DROP_OK;
    goto done;
rollback:
    sql(server->database, "ROLLBACK");
done:
    cJSON_Delete(payload);
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    if (error != DROP_OK) {
        cJSON_Delete(*output);
        *output = NULL;
    }
    return error;
}
/* 按账号和状态游标分页返回保存及已读变化，限制页大小并报告后续位置。 */
drop_error drop_states_read(drop_server *server, const char *user, const cJSON *input, cJSON **output) {
    const char *generation = drop_json_string(input, "generation", 32);
    uint64_t after;
    sqlite3_int64 highest = 0, next;
    sqlite3_stmt *s = NULL;
    cJSON *events;
    drop_error error = DROP_E_DATABASE;
    int step;
    size_t budget = 256;
    if (!generation || strcmp(generation, server->keys.generation)) return DROP_E_CONFLICT;
    if (!drop_u64_parse(drop_json_string(input, "after", 20), &after) || after > INT64_MAX) return DROP_E_ARGUMENT;
    next = (sqlite3_int64)after;
    EnterCriticalSection(&server->database_lock);
    if (!prepare(server->database, &s, "SELECT last_seq FROM state_sequences WHERE user_id=?1")) goto done;
    bind(s, 1, user);
    if (sqlite3_step(s) == SQLITE_ROW) highest = sqlite3_column_int64(s, 0);
    if (next > highest) {
        error = DROP_E_CONFLICT;
        goto done;
    }
    *output = cJSON_CreateObject();
    events = *output ? cJSON_AddArrayToObject(*output, "events") : NULL;
    if (!events || !cJSON_AddStringToObject(*output, "server_id", server->keys.server_id) || !cJSON_AddStringToObject(*output, "generation", generation)) {
        error = DROP_E_IO;
        goto done;
    }
    if (!prepare(server->database, &s, "SELECT seq,kind,payload FROM state_events WHERE user_id=?1 AND seq>?2 ORDER BY seq LIMIT 50")) goto done;
    bind(s, 1, user);
    sqlite3_bind_int64(s, 2, next);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        cJSON *event = cJSON_CreateObject(), *payload = drop_json_parse((const char *)sqlite3_column_text(s, 2), (size_t)sqlite3_column_bytes(s, 2));
        char *encoded;
        sqlite3_int64 seq = sqlite3_column_int64(s, 0);
        if (!event || !payload || seq != next + 1 || !number(event, "seq", seq) || !cJSON_AddStringToObject(event, "kind", (const char *)sqlite3_column_text(s, 1))) {
            cJSON_Delete(event);
            cJSON_Delete(payload);
            goto done;
        }
        if (!cJSON_AddItemToObject(event, "data", payload)) {
            cJSON_Delete(payload);
            cJSON_Delete(event);
            goto done;
        }
        encoded = cJSON_PrintUnformatted(event);
        if (!encoded) {
            cJSON_Delete(event);
            error = DROP_E_IO;
            goto done;
        }
        budget += strlen(encoded) + 1;
        cJSON_free(encoded);
        if (budget >= DROP_MAX_JSON) {
            cJSON_Delete(event);
            break;
        }
        if (!cJSON_AddItemToArray(events, event)) {
            cJSON_Delete(event);
            error = DROP_E_IO;
            goto done;
        }
        next = seq;
    }
    if (step != SQLITE_ROW && step != SQLITE_DONE) goto done;
    if (!number(*output, "next", next) || !cJSON_AddBoolToObject(*output, "more", next < highest)) {
        error = DROP_E_IO;
        goto done;
    }
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

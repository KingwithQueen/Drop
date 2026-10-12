/* 本机设备保存确认及账号已读同步；状态位置仅在完整保存或服务端确认后推进。 */
#include "client.h"
#include "object.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 执行本机状态 SQL，事务提交或回滚由调用者负责。 */
static int sql(sqlite3 *db, const char *text) {
    return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK;
}
/* 先结束并清空旧语句，再准备本机状态查询；首次调用时 *s 必须为空，成功语句仍由调用者 sqlite3_finalize。 */
static int prepare(sqlite3 *db, sqlite3_stmt **s, const char *text) {
    sqlite3_finalize(*s);
    *s = NULL;
    return sqlite3_prepare_v2(db, text, -1, s, NULL) == SQLITE_OK;
}
/* 复制绑定文本，避免异步任务结束后语句仍引用原缓冲。 */
static void bind(sqlite3_stmt *s, int i, const char *text) {
    sqlite3_bind_text(s, i, text, -1, SQLITE_TRANSIENT);
}
/* 发送经过当前传输模式保护的状态请求，并检查服务器业务响应。 */
static drop_error post(drop_client *c, const WCHAR *path, cJSON *input, cJSON **out) {
    DWORD status;
    drop_error error = drop_http_json_ca(c->url, L"POST", path, c->token, input, out, &status, c->ca_file);
    if (error == DROP_OK && status != 200)
        error = status == 401 ? DROP_E_AUTH : status == 409 ? DROP_E_CONFLICT : status == 400 ? DROP_E_ARGUMENT : status == 507 ? DROP_E_SPACE : DROP_E_DATABASE;
    return error;
}
/* 读取本机游标等文本元数据，缺失或超出输出容量返回 0；十进制数值校验由调用者执行。 */
static int meta(drop_client *c, const char *key, char value[32]) {
    sqlite3_stmt *s = NULL;
    int ok = 0;
    if (prepare(c->database, &s, "SELECT value FROM meta WHERE key=?1")) {
        bind(s, 1, key);
        if (sqlite3_step(s) == SQLITE_ROW) ok = strcpy_s(value, 32, (const char *)sqlite3_column_text(s, 0)) == 0;
    }
    sqlite3_finalize(s);
    return ok;
}
/* 更新已有本机文本元数据，供状态同步事务内保存经校验的游标；不自行创建缺失键。 */
static int set_meta(drop_client *c, const char *key, const char *value) {
    sqlite3_stmt *s = NULL;
    int ok = 0;
    if (prepare(c->database, &s, "UPDATE meta SET value=?1 WHERE key=?2")) {
        bind(s, 1, value);
        bind(s, 2, key);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    return ok;
}
/* 升级本机保存确认、已读和顺序元数据结构；保留旧正文、附件及通知去重记录。 */
int drop_client_receipts_schema(drop_client *c) {
    sqlite3_stmt *s = NULL;
    int version = -1;
    if (prepare(c->database, &s, "PRAGMA user_version") && sqlite3_step(s) == SQLITE_ROW) version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (version == 4 || version == 5) return 1;
    if (version != 3 || !drop_client_snapshot(c, 3)) return 0;
    /* 重放只补充顺序元数据，保留已有正文、文件及通知去重状态。 */
    if (!sql(c->database,
            "BEGIN IMMEDIATE;"
            "CREATE TABLE message_orders(message_id TEXT PRIMARY KEY REFERENCES messages(message_id),server_order INTEGER NOT NULL,generation TEXT NOT NULL);"
            "CREATE TABLE device_states(conversation_id TEXT NOT NULL,user_id TEXT NOT NULL,device_id TEXT NOT NULL,body_order INTEGER NOT NULL,PRIMARY "
            "KEY(conversation_id,user_id,device_id));"
            "CREATE TABLE object_states(object_id TEXT NOT NULL,user_id TEXT NOT NULL,device_id TEXT NOT NULL,PRIMARY KEY(object_id,user_id,device_id));"
            "CREATE TABLE account_reads(conversation_id TEXT NOT NULL,user_id TEXT NOT NULL,read_order INTEGER NOT NULL,PRIMARY KEY(conversation_id,user_id));"
            "CREATE TABLE pending_reads(conversation_id TEXT PRIMARY KEY,message_id TEXT NOT NULL,server_order INTEGER NOT NULL);"
            "ALTER TABLE downloads ADD COLUMN saved_ack INTEGER NOT NULL DEFAULT 0;"
            "INSERT INTO meta VALUES('state_cursor','0'),('saved_ack_cursor','0'); UPDATE meta SET value='0' WHERE key='cursor';"
            "PRAGMA user_version=4; COMMIT;")) {
        sql(c->database, "ROLLBACK");
        return 0;
    }
    return 1;
}
/* 保存服务端消息顺序，用于已读位置和状态关联；重放不覆盖本机正文及文件状态。 */
drop_error drop_client_message_order(drop_client *c, const cJSON *message) {
    const char *id = drop_json_string(message, "message_id", 32);
    uint64_t order;
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_PROTOCOL;
    if (!drop_id_valid(id) || !drop_u64_parse(drop_json_string(message, "server_order", 20), &order) || !order || order > INT64_MAX) return DROP_E_PROTOCOL;
    if (!prepare(c->database, &s,
            "INSERT INTO message_orders VALUES(?1,?2,?3) ON CONFLICT(message_id) DO UPDATE SET server_order=excluded.server_order,generation=excluded.generation WHERE "
            "generation!=excluded.generation OR server_order=excluded.server_order")) {
        error = drop_client_database_error(c);
        goto done;
    }
    bind(s, 1, id);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)order);
    bind(s, 3, c->generation);
    if (sqlite3_step(s) != SQLITE_DONE)
        error = drop_client_database_error(c);
    else
        error = sqlite3_changes(c->database) == 1 ? DROP_OK : DROP_E_PROTOCOL;
done:
    sqlite3_finalize(s);
    return error;
}
/* 将消息对应的账号已读位置加入本机持久队列，网络恢复后再提交。 */
int drop_client_read_queue(drop_client *c, const char *id) {
    sqlite3_stmt *s = NULL;
    int ok = 0;
    if (!drop_id_valid(id) || !sql(c->database, "BEGIN IMMEDIATE")) return 0;
    if (!prepare(c->database, &s,
            "INSERT INTO pending_reads SELECT conversation_id,message_id,server_order FROM messages JOIN message_orders USING(message_id) WHERE message_id=?1 ON "
            "CONFLICT(conversation_id) DO UPDATE SET message_id=excluded.message_id,server_order=excluded.server_order WHERE excluded.server_order>server_order"))
        goto done;
    bind(s, 1, id);
    if (sqlite3_step(s) != SQLITE_DONE) goto done;
    if (!prepare(c->database, &s,
            "UPDATE ui_messages SET unread=0 WHERE message_id=?1 OR message_id IN(SELECT m.message_id FROM messages m JOIN message_orders o USING(message_id) JOIN pending_reads p "
            "USING(conversation_id) WHERE o.server_order<=p.server_order)"))
        goto done;
    bind(s, 1, id);
    if (sqlite3_step(s) != SQLITE_DONE) goto done;
    sqlite3_finalize(s);
    s = NULL;
    ok = sql(c->database, "COMMIT");
done:
    sqlite3_finalize(s);
    if (!ok) sql(c->database, "ROLLBACK");
    return ok;
}
/* 核对本机缓存附件的长度及 SHA-256，只有完整且校验通过的文件可以发送保存确认。 */
static int cache_valid(drop_client *c, const char *id, const char *sha, sqlite3_int64 total) {
    WCHAR wide[DROP_ID_CAP], name[80], path[32768];
    unsigned char expected[32], actual[32];
    uint64_t size;
    return total >= 0 && drop_id_valid(id) && drop_hash_decode(sha, expected) && drop_to_wide(id, wide, DROP_ID_CAP) && swprintf_s(name, 80, L"%s.file", wide) > 0 &&
        drop_path_join(c->files, name, path, 32768) && drop_hash_file(path, actual, &size) && size == (uint64_t)total && sodium_memcmp(expected, actual, 32) == 0;
}
/* 批量提交已保存正文或附件的位置；响应成功后才推进本机确认状态，丢失响应可重复提交。 */
static drop_error acknowledge(drop_client *c, int files) {
    char cursor[32], confirmed[32];
    uint64_t after, previous;
    int again = 0;
    if (!meta(c, "cursor", cursor) || !meta(c, "saved_ack_cursor", confirmed) || !drop_u64_parse(cursor, &after) || !drop_u64_parse(confirmed, &previous)) return DROP_E_DATABASE;
    do {
        cJSON *input = cJSON_CreateObject(), *array, *out = NULL;
        sqlite3_stmt *s = NULL;
        char ids[DROP_MAX_ATTACHMENTS][DROP_ID_CAP];
        int count = 0, i;
        drop_error error = DROP_E_DATABASE;
        array = input ? cJSON_AddArrayToObject(input, "objects") : NULL;
        if (!array || !cJSON_AddStringToObject(input, "generation", c->generation) || !cJSON_AddStringToObject(input, "body_after", cursor)) {
            error = DROP_E_IO;
            goto done;
        }
        if (files) {
            if (!prepare(c->database, &s,
                    "SELECT object_id,sha256,total FROM downloads d WHERE state='complete' AND saved_ack=0 AND EXISTS(SELECT 1 FROM message_orders o WHERE "
                    "o.message_id=d.message_id) ORDER BY rowid LIMIT 32"))
                goto done;
            while (sqlite3_step(s) == SQLITE_ROW) {
                const char *id = (const char *)sqlite3_column_text(s, 0), *sha = (const char *)sqlite3_column_text(s, 1);
                cJSON *item;
                if (!cache_valid(c, id, sha, sqlite3_column_int64(s, 2))) {
                    error = DROP_E_CRYPTO;
                    goto done;
                }
                item = cJSON_CreateObject();
                if (!item || !cJSON_AddStringToObject(item, "object_id", id) || !cJSON_AddStringToObject(item, "sha256", sha) || !cJSON_AddItemToArray(array, item)) {
                    cJSON_Delete(item);
                    error = DROP_E_IO;
                    goto done;
                }
                strcpy_s(ids[count++], DROP_ID_CAP, id);
            }
            sqlite3_finalize(s);
            s = NULL;
        }
        again = count == DROP_MAX_ATTACHMENTS;
        if (after == previous && !count) {
            error = DROP_OK;
            goto done;
        }
        error = post(c, L"/api/v1/saved", input, &out);
        if (error != DROP_OK) goto done;
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "saved"))) {
            error = DROP_E_PROTOCOL;
            goto done;
        }
        error = DROP_E_DATABASE;
        if (!sql(c->database, "BEGIN IMMEDIATE")) goto done;
        if (!set_meta(c, "saved_ack_cursor", cursor) || !prepare(c->database, &s, "UPDATE downloads SET saved_ack=1 WHERE object_id=?1")) goto rollback;
        for (i = 0; i < count; ++i) {
            sqlite3_reset(s);
            bind(s, 1, ids[i]);
            if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
        }
        sqlite3_finalize(s);
        s = NULL;
        if (!sql(c->database, "COMMIT")) goto rollback;
        previous = after;
        error = DROP_OK;
        goto done;
    rollback:
        if (sqlite3_errcode(c->database) == SQLITE_FULL) error = DROP_E_SPACE;
        sql(c->database, "ROLLBACK");
    done:
        sqlite3_finalize(s);
        cJSON_Delete(input);
        cJSON_Delete(out);
        if (error != DROP_OK) return error;
    } while (again);
    return DROP_OK;
}
/* 发送持久已读队列，成功后清除已提交位置，失败保留以便重试。 */
static drop_error send_reads(drop_client *c) {
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_DATABASE;
    int step;
    if (!prepare(c->database, &s, "SELECT conversation_id,message_id FROM pending_reads")) return error;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        cJSON *input = cJSON_CreateObject(), *out = NULL;
        sqlite3_stmt *remove = NULL;
        const char *conversation = (const char *)sqlite3_column_text(s, 0);
        if (!input || !cJSON_AddStringToObject(input, "generation", c->generation) || !cJSON_AddStringToObject(input, "conversation_id", conversation) ||
            !cJSON_AddStringToObject(input, "message_id", (const char *)sqlite3_column_text(s, 1)))
            error = DROP_E_IO;
        else {
            error = post(c, L"/api/v1/read", input, &out);
            if (error == DROP_OK && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "read"))) error = DROP_E_PROTOCOL;
            if (error == DROP_OK) {
                if (!prepare(c->database, &remove, "DELETE FROM pending_reads WHERE conversation_id=?1"))
                    error = DROP_E_DATABASE;
                else {
                    bind(remove, 1, conversation);
                    if (sqlite3_step(remove) != SQLITE_DONE) error = DROP_E_DATABASE;
                }
            }
        }
        sqlite3_finalize(remove);
        cJSON_Delete(input);
        cJSON_Delete(out);
        if (error != DROP_OK) goto done;
    }
    error = step == SQLITE_DONE ? DROP_OK : DROP_E_DATABASE;
done:
    sqlite3_finalize(s);
    return error;
}
/* 在本机事务中保存单个设备或账号状态事件，拒绝不符合类型及范围的服务端数据。 */
static int state_save(drop_client *c, const cJSON *event) {
    const char *kind = drop_json_string(event, "kind", 32);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(event, "data");
    const char *conversation = drop_json_string(data, "conversation_id", 32), *user = drop_json_string(data, "user_id", 32), *device, *object;
    uint64_t order;
    sqlite3_stmt *s = NULL;
    int ok = 0;
    if (!kind || !drop_id_valid(conversation) || !drop_id_valid(user)) return 0;
    if (!strcmp(kind, "read")) {
        if (!drop_u64_parse(drop_json_string(data, "server_order", 20), &order) || !order || order > INT64_MAX ||
            !prepare(
                c->database, &s, "INSERT INTO account_reads VALUES(?1,?2,?3) ON CONFLICT(conversation_id,user_id) DO UPDATE SET read_order=max(read_order,excluded.read_order)"))
            goto done;
        bind(s, 1, conversation);
        bind(s, 2, user);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)order);
    } else if (!strcmp(kind, "body_saved")) {
        device = drop_json_string(data, "device_id", 32);
        if (!drop_id_valid(device) || !drop_u64_parse(drop_json_string(data, "server_order", 20), &order) || !order || order > INT64_MAX ||
            !prepare(c->database, &s,
                "INSERT INTO device_states VALUES(?1,?2,?3,?4) ON CONFLICT(conversation_id,user_id,device_id) DO UPDATE SET body_order=max(body_order,excluded.body_order)"))
            goto done;
        bind(s, 1, conversation);
        bind(s, 2, user);
        bind(s, 3, device);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)order);
    } else if (!strcmp(kind, "object_saved")) {
        device = drop_json_string(data, "device_id", 32);
        object = drop_json_string(data, "object_id", 32);
        if (!drop_id_valid(device) || !drop_id_valid(object) || !prepare(c->database, &s, "INSERT INTO object_states VALUES(?1,?2,?3) ON CONFLICT DO NOTHING")) goto done;
        bind(s, 1, object);
        bind(s, 2, user);
        bind(s, 3, device);
    } else
        goto done;
    ok = sqlite3_step(s) == SQLITE_DONE;
done:
    sqlite3_finalize(s);
    return ok;
}
/* 分页拉取保存和已读事件，在同一事务中应用状态并推进游标；整页失败时回滚。 */
static drop_error pull_states(drop_client *c) {
    char cursor[32];
    int pages = 0, more = 1;
    drop_error error = DROP_E_DATABASE;
    if (!meta(c, "state_cursor", cursor)) return error;
    while (more && ++pages <= 10000) {
        cJSON *input = cJSON_CreateObject(), *out = NULL, *events, *event, *flag;
        const char *next;
        uint64_t expected, received;
        if (!input || !cJSON_AddStringToObject(input, "generation", c->generation) || !cJSON_AddStringToObject(input, "after", cursor)) {
            cJSON_Delete(input);
            return DROP_E_IO;
        }
        error = post(c, L"/api/v1/states", input, &out);
        cJSON_Delete(input);
        if (error != DROP_OK) {
            cJSON_Delete(out);
            return error;
        }
        events = cJSON_GetObjectItemCaseSensitive(out, "events");
        flag = cJSON_GetObjectItemCaseSensitive(out, "more");
        next = drop_json_string(out, "next", 20);
        if (!cJSON_IsArray(events) || !cJSON_IsBool(flag) || !drop_u64_parse(cursor, &expected) || !drop_u64_parse(next, &received) || received > INT64_MAX ||
            strcmp(drop_json_string(out, "server_id", 32) ? drop_json_string(out, "server_id", 32) : "", c->server) ||
            strcmp(drop_json_string(out, "generation", 32) ? drop_json_string(out, "generation", 32) : "", c->generation)) {
            cJSON_Delete(out);
            return DROP_E_PROTOCOL;
        }
        error = DROP_E_DATABASE;
        if (!sql(c->database, "BEGIN IMMEDIATE")) {
            cJSON_Delete(out);
            return error;
        }
        cJSON_ArrayForEach(event, events) {
            uint64_t seq;
            if (!drop_u64_parse(drop_json_string(event, "seq", 20), &seq) || seq != expected + 1 || !state_save(c, event)) {
                error = DROP_E_PROTOCOL;
                goto rollback;
            }
            expected = seq;
        }
        more = cJSON_IsTrue(flag);
        if (expected != received || (more && !cJSON_GetArraySize(events))) {
            error = DROP_E_PROTOCOL;
            goto rollback;
        }
        if (!set_meta(c, "state_cursor", next) || !sql(c->database, "COMMIT")) goto rollback;
        strcpy_s(cursor, sizeof(cursor), next);
        cJSON_Delete(out);
        continue;
    rollback:
        if (sqlite3_errcode(c->database) == SQLITE_FULL) error = DROP_E_SPACE;
        sql(c->database, "ROLLBACK");
        cJSON_Delete(out);
        return error;
    }
    return more ? DROP_E_LIMIT : DROP_OK;
}
/* 阅读反馈单独处理，避免等待大附件上传、下载或保存确认中的文件散列。 */
drop_error drop_client_read_receipts(drop_client *c) {
    drop_error deleted = drop_client_deletions(c);
    if (deleted != DROP_OK) return deleted;
    drop_error read = send_reads(c), state;
    if (read == DROP_E_AUTH || read == DROP_E_CONFLICT || read == DROP_E_PROTOCOL) return read;
    state = pull_states(c);
    return read != DROP_OK ? read : state;
}
/* 依次处理正文确认、可选附件确认、已读提交及状态增量，失败保留未完成步骤。 */
drop_error drop_client_receipts(drop_client *c, int files) {
    drop_error deleted = drop_client_deletions(c);
    if (deleted != DROP_OK) return deleted;
    drop_error saved = acknowledge(c, files), read, state;
    if (saved == DROP_E_AUTH || saved == DROP_E_CONFLICT || saved == DROP_E_PROTOCOL) return saved;
    read = send_reads(c);
    state = pull_states(c);
    if (saved != DROP_OK) return saved;
    if (read != DROP_OK) return read;
    return state;
}

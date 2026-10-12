/* 本机删除意图、独立增量和缓存回收；数据库事务提交后才能删除文件，所有重试使用稳定消息 ID。 */
#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sql(sqlite3 *db, const char *text) {
    return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK;
}
/* 复用语句时先结束旧查询，绑定临时文本时复制内容；所有调用都在单一同步核心执行。 */
static int prepare(sqlite3 *db, sqlite3_stmt **s, const char *text) {
    sqlite3_finalize(*s);
    *s = NULL;
    return sqlite3_prepare_v2(db, text, -1, s, NULL) == SQLITE_OK;
}
static void bind(sqlite3_stmt *s, int index, const char *text) {
    sqlite3_bind_text(s, index, text, -1, SQLITE_TRANSIENT);
}
/* 客户端结构升级先建立回退快照；删除身份无需正文外键，允许删除增量先于原消息到达。 */
int drop_client_deletions_schema(drop_client *c) {
    sqlite3_stmt *s = NULL;
    int version = -1;
    if (prepare(c->database, &s, "PRAGMA user_version") && sqlite3_step(s) == SQLITE_ROW) version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (version == 5) return 1;
    if (version != 4 || !drop_client_snapshot(c, 4)) return 0;
    if (!sql(c->database,
            "BEGIN IMMEDIATE;"
            "CREATE TABLE message_deletions(message_id TEXT PRIMARY KEY,request_id TEXT NOT NULL,conversation_id TEXT NOT NULL,sender TEXT NOT NULL,recipient TEXT NOT NULL,"
            "device TEXT NOT NULL,created_at INTEGER NOT NULL,server_order INTEGER NOT NULL,deleted_at INTEGER NOT NULL);"
            "CREATE TABLE pending_deletes(message_id TEXT PRIMARY KEY,generation TEXT NOT NULL,error TEXT NOT NULL DEFAULT '');"
            "CREATE TABLE cache_cleanup(name TEXT PRIMARY KEY); INSERT INTO meta VALUES('deletion_cursor','0'); PRAGMA user_version=5; COMMIT;")) {
        sql(c->database, "ROLLBACK");
        return 0;
    }
    return 1;
}
/* 返回身份字段和有范围约束的时间顺序，消息必须涉及当前账号；不信任远端事件中的任意路径。 */
static int identity(drop_client *c, const cJSON *message, const char *ids[6], uint64_t *created, uint64_t *order) {
    static const char *fields[] = {"message_id", "request_id", "conversation_id", "sender_user_id", "recipient_user_id", "sender_device_id"};
    for (int i = 0; i < 6; ++i) {
        ids[i] = drop_json_string(message, fields[i], 32);
        if (!drop_id_valid(ids[i])) return 0;
    }
    return (!strcmp(ids[3], c->user) || !strcmp(ids[4], c->user)) && drop_u64_parse(drop_json_string(message, "created_at", 20), created) && *created <= INT64_MAX &&
        drop_u64_parse(drop_json_string(message, "server_order", 20), order) && *order && *order <= INT64_MAX;
}
/* 已删除身份的重放只能匹配原请求、参与者、设备和时间；即使旧服务器快照带回正文，也不能重新显示。 */
int drop_client_deleted_known(drop_client *c, const cJSON *message) {
    const char *ids[6];
    uint64_t created, order;
    sqlite3_stmt *s = NULL;
    int known = -1, step;
    if (!identity(c, message, ids, &created, &order) ||
        !prepare(c->database, &s, "SELECT request_id,conversation_id,sender,recipient,device,created_at,server_order FROM message_deletions WHERE message_id=?1"))
        goto done;
    bind(s, 1, ids[0]);
    step = sqlite3_step(s);
    if (step == SQLITE_DONE) {
        known = 0;
        goto done;
    }
    if (step != SQLITE_ROW) goto done;
    for (int i = 1; i < 6; ++i)
        if (strcmp(ids[i], (const char *)sqlite3_column_text(s, i - 1))) goto done;
    if (sqlite3_column_int64(s, 5) == (sqlite3_int64)created && sqlite3_column_int64(s, 6) == (sqlite3_int64)order) known = 1;
done:
    sqlite3_finalize(s);
    return known;
}
/* 在外层事务中应用删除，先保存受限缓存名，再清除正文及发送/下载任务；文件操作留到提交后。 */
drop_error drop_client_deleted_save(drop_client *c, const cJSON *message) {
    const char *ids[6];
    uint64_t created, order, deleted;
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_PROTOCOL;
    int step;
    if (!identity(c, message, ids, &created, &order) || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "deleted")) ||
        !drop_u64_parse(drop_json_string(message, "deleted_at", 20), &deleted) || !deleted || deleted > INT64_MAX || drop_client_deleted_known(c, message) < 0)
        return DROP_E_PROTOCOL;
    if (!prepare(c->database, &s, "SELECT request_id,conversation_id,sender,recipient,device,created_at FROM messages WHERE message_id=?1")) goto database_error;
    bind(s, 1, ids[0]);
    step = sqlite3_step(s);
    if (step == SQLITE_ROW) {
        for (int i = 1; i < 6; ++i)
            if (strcmp(ids[i], (const char *)sqlite3_column_text(s, i - 1))) goto done;
        if (sqlite3_column_int64(s, 5) != (sqlite3_int64)created) goto done;
    } else if (step != SQLITE_DONE)
        goto database_error;
    if (!prepare(c->database, &s,
            "INSERT INTO message_deletions VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) ON CONFLICT(message_id) DO UPDATE SET deleted_at=max(deleted_at,excluded.deleted_at)"))
        goto database_error;
    for (int i = 0; i < 6; ++i)
        bind(s, i + 1, ids[i]);
    sqlite3_bind_int64(s, 7, (sqlite3_int64)created);
    sqlite3_bind_int64(s, 8, (sqlite3_int64)order);
    sqlite3_bind_int64(s, 9, (sqlite3_int64)deleted);
    if (sqlite3_step(s) != SQLITE_DONE) goto database_error;
    static const char *changes[] = {
        "INSERT OR IGNORE INTO cache_cleanup SELECT object_id||'.file' FROM downloads WHERE message_id=?1 UNION SELECT object_id||'.part' FROM downloads WHERE message_id=?1",
        "INSERT OR IGNORE INTO cache_cleanup SELECT local_id||'.source' FROM pending_files WHERE request_id=?2 AND ?3=?4 UNION SELECT object_id||'.file' FROM pending_files WHERE "
        "request_id=?2 AND ?3=?4 AND object_id IS NOT NULL AND object_id!=''",
        "DELETE FROM object_states WHERE object_id IN(SELECT object_id FROM downloads WHERE message_id=?1)", "DELETE FROM downloads WHERE message_id=?1",
        "DELETE FROM pending_files WHERE request_id=?2 AND ?3=?4", "DELETE FROM pending WHERE request_id=?2 AND ?3=?4", "DELETE FROM pending_reads WHERE message_id=?1",
        "UPDATE messages SET body=X'',compressed=0,original_length=0 WHERE message_id=?1", "UPDATE ui_messages SET unread=0,notified=1 WHERE message_id=?1",
        "DELETE FROM pending_deletes WHERE message_id=?1"};
    for (int i = 0; i < (int)(sizeof(changes) / sizeof(*changes)); ++i) {
        if (!prepare(c->database, &s, changes[i])) goto database_error;
        bind(s, 1, ids[0]);
        if (sqlite3_bind_parameter_count(s) >= 2) bind(s, 2, ids[1]);
        if (sqlite3_bind_parameter_count(s) >= 4) {
            bind(s, 3, ids[3]);
            bind(s, 4, c->user);
        }
        if (sqlite3_step(s) != SQLITE_DONE) goto database_error;
    }
    error = DROP_OK;
    goto done;
database_error:
    error = drop_client_database_error(c);
done:
    sqlite3_finalize(s);
    return error;
}
/* 发送设备不参与判断：当前账号拥有且已核对服务器顺序的消息可以排队删除，重复点击保持幂等。 */
drop_error drop_client_delete_queue(drop_client *c, const char *id) {
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_DATABASE;
    if (!drop_id_valid(id)) return DROP_E_ARGUMENT;
    if (!c->deletion_supported) return DROP_E_UNSUPPORTED;
    if (!prepare(c->database, &s, "SELECT sender,EXISTS(SELECT 1 FROM message_orders o WHERE o.message_id=m.message_id AND o.generation=?2) FROM messages m WHERE message_id=?1"))
        goto done;
    bind(s, 1, id);
    bind(s, 2, c->generation);
    if (sqlite3_step(s) != SQLITE_ROW) {
        error = DROP_E_ARGUMENT;
        goto done;
    }
    if (strcmp((const char *)sqlite3_column_text(s, 0), c->user)) {
        error = DROP_E_AUTH;
        goto done;
    }
    if (!sqlite3_column_int(s, 1)) {
        error = DROP_E_CONFLICT;
        goto done;
    }
    if (!sql(c->database, "BEGIN IMMEDIATE") || !prepare(c->database, &s, "INSERT OR IGNORE INTO pending_deletes(message_id,generation) VALUES(?1,?2)")) goto rollback;
    bind(s, 1, id);
    bind(s, 2, c->generation);
    if (sqlite3_step(s) != SQLITE_DONE || !prepare(c->database, &s, "UPDATE ui_messages SET unread=0,notified=1 WHERE message_id=?1")) goto rollback;
    bind(s, 1, id);
    if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
    sqlite3_finalize(s);
    s = NULL;
    if (!sql(c->database, "COMMIT")) goto rollback;
    c->more_deletions = 1;
    error = DROP_OK;
    goto done;
rollback:
    error = drop_client_database_error(c);
    sql(c->database, "ROLLBACK");
done:
    sqlite3_finalize(s);
    return error;
}
/* 删除扩展使用当前 TLS 或 PAKE 通道；404 仅表示旧服务器不支持，不清除其他同步数据。 */
static drop_error post(drop_client *c, const WCHAR *path, cJSON *input, cJSON **out) {
    DWORD status = 0;
    drop_error error = drop_http_json_ca(c->url, L"POST", path, c->token, input, out, &status, c->ca_file);
    if (error == DROP_OK && status != 200)
        error = status == 401 ? DROP_E_AUTH
            : status == 409   ? DROP_E_CONFLICT
            : status == 400   ? DROP_E_ARGUMENT
            : status == 404   ? DROP_E_UNSUPPORTED
            : status == 507   ? DROP_E_SPACE
                              : DROP_E_DATABASE;
    return error;
}
/* 同一附件的损坏隔离副本也由缓存拥有；仅扫描固定随机编号前缀，拒绝未知名称、目录或重解析点。 */
static int remove_corrupt(drop_client *c, const char *id) {
    WCHAR wide[DROP_ID_CAP], name[96], pattern[32768], path[32768];
    WIN32_FIND_DATAW item;
    unsigned count = 0;
    int ok = 1;
    if (!drop_to_wide(id, wide, DROP_ID_CAP) || swprintf_s(name, 96, L"%s.corrupt.*", wide) < 0 || !drop_path_join(c->files, name, pattern, 32768)) return 0;
    HANDLE find = FindFirstFileW(pattern, &item);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        char suffix[DROP_ID_CAP];
        if (++count > 64) {
            ok = 0;
            break;
        }
        if (wcslen(item.cFileName) != 73 || wcsncmp(item.cFileName, wide, 32) || wcsncmp(item.cFileName + 32, L".corrupt.", 9) ||
            !drop_to_utf8(item.cFileName + 41, suffix, DROP_ID_CAP) || !drop_id_valid(suffix) ||
            (item.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !drop_path_join(c->files, item.cFileName, path, 32768) || !DeleteFileW(path))
            ok = 0;
    } while (FindNextFileW(find, &item));
    FindClose(find);
    return ok;
}
/* 原有缓存目录与文件均不允许重解析点；只删除数据库保存的随机 ID 名，导出位置与原始文件不参与。 */
static int remove_cache(drop_client *c, const char *name) {
    char id[DROP_ID_CAP];
    WCHAR wide[96], path[32768];
    size_t length = strlen(name);
    if (length < 37 || length > 40) return 0;
    memcpy(id, name, 32);
    id[32] = 0;
    if (!drop_id_valid(id) || (strcmp(name + 32, ".file") && strcmp(name + 32, ".part") && strcmp(name + 32, ".source"))) return 0;
    DWORD root = GetFileAttributesW(c->files);
    if (root == INVALID_FILE_ATTRIBUTES || !(root & FILE_ATTRIBUTE_DIRECTORY) || (root & FILE_ATTRIBUTE_REPARSE_POINT) || !drop_to_wide(name, wide, 96) ||
        !drop_path_join(c->files, wide, path, 32768))
        return 0;
    DWORD attributes = GetFileAttributesW(path);
    int removed = attributes == INVALID_FILE_ATTRIBUTES ? (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)
                                                        : (!(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && DeleteFileW(path));
    if (!strcmp(name + 32, ".file")) return remove_corrupt(c, id) && removed;
    return removed;
}
/* 分批回收已经提交删除的内部缓存；文件被占用时保留队列，下轮继续，不把失败文件标记为已回收。 */
static void cleanup(drop_client *c) {
    sqlite3_stmt *s = NULL, *remove = NULL;
    if (!prepare(c->database, &s, "SELECT name FROM cache_cleanup LIMIT 64") || !prepare(c->database, &remove, "DELETE FROM cache_cleanup WHERE name=?1")) goto done;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(s, 0);
        if (!remove_cache(c, name)) continue;
        sqlite3_reset(remove);
        bind(remove, 1, name);
        if (sqlite3_step(remove) != SQLITE_DONE) break;
    }
done:
    sqlite3_finalize(s);
    sqlite3_finalize(remove);
}
/* 每轮处理有限删除任务，服务端成功后在本机事务接管删除标记；丢失响应时保留意图供原 ID 重试。 */
static drop_error transmit(drop_client *c) {
    char ids[32][DROP_ID_CAP], generations[32][DROP_ID_CAP];
    sqlite3_stmt *s = NULL;
    int count = 0, step;
    drop_error error = DROP_E_DATABASE;
    if (!prepare(c->database, &s, "SELECT message_id,generation FROM pending_deletes WHERE error!='invalid_argument' ORDER BY rowid LIMIT 32")) goto done;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        if (strcpy_s(ids[count], DROP_ID_CAP, (const char *)sqlite3_column_text(s, 0)) || strcpy_s(generations[count], DROP_ID_CAP, (const char *)sqlite3_column_text(s, 1)))
            goto done;
        ++count;
    }
    if (step != SQLITE_DONE) goto done;
    sqlite3_finalize(s);
    s = NULL;
    for (int i = 0; i < count; ++i) {
        cJSON *input = cJSON_CreateObject(), *out = NULL;
        if (!input || !cJSON_AddStringToObject(input, "message_id", ids[i]) || !cJSON_AddStringToObject(input, "generation", generations[i])) {
            cJSON_Delete(input);
            error = DROP_E_IO;
            goto done;
        }
        error = post(c, L"/api/v1/messages/delete", input, &out);
        cJSON_Delete(input);
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(out, "message");
        const char *id = drop_json_string(message, "message_id", 32);
        if (error == DROP_OK && (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "deleted")) || !id || strcmp(id, ids[i]))) error = DROP_E_PROTOCOL;
        if (error == DROP_OK) {
            if (!sql(c->database, "BEGIN IMMEDIATE"))
                error = drop_client_database_error(c);
            else {
                error = drop_client_deleted_save(c, message);
                if (error == DROP_OK && !sql(c->database, "COMMIT")) error = drop_client_database_error(c);
                if (error != DROP_OK) sql(c->database, "ROLLBACK");
            }
        }
        cJSON_Delete(out);
        if (error != DROP_OK) {
            if (!prepare(c->database, &s, "UPDATE pending_deletes SET error=?1 WHERE message_id=?2")) {
                error = drop_client_database_error(c);
                goto done;
            }
            bind(s, 1, drop_error_name(error));
            bind(s, 2, ids[i]);
            if (sqlite3_step(s) != SQLITE_DONE) {
                error = drop_client_database_error(c);
                goto done;
            }
            /* 恢复快照可能已不含此消息；保留删除意图供核对，但不能阻塞后面其他有效删除。 */
            if (error == DROP_E_ARGUMENT) continue;
            goto done;
        }
    }
    error = DROP_OK;
done:
    sqlite3_finalize(s);
    return error;
}
/* 分页应用独立删除序列，身份、连续性、下一游标和整页提交一并校验；失败页不能推进游标。 */
static drop_error pull(drop_client *c) {
    sqlite3_stmt *s = NULL;
    char cursor[32];
    int more = 1, pages = 0;
    drop_error error = DROP_E_DATABASE;
    if (!prepare(c->database, &s, "SELECT value FROM meta WHERE key='deletion_cursor'") || sqlite3_step(s) != SQLITE_ROW ||
        strcpy_s(cursor, 32, (const char *)sqlite3_column_text(s, 0)))
        goto done;
    sqlite3_finalize(s);
    s = NULL;
    while (more && ++pages <= 10000) {
        cJSON *input = cJSON_CreateObject(), *out = NULL, *event;
        if (!input || !cJSON_AddStringToObject(input, "generation", c->generation) || !cJSON_AddStringToObject(input, "after", cursor)) {
            cJSON_Delete(input);
            error = DROP_E_IO;
            goto done;
        }
        error = post(c, L"/api/v1/deletions", input, &out);
        cJSON_Delete(input);
        if (error != DROP_OK) {
            cJSON_Delete(out);
            goto done;
        }
        const char *next = drop_json_string(out, "next", 20);
        const cJSON *events = cJSON_GetObjectItemCaseSensitive(out, "events"), *flag = cJSON_GetObjectItemCaseSensitive(out, "more");
        uint64_t expected, received;
        if (!cJSON_IsArray(events) || !cJSON_IsBool(flag) || !drop_u64_parse(cursor, &expected) || !drop_u64_parse(next, &received) || received > INT64_MAX ||
            strcmp(drop_json_string(out, "server_id", 32) ? drop_json_string(out, "server_id", 32) : "", c->server) ||
            strcmp(drop_json_string(out, "generation", 32) ? drop_json_string(out, "generation", 32) : "", c->generation)) {
            cJSON_Delete(out);
            error = DROP_E_PROTOCOL;
            goto done;
        }
        if (!sql(c->database, "BEGIN IMMEDIATE")) {
            cJSON_Delete(out);
            error = drop_client_database_error(c);
            goto done;
        }
        cJSON_ArrayForEach(event, events) {
            uint64_t seq;
            if (!drop_u64_parse(drop_json_string(event, "seq", 20), &seq) || seq != expected + 1) {
                error = DROP_E_PROTOCOL;
                goto rollback;
            }
            error = drop_client_deleted_save(c, cJSON_GetObjectItemCaseSensitive(event, "message"));
            if (error != DROP_OK) goto rollback;
            expected = seq;
        }
        more = cJSON_IsTrue(flag);
        if (expected != received || (more && !cJSON_GetArraySize(events))) {
            error = DROP_E_PROTOCOL;
            goto rollback;
        }
        if (!prepare(c->database, &s, "UPDATE meta SET value=?1 WHERE key='deletion_cursor'")) goto rollback;
        bind(s, 1, next);
        if (sqlite3_step(s) != SQLITE_DONE) goto rollback;
        sqlite3_finalize(s);
        s = NULL;
        if (!sql(c->database, "COMMIT")) goto rollback;
        strcpy_s(cursor, 32, next);
        cJSON_Delete(out);
        if (c->work_pending && WaitForSingleObject(c->work_pending, 0) == WAIT_OBJECT_0) break;
        continue;
    rollback:
        if (sqlite3_errcode(c->database) == SQLITE_FULL)
            error = DROP_E_SPACE;
        else if (error == DROP_OK)
            error = DROP_E_DATABASE;
        sql(c->database, "ROLLBACK");
        cJSON_Delete(out);
        goto done;
    }
    c->more_deletions = more;
    error = more && pages > 10000 ? DROP_E_LIMIT : DROP_OK;
done:
    sqlite3_finalize(s);
    return error;
}
/* 删除先于附件确认及下载推进；离线发送错误不阻止尝试补拉已发生的远端删除，清理仅发生在事务之后。 */
drop_error drop_client_deletions(drop_client *c) {
    if (!c->deletion_supported) {
        cleanup(c);
        return DROP_OK;
    }
    drop_error sent = transmit(c), received;
    if (sent == DROP_E_AUTH || sent == DROP_E_CONFLICT || sent == DROP_E_PROTOCOL) return sent;
    received = pull(c);
    cleanup(c);
    sqlite3_stmt *s = NULL;
    if (prepare(c->database, &s, "SELECT (SELECT count(*) FROM pending_deletes WHERE error!='invalid_argument')+(SELECT count(*) FROM cache_cleanup)") &&
        sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0))
        c->more_deletions = 1;
    sqlite3_finalize(s);
    return sent != DROP_OK ? sent : received;
}

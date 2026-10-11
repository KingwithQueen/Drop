/* 账号资料是可重建的界面缓存，独立于消息游标；所有数据库操作均由客户端同步线程执行。 */
#include "client.h"
#include <string.h>

/* 校验完整资料后写入缓存；显示名最多 128 个 UTF-8 字节，删除身份保留名字并停止作为发送目标。 */
drop_error drop_client_profile_save(drop_client *c, const cJSON *profile) {
    const char *id = drop_json_string(profile, "user_id", 32), *login = drop_json_string(profile, "login", 64), *display = drop_json_string(profile, "display_name", 128);
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(profile, "enabled"), *deleted = cJSON_GetObjectItemCaseSensitive(profile, "deleted");
    sqlite3_stmt *s = NULL;
    drop_error error = DROP_E_DATABASE;
    if (!drop_id_valid(id) || !login || !display || !*display || !cJSON_IsBool(enabled) || !cJSON_IsBool(deleted) ||
        (cJSON_IsTrue(deleted) ? (*login || cJSON_IsTrue(enabled)) : !drop_account_login_valid(login)))
        return DROP_E_PROTOCOL;
    if (sqlite3_prepare_v2(c->database,
            "INSERT INTO ui_profiles VALUES(?1,?2,?3,?4,?5) ON CONFLICT(user_id) DO UPDATE SET "
            "login=excluded.login,display_name=excluded.display_name,enabled=excluded.enabled,deleted=excluded.deleted",
            -1, &s, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, login, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, display, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 4, cJSON_IsTrue(enabled));
    sqlite3_bind_int(s, 5, cJSON_IsTrue(deleted));
    error = sqlite3_step(s) == SQLITE_DONE ? DROP_OK : drop_client_database_error(c);
done:
    sqlite3_finalize(s);
    return error;
}

/* 每批查询一百个已知身份；响应只允许包含本批请求的唯一 ID，资料和分页进度在成功提交后一起推进。 */
drop_error drop_client_profiles(drop_client *c) {
    sqlite3_stmt *s = NULL;
    cJSON *input = cJSON_CreateObject(), *output = NULL, *users = NULL, *profiles, *profile;
    char ids[100][DROP_ID_CAP], next[DROP_ID_CAP] = {0};
    int count = 0, step = SQLITE_ERROR, more = 0, transaction = 0, seen[100] = {0};
    DWORD status = 0;
    drop_error error = DROP_E_DATABASE;
    const char *query = "WITH peers(id) AS (SELECT ?1 UNION SELECT sender FROM messages UNION SELECT recipient FROM messages UNION SELECT recipient FROM pending UNION SELECT "
                        "user_id FROM ui_contacts) SELECT id,coalesce((SELECT login FROM ui_contacts WHERE user_id=id),'') FROM peers WHERE id>?2 ORDER BY id LIMIT 101";
    if (!input || !(users = cJSON_AddArrayToObject(input, "users"))) {
        error = DROP_E_IO;
        goto done;
    }
    if (sqlite3_prepare_v2(c->database, query, -1, &s, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(s, 1, c->user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, c->profile_after, -1, SQLITE_TRANSIENT);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(s, 0), *login = (const char *)sqlite3_column_text(s, 1);
        cJSON *entry;
        if (count == 100) {
            more = 1;
            break;
        }
        if (!drop_id_valid(id)) {
            error = DROP_E_PROTOCOL;
            goto done;
        }
        entry = cJSON_CreateObject();
        if (!entry || !cJSON_AddStringToObject(entry, "user_id", id) || !cJSON_AddStringToObject(entry, "login", login) || !cJSON_AddItemToArray(users, entry)) {
            cJSON_Delete(entry);
            error = DROP_E_IO;
            goto done;
        }
        strcpy_s(ids[count++], DROP_ID_CAP, id);
        strcpy_s(next, DROP_ID_CAP, id);
    }
    sqlite3_finalize(s);
    s = NULL;
    if (step != SQLITE_DONE && !more) goto done;
    error = drop_http_json_ca(c->url, L"POST", L"/api/v1/profiles", c->token, input, &output, &status, c->ca_file);
    /* 旧版服务器没有资料接口时继续同步正文，界面保留已有登录名；认证错误仍按原规则处理。 */
    if (status == 404) {
        c->more_profiles = 0;
        error = DROP_OK;
        goto done;
    }
    if (error != DROP_OK) goto done;
    profiles = cJSON_GetObjectItemCaseSensitive(output, "profiles");
    if (!cJSON_IsArray(profiles) || cJSON_GetArraySize(profiles) > count) {
        error = DROP_E_PROTOCOL;
        goto done;
    }
    if (sqlite3_exec(c->database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        error = drop_client_database_error(c);
        goto done;
    }
    transaction = 1;
    cJSON_ArrayForEach(profile, profiles) {
        const char *id = drop_json_string(profile, "user_id", 32);
        int index;
        for (index = 0; index < count; ++index)
            if (id && !strcmp(id, ids[index])) break;
        if (index == count || seen[index]++) {
            error = DROP_E_PROTOCOL;
            goto done;
        }
        error = drop_client_profile_save(c, profile);
        if (error != DROP_OK) goto done;
    }
    if (sqlite3_exec(c->database, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        error = drop_client_database_error(c);
        goto done;
    }
    transaction = 0;
    strcpy_s(c->profile_after, DROP_ID_CAP, more ? next : "");
    c->more_profiles = more;
    error = DROP_OK;
done:
    if (transaction) sqlite3_exec(c->database, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_finalize(s);
    cJSON_Delete(input);
    cJSON_Delete(output);
    return error;
}

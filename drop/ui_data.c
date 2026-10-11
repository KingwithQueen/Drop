/* 从本机数据库生成历史和详情，以及经过完整校验的显式附件导出；由同步工作线程调用。 */
#include "ui_data.h"
#include "object.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 复制绑定查询条件文本，使语句生命周期独立于界面任务。 */
static void bind(sqlite3_stmt *s, int i, const char *text) {
    sqlite3_bind_text(s, i, text, -1, SQLITE_TRANSIENT);
}
/* 执行单值统计查询，供历史列表和状态栏读取本机任务数量。 */
static int scalar(sqlite3 *db, const char *query) {
    sqlite3_stmt *s = NULL;
    int value = 0;
    if (sqlite3_prepare_v2(db, query, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}
/* 建立本机界面和通知去重元数据，迁移时保留已有历史。 */
int drop_client_ui_schema(drop_client *c) {
    int version = scalar(c->database, "PRAGMA user_version");
    /* 可重建的显示名缓存独立于协议和消息 schema，只记录已验证的账号，不保存凭据。 */
    if (sqlite3_exec(c->database, "CREATE TABLE IF NOT EXISTS ui_contacts(user_id TEXT PRIMARY KEY,login TEXT NOT NULL)", NULL, NULL, NULL) != SQLITE_OK) return 0;
    if (sqlite3_exec(c->database,
            "CREATE TABLE IF NOT EXISTS ui_profiles(user_id TEXT PRIMARY KEY,login TEXT NOT NULL,display_name TEXT NOT NULL,enabled INTEGER NOT NULL,deleted INTEGER NOT NULL)",
            NULL, NULL, NULL) != SQLITE_OK)
        return 0;
    if (version == 3 || version == 4) return 1;
    if (version != 2) return 0;
    if (sqlite3_exec(c->database,
            "BEGIN IMMEDIATE; CREATE TABLE ui_messages(message_id TEXT PRIMARY KEY REFERENCES messages(message_id),"
            "unread INTEGER NOT NULL,notified INTEGER NOT NULL); PRAGMA user_version=3; COMMIT;",
            NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(c->database, "ROLLBACK", NULL, NULL, NULL);
        return 0;
    }
    return 1;
}
/* 从本机记录解压正文并转为可展示文本，检查原始长度及编码后才返回内容。 */
static WCHAR *body(sqlite3_stmt *s, int column) {
    drop_text packed;
    char *plain = NULL;
    WCHAR *wide = NULL;
    sqlite3_int64 original = sqlite3_column_int64(s, column + 2);
    packed.data = (unsigned char *)sqlite3_column_blob(s, column);
    packed.size = (size_t)sqlite3_column_bytes(s, column);
    packed.compressed = sqlite3_column_int(s, column + 1);
    packed.original_size = original >= 0 ? (size_t)original : SIZE_MAX;
    if (!drop_text_unpack(&packed, &plain)) return NULL;
    wide = malloc((strlen(plain) + 1) * sizeof(WCHAR));
    if (wide && !drop_to_wide(plain, wide, strlen(plain) + 1)) {
        free(wide);
        wide = NULL;
    }
    free(plain);
    return wide;
}
/* 将消息时间转为本机可读的日期文本，供列表展示。 */
static void time_text(sqlite3_int64 seconds, WCHAR text[32]) {
    ULARGE_INTEGER value;
    FILETIME file;
    SYSTEMTIME utc, local;
    if (seconds < 0 || (uint64_t)seconds > UINT64_C(253402300799)) {
        wcscpy_s(text, 32, L"时间无效");
        return;
    }
    value.QuadPart = ((uint64_t)seconds + UINT64_C(11644473600)) * UINT64_C(10000000);
    file.dwLowDateTime = value.LowPart;
    file.dwHighDateTime = value.HighPart;
    if (!FileTimeToSystemTime(&file, &utc) || !SystemTimeToTzSpecificLocalTime(NULL, &utc, &local)) {
        wcscpy_s(text, 32, L"时间无效");
        return;
    }
    swprintf_s(text, 32, L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
}
/* 查询本机已知参与者，自己置顶；未知名称使用完整账号 ID，避免猜测或泄露其他账号。 */
static int peers_load(drop_client *c, drop_history *view) {
    sqlite3_stmt *s = NULL;
    int step;
    const char *query =
        "WITH peers(id) AS (SELECT ?1 UNION SELECT sender FROM messages UNION SELECT recipient FROM messages UNION SELECT recipient FROM pending UNION "
        "SELECT user_id FROM ui_contacts) SELECT id,coalesce(p.display_name,c.login,id)||CASE WHEN p.deleted THEN '（已删除）' WHEN p.enabled=0 THEN '（已停用）' ELSE '' END,"
        "(SELECT count(*) FROM messages m JOIN ui_messages u USING(message_id) WHERE u.unread=1 AND CASE WHEN m.sender=?1 THEN m.recipient ELSE m.sender END=id) "
        ",coalesce(p.login,c.login,''),coalesce(p.enabled,1) AND NOT coalesce(p.deleted,0) FROM peers LEFT JOIN ui_profiles p ON p.user_id=id LEFT JOIN ui_contacts c ON "
        "c.user_id=id ORDER BY id!=?1,(SELECT max(created_at) FROM messages WHERE sender=id OR recipient=id) DESC,id";
    if (sqlite3_prepare_v2(c->database, query, -1, &s, NULL) != SQLITE_OK) return 0;
    bind(s, 1, c->user);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        drop_peer *grown = realloc(view->peers, ((size_t)view->peer_count + 1) * sizeof(*grown)), *peer;
        if (!grown) break;
        view->peers = grown;
        peer = &view->peers[view->peer_count++];
        memset(peer, 0, sizeof(*peer));
        if (strcpy_s(peer->id, DROP_ID_CAP, (const char *)sqlite3_column_text(s, 0)) || !drop_to_wide((const char *)sqlite3_column_text(s, 1), peer->name, 160) ||
            !drop_to_wide((const char *)sqlite3_column_text(s, 3), peer->login, 65))
            break;
        peer->unread = sqlite3_column_int(s, 2);
        peer->available = sqlite3_column_int(s, 4);
    }
    sqlite3_finalize(s);
    return step == SQLITE_DONE;
}
/* 保存明确验证的登录名与账号关联；缓存属于当前账号自己的本地数据库。 */
int drop_ui_contact(drop_client *c, const char *id, const char *login) {
    sqlite3_stmt *s = NULL;
    int ok = 0;
    if (!drop_id_valid(id) || !login || !*login || strlen(login) > 64 || !drop_utf8_valid(login, strlen(login))) return 0;
    if (sqlite3_prepare_v2(c->database, "INSERT INTO ui_contacts VALUES(?1,?2) ON CONFLICT(user_id) DO UPDATE SET login=excluded.login", -1, &s, NULL) == SQLITE_OK) {
        bind(s, 1, id);
        bind(s, 2, login);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    return ok;
}
/* 查询本机历史并携带逐条附件快照；空对方表示全部，非空限制会话，结果须用 drop_ui_history_free 释放。 */
drop_history *drop_ui_history(drop_client *c, int page, const char *search, int64_t from, int64_t to, const char *peer) {
    drop_history *view = calloc(1, sizeof(*view));
    sqlite3_stmt *s = NULL;
    WCHAR *find = NULL;
    int skip = page * DROP_HISTORY_PAGE, step;
    if (!view || page < 0 || page > 1000000 || !peer || (*peer && !drop_id_valid(peer))) goto fail;
    strcpy_s(view->self, DROP_ID_CAP, c->user);
    strcpy_s(view->peer, DROP_ID_CAP, peer);
    if (search && *search) {
        find = malloc((strlen(search) + 1) * sizeof(WCHAR));
        if (!find || !drop_to_wide(search, find, strlen(search) + 1)) goto fail;
    }
    if (sqlite3_prepare_v2(c->database,
            "INSERT INTO ui_messages SELECT message_id,CASE WHEN device=?1 THEN 0 ELSE 1 END,"
            "CASE WHEN device=?1 THEN 1 ELSE 0 END FROM messages WHERE message_id NOT IN(SELECT message_id FROM ui_messages)",
            -1, &s, NULL) != SQLITE_OK)
        goto fail;
    bind(s, 1, c->device);
    if (sqlite3_step(s) != SQLITE_DONE) goto fail;
    sqlite3_finalize(s);
    s = NULL;
    if (sqlite3_prepare_v2(c->database,
            "UPDATE ui_messages SET unread=0,notified=1 WHERE message_id IN(SELECT m.message_id FROM messages m JOIN message_orders o USING(message_id) JOIN account_reads r "
            "USING(conversation_id) WHERE r.user_id=?1 AND o.server_order<=r.read_order)",
            -1, &s, NULL) != SQLITE_OK)
        goto fail;
    bind(s, 1, c->user);
    if (sqlite3_step(s) != SQLITE_DONE) goto fail;
    sqlite3_finalize(s);
    s = NULL;
    view->unread = scalar(c->database, "SELECT count(*) FROM ui_messages WHERE unread=1");
    view->new_notifications = scalar(c->database, "SELECT count(*) FROM ui_messages WHERE notified=0");
    view->pending_count = scalar(c->database, "SELECT count(*) FROM pending WHERE state='queued'");
    view->restore_review = scalar(c->database, "SELECT count(*) FROM pending WHERE state='restore_review'");
    view->downloads = scalar(c->database, "SELECT count(*) FROM downloads WHERE state!='complete'");
    /* 即使关闭 Windows 通知，也持久保存通知去重状态，避免重新开启后重复提醒旧消息。 */
    if (sqlite3_exec(c->database, "UPDATE ui_messages SET notified=1 WHERE notified=0", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    if (sqlite3_prepare_v2(c->database,
            "SELECT request_id,recipient,body,compressed,original_length,(SELECT count(*) FROM pending_files f WHERE f.request_id=p.request_id) FROM pending p WHERE "
            "state='restore_review' AND (?1='' OR recipient=?1) ORDER BY rowid DESC",
            -1, &s, NULL) != SQLITE_OK)
        goto fail;
    bind(s, 1, peer);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        WCHAR *text = body(s, 2);
        drop_history_row *row;
        if (!text) goto fail;
        if (find && !wcsstr(text, find)) {
            free(text);
            continue;
        }
        if (skip) {
            --skip;
            free(text);
            continue;
        }
        if (view->count == DROP_HISTORY_PAGE) {
            free(text);
            view->more = 1;
            goto finish;
        }
        row = &view->rows[view->count++];
        row->pending = 1;
        row->mine = 1;
        wcscpy_s(row->sender, 160, L"自己");
        strcpy_s(row->id, sizeof(row->id), (const char *)sqlite3_column_text(s, 0));
        wcscpy_s(row->time, 32, L"恢复后待核对");
        wcscpy_s(row->direction, 96, L"待发送任务");
        wcsncpy_s(row->preview, 320, text, _TRUNCATE);
        free(text);
        swprintf_s(row->files, 48, L"%d", sqlite3_column_int(s, 5));
        wcscpy_s(row->state, 96, L"请核对后明确重新提交");
    }
    if (step != SQLITE_DONE) goto fail;
    sqlite3_finalize(s);
    s = NULL;
    if (sqlite3_prepare_v2(c->database,
            "SELECT message_id,created_at,sender,recipient,body,compressed,original_length,"
            "(SELECT count(*) FROM downloads d WHERE d.message_id=m.message_id),"
            "(SELECT count(*) FROM downloads d WHERE d.message_id=m.message_id AND state!='complete'),"
            "(SELECT unread FROM ui_messages u WHERE u.message_id=m.message_id),"
            "(SELECT count(*) FROM device_states ds WHERE ds.conversation_id=m.conversation_id AND ds.user_id=m.recipient AND ds.body_order>=o.server_order AND "
            "(m.sender!=m.recipient OR ds.device_id!=m.device)),"
            "(SELECT count(*) FROM device_states ds WHERE ds.conversation_id=m.conversation_id AND ds.user_id=m.recipient AND ds.body_order>=o.server_order AND "
            "(m.sender!=m.recipient OR ds.device_id!=m.device) AND NOT EXISTS(SELECT 1 FROM downloads d WHERE d.message_id=m.message_id AND NOT EXISTS(SELECT 1 FROM object_states "
            "os WHERE os.object_id=d.object_id AND os.user_id=ds.user_id AND os.device_id=ds.device_id))),"
            "EXISTS(SELECT 1 FROM account_reads r WHERE r.conversation_id=m.conversation_id AND r.user_id=m.recipient AND r.read_order>=o.server_order),o.message_id IS NULL,"
            "coalesce((SELECT display_name||CASE WHEN deleted THEN '（已删除）' WHEN enabled=0 THEN '（已停用）' ELSE '' END FROM ui_profiles WHERE user_id=m.sender),(SELECT "
            "login FROM ui_contacts WHERE user_id=m.sender),m.sender)"
            " FROM messages m LEFT JOIN message_orders o USING(message_id) WHERE created_at>=?1 AND created_at<=?2 AND "
            "(?3='' OR CASE WHEN sender=?4 THEN recipient ELSE sender END=?3) ORDER BY created_at DESC,m.rowid DESC",
            -1, &s, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_bind_int64(s, 1, from);
    sqlite3_bind_int64(s, 2, to);
    bind(s, 3, peer);
    bind(s, 4, c->user);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        WCHAR *text = body(s, 4);
        drop_history_row *row;
        const char *sender = (const char *)sqlite3_column_text(s, 2), *recipient = (const char *)sqlite3_column_text(s, 3);
        WCHAR peer_name[DROP_ID_CAP];
        if (!text) goto fail;
        if (find && !wcsstr(text, find)) {
            free(text);
            continue;
        }
        if (skip) {
            --skip;
            free(text);
            continue;
        }
        if (view->count == DROP_HISTORY_PAGE) {
            free(text);
            view->more = 1;
            break;
        }
        row = &view->rows[view->count++];
        row->mine = !strcmp(sender, c->user);
        if (!drop_to_wide((const char *)sqlite3_column_text(s, 14), row->sender, 160)) goto fail;
        if (row->mine) wcscat_s(row->sender, 160, L"（自己）");
        strcpy_s(row->id, sizeof(row->id), (const char *)sqlite3_column_text(s, 0));
        time_text(sqlite3_column_int64(s, 1), row->time);
        row->unread = sqlite3_column_int(s, 9);
        drop_to_wide(!strcmp(sender, c->user) ? recipient : sender, peer_name, DROP_ID_CAP);
        if (!strcmp(sender, recipient))
            wcscpy_s(row->direction, 96, L"自己（多设备）");
        else
            swprintf_s(row->direction, 96, L"%s %.12s", !strcmp(sender, c->user) ? L"发送给" : L"收到来自", peer_name);
        wcsncpy_s(row->preview, 320, text, _TRUNCATE);
        free(text);
        {
            size_t i;
            for (i = 0; row->preview[i]; ++i)
                if (row->preview[i] == L'\r' || row->preview[i] == L'\n') row->preview[i] = L' ';
        }
        swprintf_s(row->files, 48, L"%d", sqlite3_column_int(s, 7));
        swprintf_s(row->state, 96, L"%s · 接收正文 %d 台 · %s %d 台 · %s", sqlite3_column_int(s, 8) ? L"本机附件待完成" : L"本机完整保存", sqlite3_column_int(s, 10),
            sqlite3_column_int(s, 7) ? L"完整附件" : L"正文确认", sqlite3_column_int(s, 11), sqlite3_column_int(s, 12) ? L"账号已读" : L"账号未读");
        if (sqlite3_column_int(s, 13)) wcscpy_s(row->state, 96, L"保留本机副本 · 服务器恢复后尚未核对");
    }
    if (step != SQLITE_DONE && step != SQLITE_ROW) goto fail;
finish:
    sqlite3_finalize(s);
    s = NULL;
    if (!peers_load(c, view)) goto fail;
    for (int i = 0; i < view->count; ++i) {
        view->rows[i].content = drop_ui_detail(c, view->rows[i].id, view->rows[i].pending);
        if (!view->rows[i].content) goto fail;
    }
    free(find);
    return view;
fail:
    sqlite3_finalize(s);
    free(find);
    drop_ui_history_free(view);
    return NULL;
}
/* 历史快照拥有全部逐条正文、附件及联系人，所有失败与界面替换路径共用此释放入口。 */
void drop_ui_history_free(drop_history *view) {
    if (!view) return;
    for (int i = 0; i < view->count; ++i)
        drop_ui_detail_free(view->rows[i].content);
    free(view->peers);
    free(view);
}
/* 读取消息完整正文及附件状态，支持待发送记录；返回结果由 drop_ui_detail_free 释放。 */
drop_detail *drop_ui_detail(drop_client *c, const char *id, int pending) {
    drop_detail *detail = calloc(1, sizeof(*detail));
    sqlite3_stmt *s = NULL;
    int step;
    const char *query =
        pending ? "SELECT body,compressed,original_length FROM pending WHERE request_id=?1" : "SELECT body,compressed,original_length FROM messages WHERE message_id=?1";
    if (!detail || !drop_id_valid(id) || sqlite3_prepare_v2(c->database, query, -1, &s, NULL) != SQLITE_OK) goto fail;
    bind(s, 1, id);
    if (sqlite3_step(s) != SQLITE_ROW || !(detail->text = body(s, 0))) goto fail;
    sqlite3_finalize(s);
    s = NULL;
    if (sqlite3_prepare_v2(c->database,
            pending ? "SELECT local_id,filename,total,sha256,'pending' FROM pending_files WHERE request_id=?1 ORDER BY position"
                    : "SELECT object_id,filename,total,sha256,state FROM downloads WHERE message_id=?1 ORDER BY rowid",
            -1, &s, NULL) != SQLITE_OK)
        goto fail;
    bind(s, 1, id);
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        drop_file_view *f;
        if (detail->count == DROP_MAX_ATTACHMENTS) goto fail;
        f = &detail->files[detail->count++];
        strcpy_s(f->id, sizeof(f->id), (const char *)sqlite3_column_text(s, 0));
        strcpy_s(f->sha, sizeof(f->sha), (const char *)sqlite3_column_text(s, 3));
        f->total = (uint64_t)sqlite3_column_int64(s, 2);
        f->complete = !strcmp((const char *)sqlite3_column_text(s, 4), "complete");
        if (!drop_to_wide((const char *)sqlite3_column_text(s, 1), f->name, 1025)) goto fail;
    }
    if (step != SQLITE_DONE) goto fail;
    sqlite3_finalize(s);
    return detail;
fail:
    sqlite3_finalize(s);
    drop_ui_detail_free(detail);
    return NULL;
}
/* 释放详情正文和结构，允许传入空指针。 */
void drop_ui_detail_free(drop_detail *detail) {
    if (detail) {
        free(detail->text);
        free(detail);
    }
}
/* 更新本机未读状态并排队账号已读位置，使其他设备随后同步相同位置。 */
int drop_ui_mark_read(drop_client *c, const char *id) {
    return drop_client_read_queue(c, id);
}
/* 查询登录名对应的可发送账号 ID；空接收人使用当前账号，失败不构造发送目标。 */
drop_error drop_ui_recipient(drop_client *c, const char *login, char id[DROP_ID_CAP]) {
    cJSON *input = cJSON_CreateObject(), *out = NULL;
    DWORD status;
    drop_error error;
    if (!login || !*login) {
        strcpy_s(id, DROP_ID_CAP, c->user);
        cJSON_Delete(input);
        return DROP_OK;
    }
    if (!input || !cJSON_AddStringToObject(input, "login", login)) {
        cJSON_Delete(input);
        return DROP_E_IO;
    }
    error = drop_http_json_ca(c->url, L"POST", L"/api/v1/accounts", c->token, input, &out, &status, c->ca_file);
    if (error == DROP_OK) {
        if (status != 200)
            error = status == 401 ? DROP_E_AUTH : DROP_E_ARGUMENT;
        else if (!drop_id_valid(drop_json_string(out, "user_id", 32)))
            error = DROP_E_PROTOCOL;
        else if (strcpy_s(id, DROP_ID_CAP, drop_json_string(out, "user_id", 32)) || !drop_ui_contact(c, id, login))
            error = DROP_E_DATABASE;
        else if (cJSON_HasObjectItem(out, "enabled"))
            error = drop_client_profile_save(c, out);
    }
    cJSON_Delete(input);
    cJSON_Delete(out);
    return error;
}
/* 将原附件名转换为安全导出名，处理路径分隔符、保留名和非法尾部；仅用于展示及显式导出。 */
void drop_ui_safe_filename(const WCHAR *original, WCHAR safe[260]) {
    const WCHAR *base = original, *p;
    size_t i = 0;
    WCHAR prefix[16] = {0};
    for (p = original; *p; ++p)
        if (*p == L'/' || *p == L'\\') base = p + 1;
    for (p = base; *p && i < 220; ++p)
        safe[i++] = *p < 32 || wcschr(L"<>:\"/\\|?*", *p) ? L'_' : *p;
    while (i && (safe[i - 1] == L'.' || safe[i - 1] == L' '))
        --i;
    safe[i] = 0;
    if (!i) wcscpy_s(safe, 260, L"附件.bin");
    for (i = 0; i < 15 && safe[i] && safe[i] != L'.'; ++i)
        prefix[i] = safe[i];
    if (!_wcsicmp(prefix, L"CON") || !_wcsicmp(prefix, L"PRN") || !_wcsicmp(prefix, L"AUX") || !_wcsicmp(prefix, L"NUL") ||
        (!_wcsnicmp(prefix, L"COM", 3) && prefix[3] >= L'1' && prefix[3] <= L'9' && !prefix[4]) ||
        (!_wcsnicmp(prefix, L"LPT", 3) && prefix[3] >= L'1' && prefix[3] <= L'9' && !prefix[4])) {
        WCHAR temp[260];
        swprintf_s(temp, 260, L"_%s", safe);
        wcscpy_s(safe, 260, temp);
    }
}
/* 通过目录句柄解析最终路径，拒绝导出到客户端数据树内部，防止覆盖数据库、凭据或管理副本。 */
static int export_parent_allowed(drop_client *c, const WCHAR *parent) {
    HANDLE root = INVALID_HANDLE_VALUE, target = INVALID_HANDLE_VALUE;
    WCHAR *canonical_root = calloc(32768, sizeof(WCHAR)), *canonical_target = calloc(32768, sizeof(WCHAR));
    DWORD r, t;
    size_t length;
    int ok = 0;
    if (!canonical_root || !canonical_target) goto done;
    root = CreateFileW(c->root, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    target = CreateFileW(parent, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (root == INVALID_HANDLE_VALUE || target == INVALID_HANDLE_VALUE) goto done;
    r = GetFinalPathNameByHandleW(root, canonical_root, 32768, FILE_NAME_NORMALIZED);
    t = GetFinalPathNameByHandleW(target, canonical_target, 32768, FILE_NAME_NORMALIZED);
    if (!r || !t || r >= 32768 || t >= 32768) goto done;
    length = wcslen(canonical_root);
    while (length > 8 && canonical_root[length - 1] == L'\\')
        canonical_root[--length] = 0;
    ok =
        t < length || _wcsnicmp(canonical_root, canonical_target, length) || (canonical_root[length - 1] != L'\\' && canonical_target[length] && canonical_target[length] != L'\\');
done:
    if (root != INVALID_HANDLE_VALUE) CloseHandle(root);
    if (target != INVALID_HANDLE_VALUE) CloseHandle(target);
    free(canonical_root);
    free(canonical_target);
    return ok;
}
/* 从本机完整副本导出附件，先验证长度和 SHA-256、刷新临时文件，再按用户明确选择替换目标。 */
drop_error drop_ui_export(drop_client *c, const drop_file_view *f, const WCHAR *destination) {
    WCHAR name[80], wide[DROP_ID_CAP], source[32768], absolute[32768], parent[32768], temporary[32768], *slash;
    unsigned char hash[32], expected[32];
    uint64_t size;
    DWORD n;
    char random[DROP_ID_CAP];
    HANDLE file = INVALID_HANDLE_VALUE;
    drop_error error;
    if (!f->complete || !drop_id_valid(f->id) || !drop_hash_decode(f->sha, expected) || !drop_to_wide(f->id, wide, DROP_ID_CAP) || swprintf_s(name, 80, L"%s.file", wide) < 0 ||
        !drop_path_join(c->files, name, source, 32768))
        return DROP_E_ARGUMENT;
    n = GetFullPathNameW(destination, 32768, absolute, NULL);
    if (!n || n >= 32768 || !_wcsicmp(absolute, source)) return DROP_E_ARGUMENT;
    wcscpy_s(parent, 32768, absolute);
    slash = wcsrchr(parent, L'\\');
    if (!slash) return DROP_E_ARGUMENT;
    if (slash == parent + 2)
        slash[1] = 0;
    else
        *slash = 0;
    if (!export_parent_allowed(c, parent)) return DROP_E_ARGUMENT;
    if (!drop_hash_file(source, hash, &size) || size != f->total || sodium_memcmp(hash, expected, 32)) return DROP_E_CRYPTO;
    drop_id_new(random);
    if (!drop_to_wide(random, wide, DROP_ID_CAP) || swprintf_s(name, 80, L"Drop.%s.exporting", wide) < 0 || !drop_path_join(parent, name, temporary, 32768)) return DROP_E_ARGUMENT;
    if (!CopyFileW(source, temporary, TRUE)) return drop_io_error();
    error = DROP_E_IO;
    if (!drop_hash_file(temporary, hash, &size) || size != f->total || sodium_memcmp(hash, expected, 32)) {
        error = DROP_E_CRYPTO;
        goto done;
    }
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE || !FlushFileBuffers(file)) {
        error = drop_io_error();
        goto done;
    }
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    /* 只有在用户通过保存框明确选择目标、完整校验并刷新临时文件后才允许替换。 */
    if (MoveFileExW(temporary, absolute, MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING)) return DROP_OK;
    error = drop_io_error();
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    DeleteFileW(temporary);
    return error;
}

/* HTTP 模式的 SRP 双向认证及有限会话缓存，请求认证和重放窗口由会话锁保护。 */
#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* 有限且有有效期的 PAKE 会话；未完成时拥有 SRP 验证器，完成后保存密钥及 HTTP 重放窗口。 */
typedef struct pake_slot {
    char id[DROP_ID_CAP], login[65], user[DROP_ID_CAP], device[DROP_ID_CAP], version[DROP_ID_CAP], token[DROP_TOKEN_CAP];
    struct SRPVerifier *verifier;
    drop_pake_keys keys;
    /* HTTP 已认证序号的最高值与 64 位防重放窗口，允许窗口内尚未接收的乱序帧。 */
    uint64_t high, window;
    ULONGLONG expires;
    /* 仅双方证明和完成帧通过后为真，未完成槽位不可处理业务请求。 */
    int ready;
} pake_slot;
/* 受同一临界区保护的 PAKE 会话缓存及未知账号模拟材料，关闭服务器时清零所有槽位。 */
typedef struct drop_pake_server {
    CRITICAL_SECTION lock;
    pake_slot slots[64];
    drop_pake_record dummy;
} drop_pake_server;

/* 迁移账号 PAKE 材料表，旧结构先保留加密快照，原消息及附件保持可用。 */
int drop_pake_schema(drop_server *server) {
    sqlite3_stmt *s = NULL;
    int version = -1;
    if (sqlite3_prepare_v2(server->database, "PRAGMA user_version", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) version = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (version >= 10 && version <= DROP_SCHEMA_VERSION) return 1;
    if (version != 9 || !drop_schema_snapshot(server, 9)) return 0;
    if (sqlite3_exec(server->database,
            "BEGIN IMMEDIATE; CREATE TABLE account_pake(user_id TEXT PRIMARY KEY REFERENCES accounts(user_id),salt BLOB NOT NULL CHECK(length(salt)=16),srp_salt BLOB NOT NULL "
            "CHECK(length(srp_salt)=32),verifier BLOB NOT NULL CHECK(length(verifier) BETWEEN 1 AND 512),version TEXT NOT NULL); PRAGMA user_version=10; COMMIT;",
            NULL, NULL, NULL) == SQLITE_OK)
        return 1;
    sqlite3_exec(server->database, "ROLLBACK", NULL, NULL, NULL);
    return 0;
}
/* 为大小写保留的账号生成独立盐及 SRP 验证材料，密码派生值和临时秘密随后清零。 */
int drop_pake_account_make(drop_server *server, const char *login, const char *password, drop_pake_record *record) {
    unsigned char secret[32];
    const unsigned char *salt = NULL, *verifier = NULL;
    unsigned salt_size = 0, verifier_size = 0;
    char identity[128];
    int ok = 0;
    memset(record, 0, sizeof(*record));
    randombytes_buf(record->salt, 16);
    if (!drop_srp_initialize() || !drop_pake_identity(server->keys.server_id, login, identity) || !drop_pake_password(password, record->salt, secret)) goto done;
    srp_create_salted_verification_key(SRP_SHA256, SRP_NG_4096, identity, secret, 32, &salt, &salt_size, &verifier, &verifier_size, NULL, NULL);
    if (!salt || salt_size != 32 || !verifier || !drop_srp_public_valid(verifier, verifier_size)) goto done;
    memcpy(record->srp_salt, salt, 32);
    memcpy(record->verifier, verifier, verifier_size);
    record->verifier_size = verifier_size;
    drop_id_new(record->version);
    ok = 1;
done:
    sodium_memzero(secret, sizeof(secret));
    drop_pake_bytes_free((unsigned char *)salt, salt_size);
    drop_pake_bytes_free((unsigned char *)verifier, verifier_size);
    if (!ok) sodium_memzero(record, sizeof(*record));
    return ok;
}
/* 在账号事务内保存 PAKE 盐、验证器及材料版本；不保存明文密码。 */
int drop_pake_account_store(drop_server *server, const char *user, const drop_pake_record *record) {
    sqlite3_stmt *s = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(server->database,
            "INSERT INTO account_pake(user_id,salt,srp_salt,verifier,version) VALUES(?1,?2,?3,?4,?5) ON CONFLICT(user_id) DO UPDATE SET "
            "salt=excluded.salt,srp_salt=excluded.srp_salt,verifier=excluded.verifier,version=excluded.version",
            -1, &s, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(s, 1, user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(s, 2, record->salt, 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(s, 3, record->srp_salt, 32, SQLITE_TRANSIENT);
    sqlite3_bind_blob(s, 4, record->verifier, (int)record->verifier_size, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, record->version, -1, SQLITE_TRANSIENT);
    ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}
/* 读取账号 PAKE 材料并核对账号可用状态，失败不建立握手会话。 */
static int account(drop_server *server, const char *login, drop_pake_record *record, char user[DROP_ID_CAP]) {
    sqlite3_stmt *s = NULL;
    int ok = 0;
    const char *version;
    memset(record, 0, sizeof(*record));
    *user = 0;
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database,
            "SELECT a.user_id,p.salt,p.srp_salt,p.verifier,p.version FROM accounts a JOIN account_pake p USING(user_id) WHERE a.login=?1 AND a.enabled=1 AND a.locked_until<=?2",
            -1, &s, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text(s, 1, login, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)time(NULL));
    if (sqlite3_step(s) != SQLITE_ROW || sqlite3_column_bytes(s, 1) != 16 || sqlite3_column_bytes(s, 2) != 32 || sqlite3_column_bytes(s, 3) < 1 ||
        sqlite3_column_bytes(s, 3) > 512 || !drop_id_valid(version = (const char *)sqlite3_column_text(s, 4)))
        goto done;
    strcpy_s(user, DROP_ID_CAP, (const char *)sqlite3_column_text(s, 0));
    memcpy(record->salt, sqlite3_column_blob(s, 1), 16);
    memcpy(record->srp_salt, sqlite3_column_blob(s, 2), 32);
    record->verifier_size = (unsigned)sqlite3_column_bytes(s, 3);
    memcpy(record->verifier, sqlite3_column_blob(s, 3), record->verifier_size);
    strcpy_s(record->version, DROP_ID_CAP, version);
    ok = 1;
done:
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
    return ok;
}
/* 销毁一个握手槽位的上游 SRP 状态并清零密钥、令牌和证明材料。 */
static void clear_slot(pake_slot *slot) {
    if (slot->verifier) srp_verifier_delete(slot->verifier);
    sodium_memzero(slot, sizeof(*slot));
}
/* 初始化 SRP 及受限握手缓存和锁，成功后服务器拥有其生命周期。 */
int drop_pake_server_start(drop_server *server) {
    drop_pake_server *p = calloc(1, sizeof(*p));
    char password[DROP_TOKEN_CAP];
    if (!p) return 0;
    InitializeCriticalSection(&p->lock);
    drop_token_new(password);
    if (!drop_pake_account_make(server, "dummy-pake-account", password, &p->dummy)) {
        DeleteCriticalSection(&p->lock);
        free(p);
        sodium_memzero(password, sizeof(password));
        return 0;
    }
    sodium_memzero(password, sizeof(password));
    server->transport = p;
    return 1;
}
/* 在请求停止后销毁全部握手槽位、同步锁和缓存，并清零传输状态。 */
void drop_pake_server_stop(drop_server *server) {
    drop_pake_server *p = server->transport;
    unsigned i;
    if (!p) return;
    for (i = 0; i < 64; i++)
        clear_slot(&p->slots[i]);
    DeleteCriticalSection(&p->lock);
    sodium_memzero(p, sizeof(*p));
    free(p);
    server->transport = NULL;
}
/* 在握手锁内按会话 ID 查找仍有效的槽位；指针不能在锁释放后无保护地使用。 */
static pake_slot *find(drop_pake_server *p, const char *id, int empty) {
    unsigned i;
    pake_slot *available = NULL;
    ULONGLONG now = GetTickCount64();
    for (i = 0; i < 64; i++) {
        pake_slot *slot = &p->slots[i];
        if (*slot->id && slot->expires <= now) clear_slot(slot);
        if (!*slot->id) {
            if (!available) available = slot;
        } else if (id && !strcmp(slot->id, id))
            return slot;
    }
    return empty ? available : NULL;
}
/* 在数据库锁内累积账号认证失败次数，达到阈值后设置短时锁定；未知账号不写入记录。 */
static void failure(drop_server *server, const char *user) {
    sqlite3_stmt *s = NULL;
    if (!*user) return;
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database,
            "UPDATE accounts SET locked_until=CASE WHEN failed_logins>=4 THEN ?1 ELSE locked_until END,failed_logins=failed_logins+1 WHERE user_id=?2", -1, &s,
            NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)time(NULL) + 60);
        sqlite3_bind_text(s, 2, user, -1, SQLITE_TRANSIENT);
        sqlite3_step(s);
    }
    sqlite3_finalize(s);
    LeaveCriticalSection(&server->database_lock);
}
/* 在同步保护下限制 PAKE 握手请求速率，过量请求不继续进行昂贵密码运算。 */
static int rate(drop_server *server) {
    int ok;
    EnterCriticalSection(&server->login_lock);
    if (GetTickCount64() - server->login_window >= 1000) {
        server->login_window = GetTickCount64();
        server->login_count = 0;
    }
    ok = ++server->login_count <= 4;
    LeaveCriticalSection(&server->login_lock);
    return ok;
}
/* 处理参数、SRP 开始及认证完成步骤，限制槽位和速率，验证双方证明后才签发设备会话。 */
int drop_pake_handshake(drop_server *server, const char *path, const cJSON *input, cJSON **output) {
    drop_pake_server *p = server->transport;
    const char *login, *hex, *id, *device, *name, *token, *context = "pake-finish-v1";
    unsigned char bytes[512], proof[32], *plain = NULL, *frame = NULL;
    DWORD plain_size = 0, frame_size = 0;
    const unsigned char *public_B = NULL, *server_proof = NULL, *shared = NULL;
    unsigned public_size = 0;
    int shared_size = 0;
    char user[DROP_ID_CAP], identity[128], salt_hex[65], public_hex[1025];
    drop_pake_record record;
    pake_slot *slot = NULL;
    cJSON *metadata = NULL, *response = NULL;
    uint64_t sequence = 0;
    int status = 401, locked = 0;
    *output = NULL;
    if (!p || !input) return 400;
    if (!strcmp(path, "/api/v1/pake/parameters")) {
        login = drop_json_string(input, "login", 64);
        if (!drop_pake_identity(server->keys.server_id, login, identity)) return 400;
        if (!account(server, login, &record, user)) {
            record = p->dummy;
            crypto_generichash(record.salt, 16, (const unsigned char *)identity, strlen(identity), server->keys.object_master_key, 32);
        }
        *output = cJSON_CreateObject();
        if (!*output) return 500;
        sodium_bin2hex(salt_hex, sizeof(salt_hex), record.salt, 16);
        cJSON_AddStringToObject(*output, "suite", DROP_PAKE_SUITE);
        cJSON_AddStringToObject(*output, "salt", salt_hex);
        cJSON_AddStringToObject(*output, "server_id", server->keys.server_id);
        sodium_memzero(&record, sizeof(record));
        return 200;
    }
    if (!strcmp(path, "/api/v1/pake/start")) {
        login = drop_json_string(input, "login", 64);
        hex = drop_json_string(input, "A", 1024);
        if (!drop_pake_identity(server->keys.server_id, login, identity) || !hex || !strlen(hex) || strlen(hex) % 2 || !drop_pake_hex(hex, bytes, strlen(hex) / 2) ||
            !drop_srp_public_valid(bytes, strlen(hex) / 2))
            return 400;
        if (!rate(server) || WaitForSingleObject(server->password_slots, 0) != WAIT_OBJECT_0) return 429;
        EnterCriticalSection(&p->lock);
        slot = find(p, NULL, 1);
        if (!slot) {
            LeaveCriticalSection(&p->lock);
            ReleaseSemaphore(server->password_slots, 1, NULL);
            return 429;
        }
        if (!account(server, login, &record, user)) {
            record = p->dummy;
            *user = 0;
        }
        slot->verifier = srp_verifier_new(SRP_SHA256, SRP_NG_4096, identity, record.srp_salt, 32, record.verifier, record.verifier_size, bytes, (unsigned)(strlen(hex) / 2),
            &public_B, &public_size, NULL, NULL, 1);
        if (!slot->verifier || !public_B || !drop_srp_public_valid(public_B, public_size)) {
            clear_slot(slot);
            status = 400;
        } else {
            drop_id_new(slot->id);
            strcpy_s(slot->login, sizeof(slot->login), login);
            strcpy_s(slot->user, sizeof(slot->user), user);
            strcpy_s(slot->version, sizeof(slot->version), record.version);
            slot->expires = GetTickCount64() + 30000;
            shared = srp_verifier_get_session_key(slot->verifier, &shared_size);
            if (shared_size != 32 || !drop_pake_derive(&slot->keys, shared, server->keys.server_id, slot->id)) {
                clear_slot(slot);
                status = 500;
            } else {
                *output = cJSON_CreateObject();
                sodium_bin2hex(salt_hex, sizeof(salt_hex), record.srp_salt, 32);
                sodium_bin2hex(public_hex, sizeof(public_hex), public_B, public_size);
                if (!*output || !cJSON_AddStringToObject(*output, "session_id", slot->id) || !cJSON_AddStringToObject(*output, "srp_salt", salt_hex) ||
                    !cJSON_AddStringToObject(*output, "B", public_hex)) {
                    clear_slot(slot);
                    status = 500;
                } else
                    status = 200;
            }
        }
        sodium_memzero(&record, sizeof(record));
        LeaveCriticalSection(&p->lock);
        ReleaseSemaphore(server->password_slots, 1, NULL);
        return status;
    }
    if (strcmp(path, "/api/v1/pake/finish")) return 404;
    id = drop_json_string(input, "session_id", 32);
    hex = drop_json_string(input, "proof", 64);
    if (!drop_id_valid(id) || !drop_pake_hex(hex, proof, 32)) return 400;
    EnterCriticalSection(&p->lock);
    locked = 1;
    slot = find(p, id, 0);
    if (!slot || slot->ready) goto done;
    srp_verifier_verify_session(slot->verifier, proof, &server_proof);
    if (!server_proof || !srp_verifier_is_authenticated(slot->verifier) || !*slot->user) {
        failure(server, slot->user);
        goto consume;
    }
    hex = drop_json_string(input, "payload", 8192);
    if (!hex || strlen(hex) % 2 || strlen(hex) > sizeof(bytes) * 2) goto consume;
    frame_size = (DWORD)(strlen(hex) / 2);
    if (!drop_pake_hex(hex, bytes, frame_size) || !drop_pake_decrypt(&slot->keys, DROP_PAKE_FINISH_TX, context, bytes, frame_size, &sequence, &plain, &plain_size) || sequence ||
        !(metadata = drop_json_parse((char *)plain, plain_size)))
        goto consume;
    device = drop_json_string(metadata, "device_id", 32);
    name = drop_json_string(metadata, "device_name", 128);
    token = drop_json_string(metadata, "token", 64);
    if (!drop_id_valid(device) || !name || !*name) goto consume;
    EnterCriticalSection(&server->database_lock);
    if (!account(server, slot->login, &record, user) || strcmp(user, slot->user) || strcmp(record.version, slot->version)) {
        LeaveCriticalSection(&server->database_lock);
        goto consume;
    }
    if (token) {
        char authorized[DROP_ID_CAP], authorized_device[DROP_ID_CAP];
        if (!drop_authorize(server, token, authorized, authorized_device) || strcmp(authorized, slot->user) || strcmp(authorized_device, device)) {
            LeaveCriticalSection(&server->database_lock);
            goto consume;
        }
        response = cJSON_CreateObject();
        if (response) {
            cJSON_AddStringToObject(response, "user_id", slot->user);
            cJSON_AddStringToObject(response, "device_id", device);
            cJSON_AddStringToObject(response, "token", token);
            cJSON_AddStringToObject(response, "server_id", server->keys.server_id);
            cJSON_AddStringToObject(response, "generation", server->keys.generation);
        }
    } else if (!drop_login_device(server, slot->user, device, name, slot->version, &response)) {
        LeaveCriticalSection(&server->database_lock);
        goto consume;
    }
    LeaveCriticalSection(&server->database_lock);
    if (!response) goto consume;
    token = drop_json_string(response, "token", 64);
    if (!token || strlen(token) != 64) goto consume;
    strcpy_s(slot->token, sizeof(slot->token), token);
    strcpy_s(slot->device, sizeof(slot->device), device);
    {
        char *serialized = cJSON_PrintUnformatted(response);
        char *frame_hex = NULL;
        if (!serialized ||
            !drop_pake_encrypt(&slot->keys, DROP_PAKE_FINISH_RX, 0, context, (unsigned char *)serialized, serialized ? strlen(serialized) : 0, &frame, &frame_size)) {
            cJSON_free(serialized);
            goto consume;
        }
        sodium_memzero(serialized, strlen(serialized));
        cJSON_free(serialized);
        frame_hex = malloc((size_t)frame_size * 2 + 1);
        *output = cJSON_CreateObject();
        sodium_bin2hex(public_hex, sizeof(public_hex), server_proof, 32);
        if (!frame_hex || !*output) {
            free(frame_hex);
            goto consume;
        }
        sodium_bin2hex(frame_hex, (size_t)frame_size * 2 + 1, frame, frame_size);
        if (!cJSON_AddStringToObject(*output, "proof", public_hex) || !cJSON_AddStringToObject(*output, "payload", frame_hex)) {
            free(frame_hex);
            goto consume;
        }
        free(frame_hex);
    }
    {
        unsigned i;
        for (i = 0; i < 64; i++)
            if (&p->slots[i] != slot && p->slots[i].ready && !strcmp(p->slots[i].user, slot->user) && !strcmp(p->slots[i].device, slot->device)) clear_slot(&p->slots[i]);
    }
    srp_verifier_delete(slot->verifier);
    slot->verifier = NULL;
    slot->ready = 1;
    slot->expires = GetTickCount64() + 3600000;
    status = 200;
    goto done;
consume:
    clear_slot(slot);
done:
    if (locked) LeaveCriticalSection(&p->lock);
    if (response) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(response, "token");
        if (cJSON_IsString(t)) sodium_memzero(t->valuestring, strlen(t->valuestring));
    }
    if (metadata) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(metadata, "token");
        if (cJSON_IsString(t)) sodium_memzero(t->valuestring, strlen(t->valuestring));
    }
    cJSON_Delete(response);
    cJSON_Delete(metadata);
    drop_pake_bytes_free(plain, plain_size);
    drop_pake_bytes_free(frame, frame_size);
    sodium_memzero(bytes, sizeof(bytes));
    sodium_memzero(&record, sizeof(record));
    return status;
}
/* 在握手锁内认证 HTTP 帧，以 64 位滑动窗口拒绝重复或过旧序号；成功返回令牌、密钥和调用者须清理的明文。 */
int drop_pake_server_open(drop_server *server, const char *context, const unsigned char *frame, DWORD size, drop_pake_keys *keys, uint64_t *sequence, unsigned char **plain,
    DWORD *length, char token[DROP_TOKEN_CAP]) {
    drop_pake_server *p = server->transport;
    pake_slot *slot;
    char id[DROP_ID_CAP];
    uint64_t distance;
    int ok = 0;
    *plain = NULL;
    *length = 0;
    if (!p || size < DROP_PAKE_FRAME_OVERHEAD) return 0;
    sodium_bin2hex(id, sizeof(id), frame + 4, 16);
    EnterCriticalSection(&p->lock);
    slot = find(p, id, 0);
    if (!slot || !slot->ready || !drop_pake_decrypt(&slot->keys, DROP_PAKE_HTTP_TX, context, frame, size, sequence, plain, length) || !*sequence) goto done;
    if (*sequence > slot->high) {
        distance = *sequence - slot->high;
        slot->window = distance >= 64 ? 1 : (slot->window << distance) | 1;
        slot->high = *sequence;
    } else {
        distance = slot->high - *sequence;
        if (distance >= 64 || (slot->window & ((uint64_t)1 << distance))) goto done;
        slot->window |= (uint64_t)1 << distance;
    }
    *keys = slot->keys;
    strcpy_s(token, DROP_TOKEN_CAP, slot->token);
    ok = 1;
done:
    LeaveCriticalSection(&p->lock);
    if (!ok && *plain) {
        drop_pake_bytes_free(*plain, *length);
        *plain = NULL;
        *length = 0;
    }
    return ok;
}

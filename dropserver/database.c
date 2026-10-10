#include "server.h"
#include "cloud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int execute(sqlite3 *database, const char *sql) { return sqlite3_exec(database, sql, NULL, NULL, NULL) == SQLITE_OK; }
static int login_valid(const char *login) {
    size_t i, length = login ? strlen(login) : 0;
    if (length < 3 || length > 64) return 0;
    for (i = 0; i < length; ++i)
        if (!((login[i] >= 'a' && login[i] <= 'z') || (login[i] >= '0' && login[i] <= '9') ||
            login[i] == '_' || login[i] == '-' || login[i] == '.')) return 0;
    return 1;
}
static int key_database(sqlite3 **output, const WCHAR *path, const unsigned char *key) {
    char *utf8; int result = 0; sqlite3 *database = NULL; sqlite3_stmt *statement = NULL;
    utf8 = malloc(131072); if (!utf8) return 0;
    if (!drop_to_utf8(path, utf8, 131072) || sqlite3_open_v2(utf8, &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) goto cleanup;
    /* No SQL or writes before the key. Refuse a SQLite build lacking the codec. */
    if (sqlite3_key(database, key, 32) != SQLITE_OK ||
        sqlite3_prepare_v2(database, "PRAGMA cipher_version", -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW || !sqlite3_column_text(statement, 0)) goto cleanup;
    sqlite3_finalize(statement); statement = NULL;
    sqlite3_busy_timeout(database, 5000);
    if (!execute(database, "PRAGMA cipher_log_source=NONE; PRAGMA cipher_log_source=CORE; PRAGMA cipher_log_source=PROVIDER;"
        "PRAGMA cipher_memory_security=ON; PRAGMA temp_store=MEMORY;"
        "PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF; PRAGMA synchronous=FULL;"
        "PRAGMA secure_delete=ON; PRAGMA journal_mode=WAL;")) goto cleanup;
    if (sqlite3_prepare_v2(database, "SELECT count(*) FROM sqlite_master", -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW) goto cleanup;
    *output = database; database = NULL; result = 1;
cleanup:
    sqlite3_finalize(statement); if (database) sqlite3_close(database); free(utf8); return result;
}
static int migrate(drop_server *server) {
    sqlite3_stmt *statement = NULL; int version = -1, success = 0;
    if (sqlite3_prepare_v2(server->database, "PRAGMA user_version", -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) version = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement); statement = NULL;
    if (version >= 1 && version <= DROP_SCHEMA_VERSION) return 1;
    if (version != 0) return 0;
    if (!execute(server->database, "BEGIN IMMEDIATE;"
        "CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "CREATE TABLE accounts(user_id TEXT PRIMARY KEY,login TEXT NOT NULL UNIQUE,display_name TEXT NOT NULL,"
        "password_hash TEXT NOT NULL,enabled INTEGER NOT NULL DEFAULT 1,quota_bytes INTEGER NOT NULL DEFAULT 10737418240,"
        "failed_logins INTEGER NOT NULL DEFAULT 0,locked_until INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE devices(device_id TEXT NOT NULL,user_id TEXT NOT NULL REFERENCES accounts(user_id),"
        "name TEXT NOT NULL,token_digest BLOB NOT NULL UNIQUE,expires_at INTEGER NOT NULL,revoked INTEGER NOT NULL DEFAULT 0,"
        "last_seen INTEGER NOT NULL,PRIMARY KEY(user_id,device_id));"
        "PRAGMA user_version=1;")) goto cleanup;
    if (sqlite3_prepare_v2(server->database, "INSERT INTO meta VALUES('server_id',?1),('generation',?2)", -1, &statement, NULL) != SQLITE_OK) goto cleanup;
    sqlite3_bind_text(statement, 1, server->keys.server_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, server->keys.generation, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_DONE || !execute(server->database, "COMMIT")) goto cleanup;
    success = 1;
cleanup:
    sqlite3_finalize(statement); if (!success) execute(server->database, "ROLLBACK"); return success;
}
static int verify_identity(drop_server *server) {
    sqlite3_stmt *statement = NULL; int matched = 0;
    if (sqlite3_prepare_v2(server->database, "SELECT key,value FROM meta WHERE key IN ('server_id','generation')", -1, &statement, NULL) != SQLITE_OK) return 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char *key = (const char *)sqlite3_column_text(statement, 0);
        const char *value = (const char *)sqlite3_column_text(statement, 1);
        if (!strcmp(key, "server_id") && !strcmp(value, server->keys.server_id)) matched |= 1;
        else if (!strcmp(key, "generation") && !strcmp(value, server->keys.generation)) matched |= 2;
        else { matched = 0; break; }
    }
    sqlite3_finalize(statement); return matched == 3;
}
int drop_server_open(drop_server *server, const WCHAR *directory, int initialize) {
    WCHAR path[32768], db_path[32768]; unsigned char *secret = NULL; DWORD length = 0; DWORD full_length;
    memset(server, 0, sizeof(*server)); server->lock_file = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&server->database_lock); InitializeCriticalSection(&server->login_lock);
    InitializeCriticalSection(&server->peers_lock);
    InitializeCriticalSection(&server->objects_lock);
    InitializeCriticalSection(&server->finalize_lock);
    server->zstd_level=3;
    server->connection_timeout_seconds=90;
    full_length = GetFullPathNameW(directory, 32768, server->directory, NULL);
    if(full_length && full_length<32768 && drop_path_join(server->directory,L"restore.incomplete",path,32768) && GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES) goto failure;
    if (!full_length || full_length >= 32768 || !drop_path_join(server->directory,L"runtime-machine.dpapi",path,32768)) goto failure;
    if (!(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES?drop_machine_directory(server->directory):drop_private_directory(server->directory)) ||
        !drop_path_join(server->directory, L"run.lock", path, 32768)) goto failure;
    server->lock_file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (server->lock_file == INVALID_HANDLE_VALUE) goto failure;
    if (!drop_path_join(server->directory, L"server.db", db_path, 32768) ||
        !drop_path_join(server->directory, L"runtime-machine.dpapi", path, 32768)) goto failure;
    if(GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES && !drop_path_join(server->directory,L"runtime-user.dpapi",path,32768)) goto failure;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        if (!initialize || GetFileAttributesW(db_path) != INVALID_FILE_ATTRIBUTES) goto failure;
        memcpy(server->keys.magic, "DROPKEY1", 8);
        randombytes_buf(server->keys.database_key, 32); randombytes_buf(server->keys.object_master_key, 32);
        drop_id_new(server->keys.server_id); drop_id_new(server->keys.generation);
        if (!drop_secret_save(path, &server->keys, sizeof(server->keys))) goto failure;
    } else {
        if (!drop_secret_load(path, &secret, &length) || length != sizeof(server->keys)) goto failure;
        memcpy(&server->keys, secret, length); sodium_memzero(secret, length); free(secret); secret = NULL;
        if (memcmp(server->keys.magic, "DROPKEY1", 8) || server->keys.server_id[32] || server->keys.generation[32] ||
            !drop_id_valid(server->keys.server_id) || !drop_id_valid(server->keys.generation)) goto failure;
    }
    if (!initialize && GetFileAttributesW(db_path) == INVALID_FILE_ATTRIBUTES) goto failure;
    if (!key_database(&server->database, db_path, server->keys.database_key) || !migrate(server) ||
        !verify_identity(server) || !drop_text_schema(server) || !drop_objects_schema(server) || !drop_receipts_schema(server) || !drop_maintenance_schema(server) || !drop_backup_schema(server) || !drop_finalize_schema(server) || !drop_cloud_schema(server)) goto failure;
    server->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    server->password_slots = CreateSemaphoreW(NULL, 2, 2, NULL);
    if (!server->stop_event || !server->password_slots || crypto_pwhash_str_alg(server->dummy_password_hash,
        "dummy-account-password", 22, crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
        crypto_pwhash_ALG_ARGON2ID13) != 0) goto failure;
    return 1;
failure:
    if (secret) { sodium_memzero(secret, length); free(secret); }
    drop_server_close(server); return 0;
}
void drop_server_close(drop_server *server) {
    drop_cloud_stop(server);
    drop_finalize_stop(server);
    if(server->orphan_scan) FindClose(server->orphan_scan);
    if (server->database) sqlite3_close(server->database);
    if (server->lock_file != INVALID_HANDLE_VALUE) CloseHandle(server->lock_file);
    if (server->stop_event) CloseHandle(server->stop_event);
    if (server->password_slots) CloseHandle(server->password_slots);
    DeleteCriticalSection(&server->database_lock); DeleteCriticalSection(&server->login_lock);
    DeleteCriticalSection(&server->peers_lock);
    DeleteCriticalSection(&server->objects_lock);
    DeleteCriticalSection(&server->finalize_lock);
    sodium_memzero(server, sizeof(*server));
}
int drop_account_create(drop_server *server, const char *login, const char *display, const char *password) {
    char hash[crypto_pwhash_STRBYTES], id[DROP_ID_CAP]; sqlite3_stmt *statement = NULL; int result = 0;
    if (!login_valid(login) || !display || strlen(display) > 128 || !drop_utf8_valid(display, strlen(display)) ||
        !password || strlen(password) < 12 || strlen(password) > 1024 || !drop_utf8_valid(password, strlen(password))) return 0;
    if (crypto_pwhash_str_alg(hash, password, strlen(password), crypto_pwhash_OPSLIMIT_INTERACTIVE,
        crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) != 0) return 0;
    drop_id_new(id); EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, "INSERT INTO accounts(user_id,login,display_name,password_hash) VALUES(?1,?2,?3,?4)", -1, &statement, NULL) == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, id, -1, SQLITE_TRANSIENT); sqlite3_bind_text(statement, 2, login, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, display, -1, SQLITE_TRANSIENT); sqlite3_bind_text(statement, 4, hash, -1, SQLITE_TRANSIENT);
        result = sqlite3_step(statement) == SQLITE_DONE;
    }
    sqlite3_finalize(statement); LeaveCriticalSection(&server->database_lock); sodium_memzero(hash, sizeof(hash)); return result;
}
int drop_login(drop_server *server, const char *login, const char *password, const char *device_id,
    const char *device_name, cJSON **response) {
    char hash[crypto_pwhash_STRBYTES], user_id[DROP_ID_CAP] = {0}, token[DROP_TOKEN_CAP];
    unsigned char digest[crypto_hash_sha256_BYTES]; sqlite3_stmt *statement = NULL;
    int enabled = 0, failures = 0, authenticated = 0, result = 0; sqlite3_int64 locked_until = 0, now = (sqlite3_int64)time(NULL);
    if (!login_valid(login) || !password || !device_name || !drop_id_valid(device_id) ||
        strlen(password) > 1024 || strlen(device_name) > 128 || !drop_utf8_valid(device_name, strlen(device_name))) return 0;
    /* Global rate and bounded Argon2 concurrency, including unknown accounts. */
    EnterCriticalSection(&server->login_lock);
    if (GetTickCount64() - server->login_window >= 1000) { server->login_window = GetTickCount64(); server->login_count = 0; }
    if (++server->login_count > 4) { LeaveCriticalSection(&server->login_lock); return 0; }
    LeaveCriticalSection(&server->login_lock);
    if (WaitForSingleObject(server->password_slots, 0) != WAIT_OBJECT_0) return 0;
    memcpy(hash, server->dummy_password_hash, sizeof(hash));
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, "SELECT user_id,password_hash,enabled,failed_logins,locked_until FROM accounts WHERE login=?1", -1, &statement, NULL) == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, login, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            strcpy_s(user_id, sizeof(user_id), (const char *)sqlite3_column_text(statement, 0));
            strcpy_s(hash, sizeof(hash), (const char *)sqlite3_column_text(statement, 1));
            enabled = sqlite3_column_int(statement, 2); failures = sqlite3_column_int(statement, 3); locked_until = sqlite3_column_int64(statement, 4);
        }
    }
    sqlite3_finalize(statement); statement = NULL; LeaveCriticalSection(&server->database_lock);
    authenticated = crypto_pwhash_str_verify(hash, password, strlen(password)) == 0 && enabled && now >= locked_until;
    sodium_memzero(hash, sizeof(hash)); ReleaseSemaphore(server->password_slots, 1, NULL);
    EnterCriticalSection(&server->database_lock);
    if (!authenticated) {
        if (*user_id && sqlite3_prepare_v2(server->database, "UPDATE accounts SET failed_logins=failed_logins+1,locked_until=?1 WHERE user_id=?2", -1, &statement, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, failures >= 4 ? now + 60 : locked_until);
            sqlite3_bind_text(statement, 2, user_id, -1, SQLITE_TRANSIENT); sqlite3_step(statement);
        }
        goto cleanup;
    }
    drop_token_new(token); crypto_hash_sha256(digest, (const unsigned char *)token, strlen(token));
    if (!execute(server->database, "BEGIN IMMEDIATE")) goto cleanup;
    if (sqlite3_prepare_v2(server->database, "INSERT INTO devices(device_id,user_id,name,token_digest,expires_at,last_seen) VALUES(?1,?2,?3,?4,?5,?6) "
        "ON CONFLICT(user_id,device_id) DO UPDATE SET name=excluded.name,token_digest=excluded.token_digest,expires_at=excluded.expires_at,last_seen=excluded.last_seen,revoked=0", -1, &statement, NULL) != SQLITE_OK) goto rollback;
    sqlite3_bind_text(statement, 1, device_id, -1, SQLITE_TRANSIENT); sqlite3_bind_text(statement, 2, user_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, device_name, -1, SQLITE_TRANSIENT); sqlite3_bind_blob(statement, 4, digest, sizeof(digest), SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 5, now + 30 * 86400); sqlite3_bind_int64(statement, 6, now);
    if (sqlite3_step(statement) != SQLITE_DONE) goto rollback;
    sqlite3_finalize(statement); statement = NULL;
    if (sqlite3_prepare_v2(server->database, "UPDATE accounts SET failed_logins=0,locked_until=0 WHERE user_id=?1 AND enabled=1", -1, &statement, NULL) != SQLITE_OK) goto rollback;
    sqlite3_bind_text(statement, 1, user_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_DONE || sqlite3_changes(server->database) != 1) goto rollback;
    *response = cJSON_CreateObject(); if (!*response) goto rollback;
    if (!cJSON_AddStringToObject(*response, "user_id", user_id) || !cJSON_AddStringToObject(*response, "device_id", device_id) ||
        !cJSON_AddStringToObject(*response, "token", token) || !cJSON_AddStringToObject(*response, "server_id", server->keys.server_id) ||
        !cJSON_AddStringToObject(*response, "generation", server->keys.generation) || !execute(server->database, "COMMIT")) {
        cJSON_Delete(*response); *response = NULL; goto rollback;
    }
    result = 1; goto cleanup;
rollback:
    execute(server->database, "ROLLBACK");
cleanup:
    sqlite3_finalize(statement); LeaveCriticalSection(&server->database_lock);
    sodium_memzero(token, sizeof(token)); sodium_memzero(digest, sizeof(digest)); return result;
}
int drop_authorize(drop_server *server, const char *token, char user_id[DROP_ID_CAP], char device_id[DROP_ID_CAP]) {
    unsigned char digest[32]; sqlite3_stmt *statement = NULL; int result = 0; size_t i;
    if (!token || strlen(token) != 64) return 0;
    for (i = 0; i < 64; ++i) if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) return 0;
    crypto_hash_sha256(digest, (const unsigned char *)token, 64);
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, "SELECT d.user_id,d.device_id FROM devices d JOIN accounts a USING(user_id) WHERE token_digest=?1 AND revoked=0 AND enabled=1 AND expires_at>?2", -1, &statement, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(statement, 1, digest, sizeof(digest), SQLITE_TRANSIENT); sqlite3_bind_int64(statement, 2, (sqlite3_int64)time(NULL));
        if (sqlite3_step(statement) == SQLITE_ROW) {
            strcpy_s(user_id, DROP_ID_CAP, (const char *)sqlite3_column_text(statement, 0));
            strcpy_s(device_id, DROP_ID_CAP, (const char *)sqlite3_column_text(statement, 1)); result = 1;
        }
    }
    sqlite3_finalize(statement); LeaveCriticalSection(&server->database_lock); sodium_memzero(digest, sizeof(digest)); return result;
}
int drop_logout(drop_server *server, const char *token) {
    unsigned char digest[32]; sqlite3_stmt *statement = NULL; int result = 0;
    crypto_hash_sha256(digest, (const unsigned char *)token, strlen(token));
    EnterCriticalSection(&server->database_lock);
    if (sqlite3_prepare_v2(server->database, "UPDATE devices SET revoked=1 WHERE token_digest=?1", -1, &statement, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(statement, 1, digest, sizeof(digest), SQLITE_TRANSIENT); result = sqlite3_step(statement) == SQLITE_DONE;
    }
    sqlite3_finalize(statement); LeaveCriticalSection(&server->database_lock); sodium_memzero(digest, sizeof(digest)); return result;
}
#if DROP_DIAGNOSTICS
int drop_database_test(const WCHAR *directory) {
    drop_server server; WCHAR root[32768], path[32768], tampered_path[32768]; sqlite3 *database = NULL; sqlite3_stmt *statement = NULL;
    unsigned char *bytes = NULL; DWORD byte_count = 0;
    unsigned char wrong_key[32] = {0}; char device[DROP_ID_CAP], user[DROP_ID_CAP], authorized_device[DROP_ID_CAP];
    cJSON *response = NULL; const char *token; int success = 0;
    if (!drop_path_join(directory, L"encrypted-server", root, 32768) || !drop_server_open(&server, root, 1)) return 0;
    /* The test directory is isolated by the invoking script. Never purge existing accounts. */
    (void)drop_account_create(&server, "test-user", "测试账号", "test-only-password-123");
    (void)drop_account_create(&server, "test-other", "第二测试账号", "test-only-other-password-123");
    (void)drop_account_create(&server, "test-third", "隔离测试账号", "test-only-third-password-123");
    (void)drop_account_create(&server,"test-quota","容量测试账号","test-only-quota-password-123");
    if(!execute(server.database,"UPDATE accounts SET quota_bytes=4096 WHERE login='test-quota'")) goto cleanup;
    drop_id_new(device);
    if (drop_login(&server, "test-user", "wrong-password", device, "test-device", &response)) goto cleanup;
    if (!drop_login(&server, "test-user", "test-only-password-123", device, "测试设备", &response)) goto cleanup;
    token = drop_json_string(response, "token", 64);
    if (!drop_authorize(&server, token, user, authorized_device) || strcmp(device, authorized_device)) goto cleanup;
    if (!execute(server.database, "UPDATE accounts SET enabled=0 WHERE login='test-user'") ||
        drop_authorize(&server, token, user, authorized_device)) goto cleanup;
    if (!execute(server.database, "UPDATE accounts SET enabled=1 WHERE login='test-user'; UPDATE devices SET expires_at=0 WHERE user_id=(SELECT user_id FROM accounts WHERE login='test-user')") ||
        drop_authorize(&server, token, user, authorized_device)) goto cleanup;
    if (!execute(server.database, "UPDATE devices SET expires_at=4102444800 WHERE user_id=(SELECT user_id FROM accounts WHERE login='test-user')") ||
        !drop_authorize(&server, token, user, authorized_device)) goto cleanup;
    if (!drop_logout(&server, token) || drop_authorize(&server, token, user, authorized_device)) goto cleanup;
    if (!drop_path_join(root, L"server.db", path, 32768) || key_database(&database, path, wrong_key)) goto cleanup;
    if (!execute(server.database, "PRAGMA wal_checkpoint(TRUNCATE)")) goto cleanup;
    if (!drop_file_read(path, &bytes, &byte_count, 8 * 1024 * 1024) || byte_count < 4096 ||
        !drop_path_join(root, L"tampered.db", tampered_path, 32768)) goto cleanup;
    bytes[2048] ^= 0x80;
    if (!drop_file_write(tampered_path, bytes, byte_count, 1) || key_database(&database, tampered_path, server.keys.database_key)) goto cleanup;
    free(bytes); bytes = NULL; DeleteFileW(tampered_path);
    if (sqlite3_prepare_v2(server.database, "PRAGMA integrity_check", -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW || strcmp((const char *)sqlite3_column_text(statement, 0), "ok")) goto cleanup;
    sqlite3_finalize(statement); statement = NULL;
    if (sqlite3_prepare_v2(server.database, "PRAGMA cipher_integrity_check", -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_DONE) goto cleanup;
    sqlite3_finalize(statement); statement=NULL;
    {
        int pages; char limit[96],request[DROP_ID_CAP],recipient[DROP_ID_CAP],*body=malloc(DROP_MAX_BODY+1); cJSON *input=NULL,*output=NULL; drop_error error;
        if(!body || sqlite3_prepare_v2(server.database,"PRAGMA page_count",-1,&statement,NULL)!=SQLITE_OK || sqlite3_step(statement)!=SQLITE_ROW) { free(body); goto cleanup; }
        pages=sqlite3_column_int(statement,0); sqlite3_finalize(statement); statement=NULL;
        sprintf_s(limit,sizeof(limit),"PRAGMA max_page_count=%d",pages); memset(body,'x',DROP_MAX_BODY); body[DROP_MAX_BODY]=0; drop_id_new(request);
        input=cJSON_CreateObject();
        if(!execute(server.database,limit) || !input || !cJSON_AddStringToObject(input,"request_id",request) || !cJSON_AddStringToObject(input,"body",body)) { free(body); cJSON_Delete(input); goto cleanup; }
        free(body); error=drop_message_submit(&server,user,device,input,&output,recipient); cJSON_Delete(input); cJSON_Delete(output);
        if(error!=DROP_E_SPACE || !execute(server.database,"PRAGMA max_page_count=2147483646") ||
            sqlite3_prepare_v2(server.database,"SELECT count(*) FROM messages",-1,&statement,NULL)!=SQLITE_OK || sqlite3_step(statement)!=SQLITE_ROW || sqlite3_column_int(statement,0)!=0) goto cleanup;
        sqlite3_finalize(statement); statement=NULL;
    }
    if(!drop_pagination_test(&server,user,device)) goto cleanup;
    success = 1;
cleanup:
    sqlite3_finalize(statement); if (database) sqlite3_close(database); free(bytes); cJSON_Delete(response); drop_server_close(&server);
    if (success) {
        if (!drop_server_open(&server, root, 0)) return 0;
        drop_server_close(&server); puts("PASS server: keyed-first-create wrong-key tamper Argon2id login disabled-account expiry token-revoke integrity reopen SQLite-full atomic rollback");
    }
    return success;
}
int drop_migration_test(const WCHAR *directory) {
    WCHAR root[32768], search[32768], backup[32768]; WIN32_FIND_DATAW item; HANDLE find = INVALID_HANDLE_VALUE;
    drop_server server; sqlite3 *old = NULL; sqlite3_stmt *s = NULL; int ok = 0, matched = 0;
    if (!drop_path_join(directory, L"migration-server", root, 32768) || !drop_server_open(&server, root, 1)) return 0;
    /* Produce a legitimate schema-1 database using the same account and credential schema.
       The fixture has no messages. No production directory is accepted by the test runner. */
    (void)drop_account_create(&server, "migration-user", "迁移账号", "migration-test-password-123");
    if (!execute(server.database, "DROP TABLE cloud_files; DROP TABLE cloud_backups; DROP TABLE backup_jobs; DROP TABLE object_pins; DROP TABLE object_cleanup; DROP TABLE state_events; DROP TABLE state_sequences; DROP TABLE read_positions; DROP TABLE saved_objects; DROP TABLE saved_bodies; DROP TABLE attachments; DROP TABLE chunks; DROP TABLE objects; DROP TABLE events; DROP TABLE messages; DROP TABLE account_sequences; DROP TABLE conversations; PRAGMA user_version=1;")) goto done;
    drop_server_close(&server);
    if (!drop_server_open(&server, root, 0)) return 0;
    if (sqlite3_prepare_v2(server.database, "SELECT count(*) FROM accounts WHERE login='migration-user'", -1, &s, NULL) != SQLITE_OK ||
        sqlite3_step(s) != SQLITE_ROW || sqlite3_column_int(s, 0) != 1) goto done;
    sqlite3_finalize(s); s = NULL;
    if (swprintf_s(search, 32768, L"%s\\migration-v1.*.db", root) < 0) goto done;
    find = FindFirstFileW(search, &item); if (find == INVALID_HANDLE_VALUE) goto done;
    do {
        if (!drop_path_join(root, item.cFileName, backup, 32768) || !key_database(&old, backup, server.keys.database_key)) goto done;
        if (sqlite3_prepare_v2(old, "SELECT count(*) FROM accounts WHERE login='migration-user'", -1, &s, NULL) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW) goto done;
        matched |= sqlite3_column_int(s, 0) == 1;
        sqlite3_finalize(s); s = NULL; sqlite3_close(old); old = NULL;
    } while (FindNextFileW(find, &item));
    if(!matched) goto done;
    /* A schema-2 message used the original recipient/body fingerprint. Migration
       must preserve idempotent retries without permitting changed attachments. */
    {
        char user[DROP_ID_CAP],request[DROP_ID_CAP],recipient[DROP_ID_CAP];
        unsigned char fingerprint[32]; crypto_hash_sha256_state hash;
        cJSON *input=NULL,*output=NULL; drop_error error;
        if(sqlite3_prepare_v2(server.database,"SELECT user_id FROM accounts WHERE login='migration-user'",-1,&s,NULL)!=SQLITE_OK || sqlite3_step(s)!=SQLITE_ROW) goto done;
        strcpy_s(user,sizeof(user),(const char *)sqlite3_column_text(s,0)); sqlite3_finalize(s); s=NULL;
        drop_id_new(request); input=cJSON_CreateObject();
        if(!input || !cJSON_AddStringToObject(input,"request_id",request) || !cJSON_AddStringToObject(input,"body","迁移正文")) { cJSON_Delete(input); goto done; }
        error=drop_message_submit(&server,user,user,input,&output,recipient); cJSON_Delete(output); output=NULL;
        if(error!=DROP_OK) { cJSON_Delete(input); goto done; }
        crypto_hash_sha256_init(&hash); crypto_hash_sha256_update(&hash,(const unsigned char *)user,32);
        crypto_hash_sha256_update(&hash,(const unsigned char *)"迁移正文",strlen("迁移正文")); crypto_hash_sha256_final(&hash,fingerprint);
        if(sqlite3_prepare_v2(server.database,"UPDATE messages SET fingerprint=?1 WHERE request_id=?2",-1,&s,NULL)!=SQLITE_OK) { cJSON_Delete(input); goto done; }
        sqlite3_bind_blob(s,1,fingerprint,32,SQLITE_TRANSIENT); sqlite3_bind_text(s,2,request,-1,SQLITE_TRANSIENT);
        if(sqlite3_step(s)!=SQLITE_DONE) { cJSON_Delete(input); goto done; } sqlite3_finalize(s); s=NULL;
        if(!execute(server.database,"DROP TABLE cloud_files; DROP TABLE cloud_backups; DROP TABLE backup_jobs; DROP TABLE object_pins; DROP TABLE object_cleanup; DROP TABLE state_events; DROP TABLE state_sequences; DROP TABLE read_positions; DROP TABLE saved_objects; DROP TABLE saved_bodies; DROP INDEX messages_server_order; ALTER TABLE messages DROP COLUMN server_order; DROP TABLE attachments; DROP TABLE chunks; DROP TABLE objects; PRAGMA user_version=2;")) { cJSON_Delete(input); goto done; }
        drop_server_close(&server);
        if(!drop_server_open(&server,root,0)) { cJSON_Delete(input); return 0; }
        error=drop_message_submit(&server,user,user,input,&output,recipient);
        if(error!=DROP_OK || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(output,"duplicate"))) { cJSON_Delete(input); cJSON_Delete(output); goto done; }
        cJSON_Delete(output); output=NULL;
        cJSON_ReplaceItemInObjectCaseSensitive(input,"body",cJSON_CreateString("改变正文"));
        error=drop_message_submit(&server,user,user,input,&output,recipient); cJSON_Delete(input); cJSON_Delete(output);
        if(error!=DROP_E_CONFLICT) goto done;
    }
    if(!execute(server.database,"DROP TABLE cloud_files; DROP TABLE cloud_backups; DROP TABLE backup_jobs; DROP TABLE object_pins; DROP TABLE object_cleanup; ALTER TABLE objects DROP COLUMN last_activity; ALTER TABLE objects DROP COLUMN finalize_error; PRAGMA user_version=5;")) goto done;
    drop_server_close(&server); if(!drop_server_open(&server,root,0)) return 0;
    if(sqlite3_prepare_v2(server.database,"SELECT count(*),min(server_order) FROM messages",-1,&s,NULL)!=SQLITE_OK || sqlite3_step(s)!=SQLITE_ROW || sqlite3_column_int(s,0)!=1 || sqlite3_column_int64(s,1)!=1) goto done;
    ok = 1;
done:
    if (find != INVALID_HANDLE_VALUE) FindClose(find); sqlite3_finalize(s); if (old) sqlite3_close(old); drop_server_close(&server);
    if (ok) puts("PASS migration: encrypted schema-1/2/5 rollback, accounts, explicit order and legacy idempotent text retained"); return ok;
}


#endif

#include "server.h"
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
    if (!execute(database, "PRAGMA cipher_memory_security=ON; PRAGMA temp_store=MEMORY;"
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
    if (version == DROP_SCHEMA_VERSION) return 1;
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
int drop_server_open(drop_server *server, const WCHAR *directory, int initialize) {
    WCHAR path[32768], db_path[32768]; unsigned char *secret = NULL; DWORD length = 0; DWORD full_length;
    memset(server, 0, sizeof(*server)); server->lock_file = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&server->database_lock); InitializeCriticalSection(&server->login_lock);
    full_length = GetFullPathNameW(directory, 32768, server->directory, NULL);
    if (!full_length || full_length >= 32768 || !drop_private_directory(server->directory) ||
        !drop_path_join(server->directory, L"run.lock", path, 32768)) goto failure;
    server->lock_file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (server->lock_file == INVALID_HANDLE_VALUE) goto failure;
    if (!drop_path_join(server->directory, L"server.db", db_path, 32768) ||
        !drop_path_join(server->directory, L"runtime-user.dpapi", path, 32768)) goto failure;
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
    if (!key_database(&server->database, db_path, server->keys.database_key) || !migrate(server)) goto failure;
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
    if (server->database) sqlite3_close(server->database);
    if (server->lock_file != INVALID_HANDLE_VALUE) CloseHandle(server->lock_file);
    if (server->stop_event) CloseHandle(server->stop_event);
    if (server->password_slots) CloseHandle(server->password_slots);
    DeleteCriticalSection(&server->database_lock); DeleteCriticalSection(&server->login_lock);
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
int drop_database_test(const WCHAR *directory) {
    drop_server server; WCHAR root[32768], path[32768]; sqlite3 *database = NULL; sqlite3_stmt *statement = NULL;
    unsigned char wrong_key[32] = {0}; char device[DROP_ID_CAP], user[DROP_ID_CAP], authorized_device[DROP_ID_CAP];
    cJSON *response = NULL; const char *token; int success = 0;
    if (!drop_path_join(directory, L"encrypted-server", root, 32768) || !drop_server_open(&server, root, 1)) return 0;
    /* Repeatable test account; existing runs may already contain it. */
    execute(server.database, "DELETE FROM devices; DELETE FROM accounts;");
    if (!drop_account_create(&server, "test-user", "测试账号", "test-only-password-123")) goto cleanup;
    drop_id_new(device);
    if (drop_login(&server, "test-user", "wrong-password", device, "test-device", &response)) goto cleanup;
    if (!drop_login(&server, "test-user", "test-only-password-123", device, "测试设备", &response)) goto cleanup;
    token = drop_json_string(response, "token", 64);
    if (!drop_authorize(&server, token, user, authorized_device) || strcmp(device, authorized_device)) goto cleanup;
    if (!drop_logout(&server, token) || drop_authorize(&server, token, user, authorized_device)) goto cleanup;
    if (!drop_path_join(root, L"server.db", path, 32768) || key_database(&database, path, wrong_key)) goto cleanup;
    if (!execute(server.database, "PRAGMA wal_checkpoint(TRUNCATE)")) goto cleanup;
    if (sqlite3_prepare_v2(server.database, "PRAGMA integrity_check", -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW || strcmp((const char *)sqlite3_column_text(statement, 0), "ok")) goto cleanup;
    sqlite3_finalize(statement); statement = NULL;
    if (sqlite3_prepare_v2(server.database, "PRAGMA cipher_integrity_check", -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_DONE) goto cleanup;
    success = 1;
cleanup:
    sqlite3_finalize(statement); if (database) sqlite3_close(database); cJSON_Delete(response); drop_server_close(&server);
    if (success) {
        if (!drop_server_open(&server, root, 0)) return 0;
        drop_server_close(&server); puts("PASS server: keyed-first-create wrong-key-rejection Argon2id login token-revoke integrity reopen");
    }
    return success;
}

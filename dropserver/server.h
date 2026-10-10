#ifndef DROP_SERVER_H
#define DROP_SERVER_H
#include "drop_common.h"
#define SQLITE_HAS_CODEC 1
#include <sqlcipher/sqlite3.h>
#include <sodium.h>

typedef struct drop_keys {
    char magic[8];
    unsigned char database_key[32];
    unsigned char object_master_key[32];
    char server_id[DROP_ID_CAP];
    char generation[DROP_ID_CAP];
} drop_keys;
typedef struct drop_server {
    sqlite3 *database;
    drop_keys keys;
    WCHAR directory[32768];
    HANDLE lock_file;
    HANDLE stop_event;
    HANDLE password_slots;
    CRITICAL_SECTION database_lock;
    CRITICAL_SECTION login_lock;
    ULONGLONG login_window;
    unsigned login_count;
    char dummy_password_hash[crypto_pwhash_STRBYTES];
} drop_server;
int drop_server_open(drop_server *server, const WCHAR *directory, int initialize);
void drop_server_close(drop_server *server);
int drop_account_create(drop_server *server, const char *login, const char *display, const char *password);
int drop_login(drop_server *server, const char *login, const char *password, const char *device_id,
    const char *device_name, cJSON **response);
int drop_authorize(drop_server *server, const char *token, char user_id[DROP_ID_CAP], char device_id[DROP_ID_CAP]);
int drop_logout(drop_server *server, const char *token);
int drop_server_run(drop_server *server, const char *listen, const char *certificate);
int drop_database_test(const WCHAR *directory);
#endif

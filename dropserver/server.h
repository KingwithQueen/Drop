#ifndef DROP_SERVER_H
#define DROP_SERVER_H
#include "drop_common.h"
#define SQLITE_HAS_CODEC 1
#include <sqlcipher/sqlite3.h>
#include <sodium.h>
#include "object.h"

typedef struct drop_keys {
    char magic[8];
    unsigned char database_key[32];
    unsigned char object_master_key[32];
    char server_id[DROP_ID_CAP];
    char generation[DROP_ID_CAP];
} drop_keys;
_Static_assert(sizeof(drop_keys) == 138, "runtime key format must be architecture independent");
typedef struct drop_server {
    sqlite3 *database;
    drop_keys keys;
    WCHAR directory[32768];
    WCHAR backup_directory[32768];
    unsigned backup_seconds;
    HANDLE backup_thread,backup_request;
    HANDLE cloud_thread;
    HANDLE finalize_threads[2];
    CRITICAL_SECTION finalize_lock;
    HANDLE lock_file;
    HANDLE stop_event;
    HANDLE password_slots;
    CRITICAL_SECTION database_lock;
    CRITICAL_SECTION login_lock;
    CRITICAL_SECTION peers_lock;
    CRITICAL_SECTION objects_lock;
    struct drop_peer *peers;
    ULONGLONG login_window;
    unsigned login_count;
    int use_zstd,zstd_level;
    unsigned connection_timeout_seconds;
    HANDLE orphan_scan;
    WIN32_FIND_DATAW orphan_item;
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
int drop_admin_account(drop_server *server,const char *login,int enabled,const char *new_password);
int drop_admin_revoke(drop_server *server,const char *login,const char *device);
int drop_runtime_machine(drop_server *server);
int drop_service_command(int argc,WCHAR **argv);
int drop_server_command(int argc,WCHAR **argv);
void drop_service_ready(drop_server *server);
int drop_recovery_export(drop_server *server,const WCHAR *path,const char *password);
int drop_backup_create(drop_server *server,const WCHAR *target,char task_id[DROP_ID_CAP]);
int drop_backup_restore(const WCHAR *backup,const WCHAR *package,const WCHAR *target,const char *password);
int drop_backup_test(const WCHAR *root);
int drop_backup_schema(drop_server *server);
int drop_finalize_schema(drop_server *server);
void drop_finalize_stop(drop_server *server);
drop_error drop_upload_finish_verify(drop_server *server,const char *user,const char *id,cJSON **output,int asynchronous);
int drop_backup_start(drop_server *server);
void drop_backup_stop(drop_server *server);
int drop_backup_trigger(const WCHAR *directory);
int drop_database_test(const WCHAR *directory);
int drop_migration_test(const WCHAR *directory);
int drop_pagination_test(drop_server *server,const char *user,const char *device);
int drop_text_schema(drop_server *server);
int drop_schema_snapshot(drop_server *server, int version);
int drop_objects_schema(drop_server *server);
int drop_receipts_schema(drop_server *server);
int drop_maintenance_schema(drop_server *server);
drop_error drop_upload_cleanup(drop_server *server,uint64_t now);
int drop_maintenance_test(const WCHAR *root);
drop_error drop_account_capacity(drop_server *server,const char *user,uint64_t extra);
drop_error drop_saved_submit(drop_server *server,const char *user,const char *device,const cJSON *input,cJSON **output);
drop_error drop_read_submit(drop_server *server,const char *user,const cJSON *input,cJSON **output);
drop_error drop_states_read(drop_server *server,const char *user,const cJSON *input,cJSON **output);
drop_error drop_upload_create(drop_server *server,const char *user,const cJSON *input,cJSON **output);
drop_error drop_upload_status(drop_server *server,const char *user,const char *id,cJSON **output);
drop_error drop_upload_chunk(drop_server *server,const char *user,const char *id,uint64_t index,const unsigned char *data,DWORD length);
drop_error drop_upload_finish(drop_server *server,const char *user,const char *id,cJSON **output);
drop_error drop_attachment_chunk(drop_server *server,const char *user,const char *id,uint64_t index,unsigned char **data,DWORD *length);
int drop_attachment_list(drop_server *server,const char *message,cJSON *array);
drop_error drop_message_submit(drop_server *server, const char *user, const char *device,
    const cJSON *input, cJSON **output, char recipient[DROP_ID_CAP]);
drop_error drop_sync_read(drop_server *server, const char *user, const cJSON *input, cJSON **output);
drop_error drop_account_lookup(drop_server *server, const cJSON *input, cJSON **output);
void drop_notify(drop_server *server, const char *user, const char *recipient);
void drop_notify_states(drop_server *server);
#endif

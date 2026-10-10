#include "server.h"
#include <string.h>
#include <stdlib.h>

int drop_admin_account(drop_server *server,const char *login,int enabled,const char *password) {
    sqlite3_stmt *s=NULL; char hash[crypto_pwhash_STRBYTES]={0}; int ok=0;
    if(!login || (password && (strlen(password)<12 || strlen(password)>1024 || !drop_utf8_valid(password,strlen(password))))) return 0;
    if(password && crypto_pwhash_str_alg(hash,password,strlen(password),crypto_pwhash_OPSLIMIT_INTERACTIVE,crypto_pwhash_MEMLIMIT_INTERACTIVE,crypto_pwhash_ALG_ARGON2ID13)) return 0;
    EnterCriticalSection(&server->database_lock);
    if(sqlite3_exec(server->database,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK) goto done;
    if(sqlite3_prepare_v2(server->database,password?"UPDATE accounts SET password_hash=?1,failed_logins=0,locked_until=0 WHERE login=?2":"UPDATE accounts SET enabled=?1 WHERE login=?2",-1,&s,NULL)!=SQLITE_OK) goto rollback;
    if(password) sqlite3_bind_text(s,1,hash,-1,SQLITE_TRANSIENT); else sqlite3_bind_int(s,1,enabled);
    sqlite3_bind_text(s,2,login,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(s)!=SQLITE_DONE || sqlite3_changes(server->database)!=1) goto rollback;
    sqlite3_finalize(s); s=NULL;
    if(password || !enabled) {
        if(sqlite3_prepare_v2(server->database,"UPDATE devices SET revoked=1 WHERE user_id=(SELECT user_id FROM accounts WHERE login=?1)",-1,&s,NULL)!=SQLITE_OK) goto rollback;
        sqlite3_bind_text(s,1,login,-1,SQLITE_TRANSIENT); if(sqlite3_step(s)!=SQLITE_DONE) goto rollback;
    }
    sqlite3_finalize(s); s=NULL; ok=sqlite3_exec(server->database,"COMMIT",NULL,NULL,NULL)==SQLITE_OK;
rollback:
    if(!ok) sqlite3_exec(server->database,"ROLLBACK",NULL,NULL,NULL);
done:
    sqlite3_finalize(s); LeaveCriticalSection(&server->database_lock); sodium_memzero(hash,sizeof(hash)); return ok;
}
int drop_admin_revoke(drop_server *server,const char *login,const char *device) {
    sqlite3_stmt *s=NULL; int ok=0;
    if(!drop_id_valid(device)) return 0;
    EnterCriticalSection(&server->database_lock);
    if(sqlite3_prepare_v2(server->database,"UPDATE devices SET revoked=1 WHERE device_id=?1 AND user_id=(SELECT user_id FROM accounts WHERE login=?2)",-1,&s,NULL)==SQLITE_OK) {
        sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,2,login,-1,SQLITE_TRANSIENT);
        ok=sqlite3_step(s)==SQLITE_DONE && sqlite3_changes(server->database)==1;
    } sqlite3_finalize(s); LeaveCriticalSection(&server->database_lock); return ok;
}
int drop_runtime_machine(drop_server *server) {
    WCHAR *path=malloc(32768*sizeof(WCHAR)),*old=malloc(32768*sizeof(WCHAR)),*cloud_old=malloc(32768*sizeof(WCHAR)),*cloud_new=malloc(32768*sizeof(WCHAR)); unsigned char *secret=NULL; DWORD size=0; int ok=0,has_cloud=0;
    if(!path || !old || !drop_path_join(server->directory,L"runtime-machine.dpapi",path,32768) || !drop_path_join(server->directory,L"runtime-user.dpapi",old,32768)) goto done;
    if(!cloud_old || !cloud_new || !drop_path_join(server->directory,L"drive-user.dpapi",cloud_old,32768) || !drop_path_join(server->directory,L"drive-machine.dpapi",cloud_new,32768)) goto done;
    has_cloud=GetFileAttributesW(cloud_old)!=INVALID_FILE_ATTRIBUTES;
    if(has_cloud && !drop_secret_load(cloud_old,&secret,&size)) goto done;
    if(!drop_machine_tree(server->directory)) goto done;
    if(has_cloud && !drop_machine_secret_replace(cloud_new,secret,size)) goto done;
    if(GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES && !drop_machine_secret_save(path,&server->keys,sizeof(server->keys))) goto done;
    if(!drop_machine_tree(server->directory) || (has_cloud && !DeleteFileW(cloud_old))) goto done;
    ok=GetFileAttributesW(old)==INVALID_FILE_ATTRIBUTES || DeleteFileW(old)!=0;
done:
    if(secret) { sodium_memzero(secret,size); free(secret); } free(path); free(old); free(cloud_old); free(cloud_new); return ok;
}

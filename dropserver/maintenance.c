#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define GRACE_SECONDS UINT64_C(604800)

static int sql(sqlite3 *db,const char *text) { return sqlite3_exec(db,text,NULL,NULL,NULL)==SQLITE_OK; }
static int prepare(sqlite3 *db,sqlite3_stmt **s,const char *text) { sqlite3_finalize(*s); *s=NULL; return sqlite3_prepare_v2(db,text,-1,s,NULL)==SQLITE_OK; }
static void bind(sqlite3_stmt *s,int i,const char *text) { sqlite3_bind_text(s,i,text,-1,SQLITE_TRANSIENT); }
int drop_maintenance_schema(drop_server *server) {
    sqlite3_stmt *s=NULL; int version=-1;
    if(prepare(server->database,&s,"PRAGMA user_version") && sqlite3_step(s)==SQLITE_ROW) version=sqlite3_column_int(s,0);
    sqlite3_finalize(s); if(version>=6 && version<=DROP_SCHEMA_VERSION) return 1; if(version!=5 || !drop_schema_snapshot(server,5)) return 0;
    if(!sql(server->database,"BEGIN IMMEDIATE; ALTER TABLE objects ADD COLUMN last_activity INTEGER NOT NULL DEFAULT 0;"
        "UPDATE objects SET last_activity=created_at; CREATE TABLE object_cleanup(object_id TEXT PRIMARY KEY);"
        "CREATE TABLE object_pins(object_id TEXT NOT NULL REFERENCES objects(object_id),task_id TEXT NOT NULL,PRIMARY KEY(object_id,task_id));"
        "PRAGMA user_version=6; COMMIT;")) { sql(server->database,"ROLLBACK"); return 0; }
    return 1;
}
static int directory(drop_server *server,const char *id,WCHAR path[32768]) {
    WCHAR root[32768],wide[DROP_ID_CAP];
    return drop_id_valid(id) && drop_to_wide(id,wide,DROP_ID_CAP) && drop_path_join(server->directory,L"objects",root,32768) && drop_path_join(root,wide,path,32768);
}
static int known_file(const WCHAR *name) {
    size_t length=wcslen(name),i;
    if(!wcscmp(name,L"manifest.bin")) return 1;
    if(length>=22 && !wcsncmp(name+16,L".chunk",6)) {
        for(i=0;i<16;++i) if(!((name[i]>=L'0' && name[i]<=L'9') || (name[i]>=L'a' && name[i]<=L'f'))) return 0;
        if(length==22) return 1;
        name+=22; length-=22;
    } else if(length>12 && !wcsncmp(name,L"manifest.bin",12)) { name+=12; length-=12; }
    else return 0;
    if(length!=37 || name[0]!=L'.' || wcscmp(name+33,L".tmp")) return 0;
    for(i=1;i<=32;++i) if(!((name[i]>=L'0' && name[i]<=L'9') || (name[i]>=L'a' && name[i]<=L'f'))) return 0;
    return 1;
}
static int remove_files(drop_server *server,const char *id) {
    WCHAR path[32768],search[32768],file[32768]; HANDLE find; WIN32_FIND_DATAW item; DWORD attributes; int ok=1;
    if(!directory(server,id,path)) return 0;
    attributes=GetFileAttributesW(path); if(attributes==INVALID_FILE_ATTRIBUTES) return GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND;
    if(!(attributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes&FILE_ATTRIBUTE_REPARSE_POINT) || !drop_path_join(path,L"*",search,32768)) return 0;
    find=FindFirstFileW(search,&item); if(find==INVALID_HANDLE_VALUE) return 0;
    /* Inspect every child first; never recursively traverse an unexpected path. */
    do {
        if(!wcscmp(item.cFileName,L".") || !wcscmp(item.cFileName,L"..")) continue;
        if((item.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || !known_file(item.cFileName)) { ok=0; break; }
    } while(FindNextFileW(find,&item)); FindClose(find); if(!ok) return 0;
    find=FindFirstFileW(search,&item); if(find==INVALID_HANDLE_VALUE) return 0;
    do {
        if(!wcscmp(item.cFileName,L".") || !wcscmp(item.cFileName,L"..")) continue;
        if((item.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || !known_file(item.cFileName) || !drop_path_join(path,item.cFileName,file,32768) || !DeleteFileW(file)) { ok=0; break; }
    } while(FindNextFileW(find,&item)); FindClose(find);
    return ok && RemoveDirectoryW(path);
}
static int queue_unindexed(drop_server *server,uint64_t now) {
    WCHAR root[32768],pattern[32768]; sqlite3_stmt *s=NULL; unsigned examined=0; int ok=1;
    if(!server->orphan_scan) {
        if(!drop_path_join(server->directory,L"objects",root,32768) || !drop_path_join(root,L"*",pattern,32768)) return 0;
        server->orphan_scan=FindFirstFileW(pattern,&server->orphan_item);
        if(server->orphan_scan==INVALID_HANDLE_VALUE) { server->orphan_scan=NULL; return GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND; }
    }
    while(server->orphan_scan && examined++<128) {
        WIN32_FIND_DATAW *item=&server->orphan_item; ULARGE_INTEGER ticks; uint64_t seconds=now; char id[DROP_ID_CAP];
        ticks.LowPart=item->ftLastWriteTime.dwLowDateTime; ticks.HighPart=item->ftLastWriteTime.dwHighDateTime;
        if(ticks.QuadPart>=UINT64_C(116444736000000000)) seconds=(ticks.QuadPart-UINT64_C(116444736000000000))/UINT64_C(10000000);
        if((item->dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(item->dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
            seconds<now-GRACE_SECONDS && drop_to_utf8(item->cFileName,id,sizeof(id)) && drop_id_valid(id)) {
            if(!prepare(server->database,&s,"INSERT OR IGNORE INTO object_cleanup SELECT ?1 WHERE NOT EXISTS(SELECT 1 FROM objects WHERE object_id=?1)")) { ok=0; break; }
            bind(s,1,id); if(sqlite3_step(s)!=SQLITE_DONE) { ok=0; break; }
        }
        if(!FindNextFileW(server->orphan_scan,item)) {
            DWORD error=GetLastError(); FindClose(server->orphan_scan); server->orphan_scan=NULL;
            if(error!=ERROR_NO_MORE_FILES) ok=0;
        }
    }
    sqlite3_finalize(s); return ok;
}
drop_error drop_upload_cleanup(drop_server *server,uint64_t now) {
    sqlite3_stmt *s=NULL; char ids[100][DROP_ID_CAP]; int count=0,i,step; drop_error error=DROP_E_DATABASE;
    if(now<GRACE_SECONDS || now>INT64_MAX) return DROP_E_ARGUMENT;
    EnterCriticalSection(&server->objects_lock); EnterCriticalSection(&server->database_lock);
    if(!sql(server->database,"BEGIN IMMEDIATE")) goto unlock;
    if(!queue_unindexed(server,now)) goto rollback;
    if(!prepare(server->database,&s,"INSERT OR IGNORE INTO object_cleanup SELECT object_id FROM objects o WHERE last_activity<?1 AND state!='validating' AND NOT EXISTS(SELECT 1 FROM attachments a WHERE a.object_id=o.object_id) AND NOT EXISTS(SELECT 1 FROM object_pins p WHERE p.object_id=o.object_id) LIMIT 100")) goto rollback;
    sqlite3_bind_int64(s,1,(sqlite3_int64)(now-GRACE_SECONDS)); if(sqlite3_step(s)!=SQLITE_DONE) goto rollback;
    if(!prepare(server->database,&s,"DELETE FROM chunks WHERE object_id IN(SELECT object_id FROM object_cleanup)")) goto rollback;
    if(sqlite3_step(s)!=SQLITE_DONE || !sql(server->database,"DELETE FROM objects WHERE object_id IN(SELECT object_id FROM object_cleanup); COMMIT;")) goto rollback;
    error=DROP_OK; goto unlock;
rollback:
    if(sqlite3_errcode(server->database)==SQLITE_FULL) error=DROP_E_SPACE; sql(server->database,"ROLLBACK");
unlock:
    sqlite3_finalize(s); s=NULL; LeaveCriticalSection(&server->database_lock);
    if(error!=DROP_OK) goto done;
    EnterCriticalSection(&server->database_lock);
    if(prepare(server->database,&s,"SELECT object_id FROM object_cleanup LIMIT 100")) {
        while((step=sqlite3_step(s))==SQLITE_ROW && count<100) strcpy_s(ids[count++],DROP_ID_CAP,(const char *)sqlite3_column_text(s,0));
        if(step!=SQLITE_DONE) error=DROP_E_DATABASE;
    } else error=DROP_E_DATABASE;
    sqlite3_finalize(s); s=NULL; LeaveCriticalSection(&server->database_lock);
    if(error!=DROP_OK) goto done;
    for(i=0;i<count;++i) {
        if(!remove_files(server,ids[i])) { error=drop_io_error(); continue; }
        EnterCriticalSection(&server->database_lock);
        if(prepare(server->database,&s,"DELETE FROM object_cleanup WHERE object_id=?1")) { bind(s,1,ids[i]); if(sqlite3_step(s)!=SQLITE_DONE) error=DROP_E_DATABASE; }
        else error=DROP_E_DATABASE;
        sqlite3_finalize(s); s=NULL; LeaveCriticalSection(&server->database_lock);
    }
done:
    sqlite3_finalize(s); LeaveCriticalSection(&server->objects_lock); return error;
}
#if DROP_DIAGNOSTICS
int drop_maintenance_test(const WCHAR *root) {
    WCHAR path[32768],name[80],wide[DROP_ID_CAP],orphan_path[32768],extra[32768]; drop_server server; sqlite3_stmt *s=NULL;
    char user[DROP_ID_CAP],device[DROP_ID_CAP],ids[4][DROP_ID_CAP],request[DROP_ID_CAP],recipient[DROP_ID_CAP],orphan[DROP_ID_CAP];
    int i,ok=0; cJSON *input=NULL,*out=NULL; uint64_t now=(uint64_t)time(NULL);
    drop_id_new(request);
    if(!drop_to_wide(request,wide,DROP_ID_CAP) || swprintf_s(name,80,L"maintenance-server.%s",wide)<0 || !drop_path_join(root,name,path,32768) || !drop_server_open(&server,path,1)) return 0;
    if(!drop_account_create(&server,"cleanup-user","cleanup-user","cleanup-test-password-123") || !prepare(server.database,&s,"SELECT user_id FROM accounts WHERE login='cleanup-user'") || sqlite3_step(s)!=SQLITE_ROW) goto done;
    strcpy_s(user,sizeof(user),(const char *)sqlite3_column_text(s,0)); sqlite3_finalize(s); s=NULL; drop_id_new(device);
    for(i=0;i<4;++i) {
        drop_id_new(request); input=cJSON_CreateObject();
        if(!input || !cJSON_AddStringToObject(input,"request_id",request) || !cJSON_AddStringToObject(input,"filename","empty.bin") ||
            !cJSON_AddStringToObject(input,"total_size","0") || !cJSON_AddStringToObject(input,"sha256","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") || drop_upload_create(&server,user,input,&out)!=DROP_OK) goto done;
        strcpy_s(ids[i],DROP_ID_CAP,drop_json_string(out,"object_id",32)); cJSON_Delete(input); input=NULL; cJSON_Delete(out); out=NULL;
        if(drop_upload_finish(&server,user,ids[i],&out)!=DROP_OK) goto done; cJSON_Delete(out); out=NULL;
    }
    input=cJSON_CreateObject(); drop_id_new(request);
    if(!input || !cJSON_AddStringToObject(input,"request_id",request) || !cJSON_AddStringToObject(input,"body","") ||
        !cJSON_AddItemToArray(cJSON_AddArrayToObject(input,"attachments"),cJSON_CreateString(ids[2])) || drop_message_submit(&server,user,device,input,&out,recipient)!=DROP_OK) goto done;
    cJSON_Delete(input); input=NULL; cJSON_Delete(out); out=NULL;
    if(!prepare(server.database,&s,"UPDATE objects SET last_activity=?1 WHERE object_id!=?2")) goto done;
    sqlite3_bind_int64(s,1,(sqlite3_int64)(now-GRACE_SECONDS-10)); bind(s,2,ids[0]); if(sqlite3_step(s)!=SQLITE_DONE) goto done;
    if(!prepare(server.database,&s,"INSERT INTO object_pins VALUES(?1,?2)")) goto done;
    bind(s,1,ids[3]); bind(s,2,request); if(sqlite3_step(s)!=SQLITE_DONE) goto done;
    sqlite3_finalize(s); s=NULL;
    {
        drop_object object={0}; unsigned char empty_hash[32]; HANDLE folder; ULARGE_INTEGER ticks; FILETIME old;
        crypto_hash_sha256(empty_hash,(const unsigned char *)"",0); drop_id_new(orphan);
        if(!directory(&server,orphan,orphan_path) || !drop_private_directory(orphan_path) || !drop_path_join(orphan_path,L"manifest.bin",path,32768) ||
            !drop_path_join(orphan_path,L"extra.bin",extra,32768) || !drop_object_create(server.keys.object_master_key,orphan,0,empty_hash,&object)) goto done;
        if(!drop_file_write(path,object.header,sizeof(object.header),0) || !drop_file_write(extra,object.header,sizeof(object.header),0)) { sodium_memzero(&object,sizeof(object)); goto done; }
        sodium_memzero(&object,sizeof(object)); ticks.QuadPart=(now-GRACE_SECONDS-10+UINT64_C(11644473600))*UINT64_C(10000000);
        old.dwLowDateTime=ticks.LowPart; old.dwHighDateTime=ticks.HighPart;
        folder=CreateFileW(orphan_path,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
        if(folder==INVALID_HANDLE_VALUE) goto done;
        i=SetFileTime(folder,NULL,NULL,&old)!=0; CloseHandle(folder); if(!i) goto done;
    }
    if(drop_upload_cleanup(&server,now)!=DROP_E_IO || GetFileAttributesW(extra)==INVALID_FILE_ATTRIBUTES || !DeleteFileW(extra) || drop_upload_cleanup(&server,now)!=DROP_OK || GetFileAttributesW(orphan_path)!=INVALID_FILE_ATTRIBUTES) goto done;
    if(!prepare(server.database,&s,"SELECT count(*) FROM objects") || sqlite3_step(s)!=SQLITE_ROW || sqlite3_column_int(s,0)!=3) goto done;
    if(!directory(&server,ids[1],path) || GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES) goto done;
    sqlite3_finalize(s); s=NULL;
    if(!sql(server.database,"DELETE FROM object_pins") || drop_upload_cleanup(&server,now)!=DROP_OK ||
        !prepare(server.database,&s,"SELECT count(*) FROM objects") || sqlite3_step(s)!=SQLITE_ROW || sqlite3_column_int(s,0)!=2) goto done;
    ok=1;
done:
    sqlite3_finalize(s); cJSON_Delete(input); cJSON_Delete(out); drop_server_close(&server);
    if(ok) puts("PASS cleanup: grace expiry, unindexed crash files, unknown-file refusal, backup pins and release; active uploads and historical attachments retained"); return ok;
}

#endif

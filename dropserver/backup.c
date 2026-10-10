#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define RECOVERY_SIZE (8+16+24+sizeof(drop_keys)+16)
#define MANIFEST_PLAIN 128
#define MANIFEST_SIZE (8+24+MANIFEST_PLAIN+16)

static int sql(sqlite3 *db,const char *text) { return sqlite3_exec(db,text,NULL,NULL,NULL)==SQLITE_OK; }
static void bind(sqlite3_stmt *s,int n,const char *text) { sqlite3_bind_text(s,n,text,-1,SQLITE_TRANSIENT); }
static void put64(unsigned char *p,uint64_t n) { int i; for(i=0;i<8;i++) p[i]=(unsigned char)(n>>(8*i)); }
static uint64_t get64(const unsigned char *p) { uint64_t n=0; int i; for(i=0;i<8;i++) n|=(uint64_t)p[i]<<(8*i); return n; }
static int database_open(sqlite3 **db,const WCHAR *path,const unsigned char key[32],int mode) {
    char *utf8=malloc(131072); int ok=0;
    if(utf8 && drop_to_utf8(path,utf8,131072) && sqlite3_open_v2(utf8,db,(mode?SQLITE_OPEN_READWRITE:SQLITE_OPEN_READONLY)|(mode==1?SQLITE_OPEN_CREATE:0)|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK &&
        sqlite3_key(*db,key,32)==SQLITE_OK && sql(*db,"PRAGMA cipher_log_source=NONE; PRAGMA cipher_memory_security=ON; PRAGMA temp_store=MEMORY;") &&
        (!mode || sql(*db,"PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"))) ok=1;
    free(utf8); return ok;
}
static int integrity(sqlite3 *db) {
    sqlite3_stmt *s=NULL; int ok=0;
    if(sqlite3_prepare_v2(db,"PRAGMA integrity_check",-1,&s,NULL)==SQLITE_OK && sqlite3_step(s)==SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(s,0),"ok")) ok=1;
    sqlite3_finalize(s); return ok;
}
int drop_backup_schema(drop_server *server) {
    sqlite3_stmt *s=NULL; int version=-1;
    if(sqlite3_prepare_v2(server->database,"PRAGMA user_version",-1,&s,NULL)==SQLITE_OK && sqlite3_step(s)==SQLITE_ROW) version=sqlite3_column_int(s,0);
    sqlite3_finalize(s); if(version>=7 && version<=DROP_SCHEMA_VERSION) return 1;
    if(version!=6 || !drop_schema_snapshot(server,6)) return 0;
    if(!sql(server->database,"BEGIN IMMEDIATE; CREATE TABLE backup_jobs(task_id TEXT PRIMARY KEY,target TEXT NOT NULL,state TEXT NOT NULL,created_at INTEGER NOT NULL,completed_at INTEGER,error TEXT NOT NULL DEFAULT ''); PRAGMA user_version=7; COMMIT;")) { sql(server->database,"ROLLBACK"); return 0; } return 1;
}
int drop_recovery_export(drop_server *server,const WCHAR *path,const char *password) {
    unsigned char package[RECOVERY_SIZE],key[32]; unsigned long long length; int ok=0;
    if(!password || strlen(password)<16 || strlen(password)>1024 || !drop_utf8_valid(password,strlen(password))) return 0;
    memcpy(package,"DROPRK01",8); randombytes_buf(package+8,16); randombytes_buf(package+24,24);
    if(crypto_pwhash(key,32,password,strlen(password),package+8,2,64*1024*1024,crypto_pwhash_ALG_ARGON2ID13)) goto done;
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(package+48,&length,(const unsigned char *)&server->keys,sizeof(server->keys),package,24,NULL,package+24,key) || length!=sizeof(server->keys)+16) goto done;
    ok=drop_file_write(path,package,sizeof(package),0);
done:
    sodium_memzero(key,sizeof(key)); sodium_memzero(package,sizeof(package)); return ok;
}
static int recovery_load(const WCHAR *path,const char *password,drop_keys *keys) {
    unsigned char *package=NULL,key[32]; DWORD size=0; unsigned long long length; int ok=0;
    if(!password || strlen(password)>1024 || !drop_file_read(path,&package,&size,RECOVERY_SIZE) || size!=RECOVERY_SIZE || memcmp(package,"DROPRK01",8)) goto done;
    if(crypto_pwhash(key,32,password,strlen(password),package+8,2,64*1024*1024,crypto_pwhash_ALG_ARGON2ID13) ||
        crypto_aead_xchacha20poly1305_ietf_decrypt((unsigned char *)keys,&length,NULL,package+48,size-48,package,24,package+24,key) || length!=sizeof(*keys) ||
        memcmp(keys->magic,"DROPKEY1",8) || keys->server_id[32] || keys->generation[32] || !drop_id_valid(keys->server_id) || !drop_id_valid(keys->generation)) goto done;
    ok=1;
done:
    if(package) { sodium_memzero(package,size); free(package); } sodium_memzero(key,sizeof(key)); if(!ok) sodium_memzero(keys,sizeof(*keys)); return ok;
}
static int paths(const WCHAR *base,const char *id,const WCHAR *name,WCHAR *out,int create) {
    WCHAR *objects=malloc(32768*sizeof(WCHAR)),*directory=malloc(32768*sizeof(WCHAR)),wide[DROP_ID_CAP]; int ok=0;
    if(objects && directory && drop_id_valid(id) && drop_to_wide(id,wide,DROP_ID_CAP) && drop_path_join(base,L"objects",objects,32768) && drop_path_join(objects,wide,directory,32768) &&
        (!create || (drop_private_directory(objects) && drop_private_directory(directory))) && drop_path_join(directory,name,out,32768)) ok=1;
    free(objects); free(directory); return ok;
}
static int copy_verified(const WCHAR *from,const WCHAR *to) {
    unsigned char source_hash[32],target_hash[32]; uint64_t source_size,target_size; WCHAR *temporary=calloc(32768,sizeof(WCHAR)); WCHAR wide[DROP_ID_CAP]; char id[DROP_ID_CAP]; HANDLE file=INVALID_HANDLE_VALUE; int ok=0;
    if(!temporary || !drop_hash_file(from,source_hash,&source_size)) goto done;
    if(GetFileAttributesW(to)!=INVALID_FILE_ATTRIBUTES) {
        ok=drop_hash_file(to,target_hash,&target_size) && target_size==source_size && !sodium_memcmp(target_hash,source_hash,32); goto done;
    }
    drop_id_new(id);
    if(!drop_to_wide(id,wide,DROP_ID_CAP) || swprintf_s(temporary,32768,L"%s.%s.tmp",to,wide)<0 || !CopyFileW(from,temporary,TRUE) || !drop_hash_file(temporary,target_hash,&target_size) || target_size!=source_size || sodium_memcmp(source_hash,target_hash,32)) goto done;
    file=CreateFileW(temporary,GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL); if(file==INVALID_HANDLE_VALUE || !FlushFileBuffers(file)) goto done;
    CloseHandle(file); file=INVALID_HANDLE_VALUE; ok=MoveFileExW(temporary,to,MOVEFILE_WRITE_THROUGH)!=0;
done:
    if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); if(!ok && temporary && *temporary) DeleteFileW(temporary); free(temporary); return ok;
}
static int object_verify(const WCHAR *root,const drop_keys *keys,const void *header,int length) {
    drop_object object={0}; WCHAR *path=malloc(32768*sizeof(WCHAR)),name[64]; unsigned char *bytes=NULL,*plain=NULL,plain_hash[32],cipher_hash[32]; DWORD size=0,plain_size=0; uint64_t i,total=0;
    crypto_hash_sha256_state plain_state,cipher_state; int ok=0;
    if(!path || !drop_object_open(keys->object_master_key,header,(size_t)length,&object) || !paths(root,object.id,L"manifest.bin",path,0) ||
        !drop_file_read(path,&bytes,&size,DROP_OBJECT_HEADER_SIZE) || size!=DROP_OBJECT_HEADER_SIZE || sodium_memcmp(bytes,header,size)) goto done;
    free(bytes); bytes=NULL; crypto_hash_sha256_init(&plain_state); crypto_hash_sha256_init(&cipher_state);
    for(i=0;i<object.chunks;i++) {
        swprintf_s(name,64,L"%016llx.chunk",(unsigned long long)i);
        if(!paths(root,object.id,name,path,0) || !drop_file_read(path,&bytes,&size,DROP_CHUNK_SIZE+DROP_CHUNK_OVERHEAD) || !drop_chunk_decrypt(&object,i,bytes,size,&plain,&plain_size)) goto done;
        { unsigned char chunk_hash[32]; crypto_hash_sha256(chunk_hash,bytes,size); crypto_hash_sha256_update(&cipher_state,chunk_hash,32); }
        crypto_hash_sha256_update(&plain_state,plain,plain_size); total+=plain_size;
        sodium_memzero(plain,plain_size); free(plain); plain=NULL; free(bytes); bytes=NULL;
    }
    crypto_hash_sha256_final(&plain_state,plain_hash); crypto_hash_sha256_final(&cipher_state,cipher_hash);
    ok=total==object.size && !sodium_memcmp(plain_hash,object.sha256,32) && !sodium_memcmp(cipher_hash,object.ciphertext_digest,32);
done:
    if(plain) { sodium_memzero(plain,plain_size); free(plain); } free(bytes); free(path); sodium_memzero(&object,sizeof(object)); return ok;
}
int drop_backup_create(drop_server *server,const WCHAR *target,char task[DROP_ID_CAP]) {
    WCHAR *root=calloc(32768,sizeof(WCHAR)),*snapshot=calloc(32768,sizeof(WCHAR)),*from=calloc(32768,sizeof(WCHAR)),*to=calloc(32768,sizeof(WCHAR)),wide[DROP_ID_CAP],name[64];
    char *utf8=malloc(131072); WCHAR *building=calloc(32768,sizeof(WCHAR)); sqlite3 *db=NULL; sqlite3_stmt *s=NULL; sqlite3_backup *backup=NULL;
    unsigned char manifest[MANIFEST_SIZE],hash[32],payload[MANIFEST_PLAIN]; uint64_t size,count=0,i; unsigned long long cipher_size; int ok=0,locked=0,phase=1; DWORD n;
    if(!root || !snapshot || !from || !to || !utf8 || !building) goto done;
    n=GetFullPathNameW(target,32768,root,NULL);
    if(!n || n>=32768 || !drop_to_utf8(root,utf8,131072) || !drop_private_directory(root)) goto done;
    /* A retry reuses the keyed snapshot and pinned immutable objects. */
    EnterCriticalSection(&server->objects_lock); EnterCriticalSection(&server->database_lock); locked=1;
    if(sqlite3_prepare_v2(server->database,"SELECT task_id FROM backup_jobs WHERE target=?1 AND state!='complete' ORDER BY created_at LIMIT 1",-1,&s,NULL)!=SQLITE_OK) goto done;
    bind(s,1,utf8);
    if(sqlite3_step(s)==SQLITE_ROW) strcpy_s(task,DROP_ID_CAP,(const char *)sqlite3_column_text(s,0)); else drop_id_new(task);
    sqlite3_finalize(s); s=NULL;
    if(!drop_to_wide(task,wide,DROP_ID_CAP) || !drop_path_join(root,wide,to,32768) || !drop_private_directory(to) || !drop_path_join(to,L"snapshot.db",snapshot,32768)) goto done;
    if(sqlite3_prepare_v2(server->database,"INSERT INTO backup_jobs(task_id,target,state,created_at) VALUES(?1,?2,'building',?3) ON CONFLICT(task_id) DO NOTHING",-1,&s,NULL)!=SQLITE_OK) goto done;
    bind(s,1,task); bind(s,2,utf8); sqlite3_bind_int64(s,3,(sqlite3_int64)time(NULL)); if(sqlite3_step(s)!=SQLITE_DONE) goto done;
    sqlite3_finalize(s); s=NULL;
    phase=2; if(GetFileAttributesW(snapshot)==INVALID_FILE_ATTRIBUTES) {
        if(sqlite3_prepare_v2(server->database,"INSERT INTO object_pins SELECT object_id,?1 FROM objects WHERE state='complete' ON CONFLICT DO NOTHING",-1,&s,NULL)!=SQLITE_OK) goto done;
        bind(s,1,task); if(sqlite3_step(s)!=SQLITE_DONE) goto done; sqlite3_finalize(s); s=NULL;
        if(!drop_path_join(to,L"snapshot.building.db",building,32768)) goto done;
        if(GetFileAttributesW(building)!=INVALID_FILE_ATTRIBUTES && !DeleteFileW(building)) goto done;
        phase=3; if(!database_open(&db,building,server->keys.database_key,1)) goto done;
        backup=sqlite3_backup_init(db,"main",server->database,"main");
        phase=4; if(!backup || sqlite3_backup_step(backup,-1)!=SQLITE_DONE) goto done;
        if(sqlite3_backup_finish(backup)!=SQLITE_OK) { backup=NULL; goto done; } backup=NULL;
        if(!integrity(db) || !sql(db,"PRAGMA journal_mode=DELETE")) goto done;
        sqlite3_close(db); db=NULL;
        { HANDLE file=CreateFileW(building,GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL); int flushed=file!=INVALID_HANDLE_VALUE && FlushFileBuffers(file); if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); if(!flushed || !MoveFileExW(building,snapshot,MOVEFILE_WRITE_THROUGH)) goto done; }
    }
    LeaveCriticalSection(&server->database_lock); LeaveCriticalSection(&server->objects_lock); locked=0;
    phase=5; if(!drop_hash_file(snapshot,hash,&size) || !database_open(&db,snapshot,server->keys.database_key,0) || !integrity(db)) goto done;
    if(sqlite3_prepare_v2(db,"SELECT header FROM objects WHERE state='complete' ORDER BY object_id",-1,&s,NULL)!=SQLITE_OK) goto done;
    phase=6; while(sqlite3_step(s)==SQLITE_ROW) {
        drop_object object={0};
        if(!drop_object_open(server->keys.object_master_key,sqlite3_column_blob(s,0),(size_t)sqlite3_column_bytes(s,0),&object)) goto done;
        if(!paths(server->directory,object.id,L"manifest.bin",from,0) || !paths(to,object.id,L"manifest.bin",root,1) || !copy_verified(from,root)) { sodium_memzero(&object,sizeof(object)); goto done; }
        for(i=0;i<object.chunks;i++) {
            if(WaitForSingleObject(server->stop_event,0)==WAIT_OBJECT_0) { sodium_memzero(&object,sizeof(object)); goto done; }
            swprintf_s(name,64,L"%016llx.chunk",(unsigned long long)i);
            if(!paths(server->directory,object.id,name,from,0) || !paths(to,object.id,name,root,0) || !copy_verified(from,root)) { sodium_memzero(&object,sizeof(object)); goto done; }
        }
        if(!object_verify(to,&server->keys,sqlite3_column_blob(s,0),sqlite3_column_bytes(s,0))) { sodium_memzero(&object,sizeof(object)); goto done; }
        sodium_memzero(&object,sizeof(object)); ++count;
    }
    phase=7; if(sqlite3_errcode(db)!=SQLITE_DONE && sqlite3_errcode(db)!=SQLITE_OK) goto done;
    sqlite3_finalize(s); s=NULL; sqlite3_close(db); db=NULL;
    memcpy(payload,"DROPBM01",8); put64(payload+8,DROP_SCHEMA_VERSION); memcpy(payload+16,server->keys.server_id,32); memcpy(payload+48,task,32); memcpy(payload+80,hash,32); put64(payload+112,count); put64(payload+120,(uint64_t)time(NULL));
    memcpy(manifest,"DROPBAK1",8); randombytes_buf(manifest+8,24);
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(manifest+32,&cipher_size,payload,sizeof(payload),manifest,8,NULL,manifest+8,server->keys.object_master_key) || cipher_size!=sizeof(payload)+16 ||
        !drop_path_join(to,L"complete.bin",root,32768) || !drop_file_write(root,manifest,sizeof(manifest),1)) goto done;
    EnterCriticalSection(&server->database_lock); locked=2;
    if(!sql(server->database,"BEGIN IMMEDIATE") || sqlite3_prepare_v2(server->database,"UPDATE backup_jobs SET state='complete',completed_at=?1,error='' WHERE task_id=?2",-1,&s,NULL)!=SQLITE_OK) goto done;
    sqlite3_bind_int64(s,1,(sqlite3_int64)time(NULL)); bind(s,2,task); if(sqlite3_step(s)!=SQLITE_DONE) goto done; sqlite3_finalize(s); s=NULL;
    if(sqlite3_prepare_v2(server->database,"DELETE FROM object_pins WHERE task_id=?1",-1,&s,NULL)!=SQLITE_OK) goto done;
    bind(s,1,task); if(sqlite3_step(s)!=SQLITE_DONE || !sql(server->database,"COMMIT")) goto done; ok=1;
done:
    if(!ok) fprintf(stderr,"backup_failed phase=%d source_database=%d snapshot_database=%d windows=%lu\n",phase,sqlite3_errcode(server->database),db?sqlite3_errcode(db):0,GetLastError());
    sqlite3_finalize(s); if(backup) sqlite3_backup_finish(backup); if(db) sqlite3_close(db);
    if(locked) { if(locked==2 && !ok) sql(server->database,"ROLLBACK"); LeaveCriticalSection(&server->database_lock); if(locked==1) LeaveCriticalSection(&server->objects_lock); }
    if(!ok && task && drop_id_valid(task)) {
        EnterCriticalSection(&server->database_lock);
        if(sqlite3_prepare_v2(server->database,"UPDATE backup_jobs SET state='retry',error='backup_failed' WHERE task_id=?1",-1,&s,NULL)==SQLITE_OK) { bind(s,1,task); sqlite3_step(s); } sqlite3_finalize(s);
        LeaveCriticalSection(&server->database_lock);
    }
    free(root); free(snapshot); free(from); free(to); free(utf8); free(building); return ok;
}
int drop_backup_restore(const WCHAR *source,const WCHAR *package,const WCHAR *target,const char *password) {
    drop_keys keys={0}; unsigned char *manifest=NULL,payload[MANIFEST_PLAIN],hash[32]; DWORD length=0; uint64_t size,count=0,i; unsigned long long plain_size;
    WCHAR *from=calloc(32768,sizeof(WCHAR)),*to=calloc(32768,sizeof(WCHAR)),*root=calloc(32768,sizeof(WCHAR)),name[64]; sqlite3 *db=NULL; sqlite3_stmt *s=NULL; int ok=0;
    if(!from || !to || !root || !recovery_load(package,password,&keys) || !drop_path_join(source,L"complete.bin",from,32768) || !drop_file_read(from,&manifest,&length,MANIFEST_SIZE) || length!=MANIFEST_SIZE || memcmp(manifest,"DROPBAK1",8) ||
        crypto_aead_xchacha20poly1305_ietf_decrypt(payload,&plain_size,NULL,manifest+32,length-32,manifest,8,manifest+8,keys.object_master_key) || plain_size!=sizeof(payload) || memcmp(payload,"DROPBM01",8) || get64(payload+8)<7 || get64(payload+8)>DROP_SCHEMA_VERSION || memcmp(payload+16,keys.server_id,32)) goto done;
    if(!drop_path_join(source,L"snapshot.db",from,32768) || !drop_hash_file(from,hash,&size) || sodium_memcmp(hash,payload+80,32) || !database_open(&db,from,keys.database_key,0) || !integrity(db)) goto done;
    if(sqlite3_prepare_v2(db,"PRAGMA user_version",-1,&s,NULL)!=SQLITE_OK || sqlite3_step(s)!=SQLITE_ROW || (uint64_t)sqlite3_column_int(s,0)!=get64(payload+8)) goto done;
    sqlite3_finalize(s); s=NULL;
    if(GetFileAttributesW(target)!=INVALID_FILE_ATTRIBUTES || !drop_private_directory(target) || !drop_path_join(target,L"restore.incomplete",to,32768) || !drop_file_write(to,"DROP",4,0)) goto done;
    if(!drop_path_join(target,L"server.db",to,32768) || !copy_verified(from,to)) goto done;
    if(sqlite3_prepare_v2(db,"SELECT header FROM objects WHERE state='complete' ORDER BY object_id",-1,&s,NULL)!=SQLITE_OK) goto done;
    while(sqlite3_step(s)==SQLITE_ROW) {
        drop_object object={0};
        if(!object_verify(source,&keys,sqlite3_column_blob(s,0),sqlite3_column_bytes(s,0)) || !drop_object_open(keys.object_master_key,sqlite3_column_blob(s,0),(size_t)sqlite3_column_bytes(s,0),&object)) goto done;
        if(!paths(source,object.id,L"manifest.bin",from,0) || !paths(target,object.id,L"manifest.bin",to,1) || !copy_verified(from,to)) { sodium_memzero(&object,sizeof(object)); goto done; }
        for(i=0;i<object.chunks;i++) { swprintf_s(name,64,L"%016llx.chunk",(unsigned long long)i); if(!paths(source,object.id,name,from,0) || !paths(target,object.id,name,to,0) || !copy_verified(from,to)) { sodium_memzero(&object,sizeof(object)); goto done; } }
        sodium_memzero(&object,sizeof(object)); ++count;
    }
    if(count!=get64(payload+112)) goto done;
    sqlite3_finalize(s); s=NULL; sqlite3_close(db); db=NULL;
    if(!drop_path_join(target,L"server.db",to,32768) || !database_open(&db,to,keys.database_key,2)) goto done;
    if(get64(payload+8)>=9 && !sql(db,"DELETE FROM cloud_files; DELETE FROM cloud_backups;")) goto done;
    drop_id_new(keys.generation);
    if(!sql(db,"BEGIN IMMEDIATE; DELETE FROM object_pins; DELETE FROM backup_jobs; DELETE FROM chunks WHERE object_id IN(SELECT object_id FROM objects WHERE state!='complete'); DELETE FROM objects WHERE state!='complete'; DELETE FROM saved_bodies; DELETE FROM saved_objects; DELETE FROM read_positions; DELETE FROM state_events; UPDATE state_sequences SET last_seq=0;") || sqlite3_prepare_v2(db,"UPDATE meta SET value=?1 WHERE key='generation'",-1,&s,NULL)!=SQLITE_OK) goto done;
    bind(s,1,keys.generation); if(sqlite3_step(s)!=SQLITE_DONE || !sql(db,"COMMIT") || !integrity(db)) goto done;
    sqlite3_finalize(s); s=NULL; sqlite3_close(db); db=NULL;
    if(!drop_path_join(target,L"runtime-user.dpapi",to,32768) || !drop_secret_save(to,&keys,sizeof(keys)) || !drop_path_join(target,L"restore.incomplete",to,32768) || !DeleteFileW(to)) goto done;
    ok=1;
done:
    sqlite3_finalize(s); if(db) sqlite3_close(db); free(manifest); free(from); free(to); free(root); sodium_memzero(&keys,sizeof(keys)); return ok;
}
#if DROP_DIAGNOSTICS
int drop_backup_test(const WCHAR *base) {
    WCHAR *root=malloc(32768*sizeof(WCHAR)),*target=malloc(32768*sizeof(WCHAR)),*package=malloc(32768*sizeof(WCHAR)),*snapshot=malloc(32768*sizeof(WCHAR)),*restored=malloc(32768*sizeof(WCHAR));
    char id[DROP_ID_CAP],task[DROP_ID_CAP]={0},user[DROP_ID_CAP],device[DROP_ID_CAP],recipient[DROP_ID_CAP],object_id[DROP_ID_CAP]; WCHAR wide[DROP_ID_CAP];
    drop_server server={0},copy={0}; cJSON *login=NULL,*input=NULL,*output=NULL; sqlite3_stmt *s=NULL; int opened=0,copy_open=0,ok=0,phase=1; unsigned char *body=malloc(DROP_CHUNK_SIZE+3),hash[32]; char hex[65];
    if(!root || !target || !package || !snapshot || !restored || !body) goto done;
    drop_id_new(id); drop_to_wide(id,wide,DROP_ID_CAP); if(swprintf_s(root,32768,L"%s\\backup-test-%s",base,wide)<0 || !drop_server_open(&server,root,1)) goto done; opened=1;
    phase=2; if(!drop_account_create(&server,"backup-user","恢复测试","backup-fixture-password-123")) goto done;
    drop_id_new(device); if(!drop_login(&server,"backup-user","backup-fixture-password-123",device,"恢复设备",&login)) goto done;
    strcpy_s(user,sizeof(user),drop_json_string(login,"user_id",32));
    phase=3; if(!drop_admin_account(&server,"backup-user",0,NULL) || drop_authorize(&server,drop_json_string(login,"token",64),user,device) || !drop_admin_account(&server,"backup-user",1,NULL)) goto done;
    cJSON_Delete(login); login=NULL; Sleep(1050);
    if(!drop_login(&server,"backup-user","backup-fixture-password-123",device,"恢复设备",&login) || !drop_admin_revoke(&server,"backup-user",device) || drop_authorize(&server,drop_json_string(login,"token",64),user,device)) goto done;
    phase=4; cJSON_Delete(login); login=NULL; randombytes_buf(body,DROP_CHUNK_SIZE+3); crypto_hash_sha256(hash,body,DROP_CHUNK_SIZE+3); drop_hash_hex(hash,hex); drop_id_new(id);
    input=cJSON_CreateObject(); cJSON_AddStringToObject(input,"request_id",id); cJSON_AddStringToObject(input,"filename","备份附件.bin"); cJSON_AddStringToObject(input,"total_size","1048579"); cJSON_AddStringToObject(input,"sha256",hex);
    if(drop_upload_create(&server,user,input,&output)!=DROP_OK) goto done;
    strcpy_s(object_id,sizeof(object_id),drop_json_string(output,"object_id",32)); cJSON_Delete(input); input=NULL; cJSON_Delete(output); output=NULL;
    phase=5; if(drop_upload_chunk(&server,user,object_id,0,body,DROP_CHUNK_SIZE)!=DROP_OK || drop_upload_chunk(&server,user,object_id,1,body+DROP_CHUNK_SIZE,3)!=DROP_OK || drop_upload_finish(&server,user,object_id,&output)!=DROP_OK) goto done;
    cJSON_Delete(output); output=NULL; input=cJSON_CreateObject(); drop_id_new(id); cJSON_AddStringToObject(input,"request_id",id); cJSON_AddStringToObject(input,"body","加密恢复正文 🎈"); cJSON_AddItemToArray(cJSON_AddArrayToObject(input,"attachments"),cJSON_CreateString(object_id));
    phase=6; if(drop_message_submit(&server,user,device,input,&output,recipient)!=DROP_OK) goto done;
    cJSON_Delete(input); input=NULL; cJSON_Delete(output); output=NULL;
    phase=7; if(!drop_path_join(root,L"portable-recovery.bin",package,32768) || !drop_recovery_export(&server,package,"independent-recovery-fixture-password-123") || !drop_path_join(root,L"local-backups",target,32768) || !drop_backup_create(&server,target,task)) goto done;
    drop_to_wide(task,wide,DROP_ID_CAP); if(!drop_path_join(target,wide,snapshot,32768)) goto done;
    phase=8; if(swprintf_s(restored,32768,L"%s-wrong",root)<0 || drop_backup_restore(snapshot,package,restored,"wrong-password") || GetFileAttributesW(restored)!=INVALID_FILE_ATTRIBUTES) goto done;
    phase=9; if(swprintf_s(restored,32768,L"%s-restored",root)<0 || !drop_backup_restore(snapshot,package,restored,"independent-recovery-fixture-password-123") || !drop_server_open(&copy,restored,0)) goto done; copy_open=1;
    if(strcmp(copy.keys.server_id,server.keys.server_id) || !strcmp(copy.keys.generation,server.keys.generation) || sqlite3_prepare_v2(copy.database,"SELECT count(*) FROM messages",-1,&s,NULL)!=SQLITE_OK || sqlite3_step(s)!=SQLITE_ROW || sqlite3_column_int(s,0)!=1) goto done;
    sqlite3_finalize(s); s=NULL;
    { unsigned char *received=NULL; DWORD length; if(drop_attachment_chunk(&copy,user,object_id,0,&received,&length)!=DROP_OK || length!=DROP_CHUNK_SIZE || memcmp(body,received,length)) { free(received); goto done; } free(received); }
    ok=1; puts("PASS backup: portable Argon2id recovery, wrong password refusal, keyed online snapshot, authenticated full attachment restore, new generation, account disable/device revoke");
done:
    if(!ok) fprintf(stderr,"Backup self-test failed at phase %d (database %d, Windows %lu)\n",phase,opened?sqlite3_errcode(server.database):0,GetLastError());
    sqlite3_finalize(s); cJSON_Delete(input); cJSON_Delete(output); cJSON_Delete(login); if(copy_open) drop_server_close(&copy); if(opened) drop_server_close(&server);
    free(root); free(target); free(package); free(snapshot); free(restored); free(body); return ok;
}

#endif

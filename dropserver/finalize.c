#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct finish_job { drop_server *server; char user[DROP_ID_CAP],id[DROP_ID_CAP]; } finish_job;
int drop_finalize_schema(drop_server *server) {
    sqlite3_stmt *s=NULL; int version=-1;
    if(sqlite3_prepare_v2(server->database,"PRAGMA user_version",-1,&s,NULL)==SQLITE_OK && sqlite3_step(s)==SQLITE_ROW) version=sqlite3_column_int(s,0);
    sqlite3_finalize(s);
    if(version==7) {
        if(!drop_schema_snapshot(server,7) || sqlite3_exec(server->database,"BEGIN IMMEDIATE; ALTER TABLE objects ADD COLUMN finalize_error TEXT NOT NULL DEFAULT ''; PRAGMA user_version=8; COMMIT;",NULL,NULL,NULL)!=SQLITE_OK) return 0;
    } else if(version<8 || version>DROP_SCHEMA_VERSION) return 0;
    return sqlite3_exec(server->database,"UPDATE objects SET state='uploading',finalize_error='interrupted' WHERE state='validating'",NULL,NULL,NULL)==SQLITE_OK;
}
static void failed(drop_server *server,const char *id,drop_error error) {
    sqlite3_stmt *s=NULL;
    EnterCriticalSection(&server->database_lock);
    if(sqlite3_prepare_v2(server->database,"UPDATE objects SET state='uploading',finalize_error=?1,last_activity=?2 WHERE object_id=?3 AND state='validating'",-1,&s,NULL)==SQLITE_OK) {
        sqlite3_bind_text(s,1,drop_error_name(error),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,(sqlite3_int64)time(NULL)); sqlite3_bind_text(s,3,id,-1,SQLITE_TRANSIENT); sqlite3_step(s);
    } sqlite3_finalize(s); LeaveCriticalSection(&server->database_lock);
}
static DWORD WINAPI worker(void *opaque) {
    finish_job *job=opaque; cJSON *out=NULL; drop_error error;
#if DROP_DIAGNOSTICS
    {
        WCHAR wide[24]; char text[24]; uint64_t pause;
        if(GetEnvironmentVariableW(L"DROP_TEST_FINALIZE_PAUSE_MS",wide,24) && drop_to_utf8(wide,text,sizeof(text)) && drop_u64_parse(text,&pause) && pause<=10000)
            WaitForSingleObject(job->server->stop_event,(DWORD)pause);
    }
#endif
    error=drop_upload_finish_verify(job->server,job->user,job->id,&out,1);
    if(error!=DROP_OK) failed(job->server,job->id,error);
    cJSON_Delete(out); free(job); return 0;
}
void drop_finalize_stop(drop_server *server) {
    int i; if(server->stop_event) SetEvent(server->stop_event);
    for(i=0;i<2;i++) if(server->finalize_threads[i]) { WaitForSingleObject(server->finalize_threads[i],INFINITE); CloseHandle(server->finalize_threads[i]); server->finalize_threads[i]=NULL; }
}
drop_error drop_upload_finish(drop_server *server,const char *user,const char *id,cJSON **output) {
    cJSON *status=NULL; uint64_t size; const char *state; sqlite3_stmt *s=NULL; drop_error error; finish_job *job=NULL; int slot=-1,i;
    error=drop_upload_status(server,user,id,&status); if(error!=DROP_OK) return error;
    state=drop_json_string(status,"state",16);
    if(!state || !drop_u64_parse(drop_json_string(status,"total_size",20),&size)) { cJSON_Delete(status); return DROP_E_DATABASE; }
    if(!strcmp(state,"complete") || !strcmp(state,"validating")) { *output=status; return DROP_OK; }
    if(size<=32*UINT64_C(1024)*1024) { cJSON_Delete(status); return drop_upload_finish_verify(server,user,id,output,0); }
    EnterCriticalSection(&server->finalize_lock);
    for(i=0;i<2;i++) {
        if(server->finalize_threads[i] && WaitForSingleObject(server->finalize_threads[i],0)==WAIT_OBJECT_0) { CloseHandle(server->finalize_threads[i]); server->finalize_threads[i]=NULL; }
        if(!server->finalize_threads[i] && slot<0) slot=i;
    }
    if(slot<0) { error=DROP_OK; goto done; }
    job=malloc(sizeof(*job)); if(!job) { error=DROP_E_IO; goto done; }
    job->server=server; strcpy_s(job->user,sizeof(job->user),user); strcpy_s(job->id,sizeof(job->id),id);
    EnterCriticalSection(&server->objects_lock); EnterCriticalSection(&server->database_lock);
    if(sqlite3_prepare_v2(server->database,"UPDATE objects SET state='validating',finalize_error='',last_activity=?1 WHERE object_id=?2 AND owner=?3 AND state='uploading' AND (SELECT count(*) FROM chunks WHERE object_id=?2)=((total+1048575)/1048576)",-1,&s,NULL)!=SQLITE_OK) error=DROP_E_DATABASE;
    else {
        sqlite3_bind_int64(s,1,(sqlite3_int64)time(NULL)); sqlite3_bind_text(s,2,id,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,3,user,-1,SQLITE_TRANSIENT);
        if(sqlite3_step(s)!=SQLITE_DONE) error=DROP_E_DATABASE;
        else if(sqlite3_changes(server->database)!=1) error=DROP_E_CONFLICT;
    }
    sqlite3_finalize(s); s=NULL; LeaveCriticalSection(&server->database_lock); LeaveCriticalSection(&server->objects_lock);
    if(error==DROP_OK) {
        server->finalize_threads[slot]=CreateThread(NULL,0,worker,job,0,NULL);
        if(!server->finalize_threads[slot]) { error=DROP_E_IO; failed(server,id,error); }
        else { job=NULL; cJSON_Delete(status); status=NULL; error=drop_upload_status(server,user,id,&status); }
    }
done:
    LeaveCriticalSection(&server->finalize_lock); free(job);
    if(error==DROP_OK) *output=status; else cJSON_Delete(status); return error;
}

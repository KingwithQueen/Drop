#include "cloud.h"
#include <winsock2.h>
#include <civetweb.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *encode(const char *text) {
    size_t n=strlen(text),i,j=0; char *out=malloc(n*3+1); static const char hex[]="0123456789ABCDEF";
    if(!out) return NULL;
    for(i=0;i<n;i++) { unsigned char c=(unsigned char)text[i]; if((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c=='~') out[j++]=(char)c;
        else { out[j++]='%'; out[j++]=hex[c>>4]; out[j++]=hex[c&15]; } } out[j]=0; return out;
}
char *drop_cloud_encode(const char *text) { return encode(text); }
static int save(drop_cloud *cloud) {
    char *text=cJSON_PrintUnformatted(cloud->credentials); int ok=0;
    if(text && strlen(text)<=65536) ok=cloud->machine?drop_machine_secret_replace(cloud->secret,text,(DWORD)strlen(text)):drop_secret_replace(cloud->secret,text,(DWORD)strlen(text));
    if(text) { sodium_memzero(text,strlen(text)); cJSON_free(text); } return ok;
}
static int token_path(drop_cloud *cloud) {
    WCHAR path[32768]; cloud->machine=drop_path_join(cloud->server->directory,L"runtime-machine.dpapi",path,32768) && GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;
    return drop_path_join(cloud->server->directory,cloud->machine?L"drive-machine.dpapi":L"drive-user.dpapi",cloud->secret,32768);
}
static void secret_json_free(cJSON *node) {
    cJSON *child; if(!node) return;
    if(node->valuestring) sodium_memzero(node->valuestring,strlen(node->valuestring));
    for(child=node->child;child;child=child->next) secret_json_free(child);
}
void drop_cloud_close(drop_cloud *cloud) { secret_json_free(cloud->credentials); cJSON_Delete(cloud->credentials); sodium_memzero(cloud,sizeof(*cloud)); }
static drop_error exchange(drop_cloud *cloud,const char *form) {
    WCHAR url[4097]; cloud_reply reply={0}; cJSON *json=NULL; const char *access,*refresh; drop_error error;
    if(swprintf_s(url,4097,L"%s/token",cloud->token_origin)<0) return DROP_E_ARGUMENT;
    error=drop_cloud_http(cloud,L"POST",url,L"Content-Type: application/x-www-form-urlencoded\r\n",form,(DWORD)strlen(form),0,&reply);
    if(error!=DROP_OK) goto done;
    json=drop_json_parse((char *)reply.data,reply.size);
    if(reply.status!=200) { error=reply.status==400 || reply.status==401?DROP_E_AUTH:drop_cloud_status(&reply); goto done; }
    access=drop_json_string(json,"access_token",4096); refresh=drop_json_string(json,"refresh_token",4096);
    if(!access || !*access || strpbrk(access,"\r\n")) { error=DROP_E_PROTOCOL; goto done; }
    strcpy_s(cloud->access,sizeof(cloud->access),access);
    if(refresh && *refresh) {
        cJSON *old=cJSON_GetObjectItemCaseSensitive(cloud->credentials,"refresh_token"); if(old && old->valuestring) sodium_memzero(old->valuestring,strlen(old->valuestring));
        cJSON_DeleteItemFromObjectCaseSensitive(cloud->credentials,"refresh_token");
        if(!cJSON_AddStringToObject(cloud->credentials,"refresh_token",refresh) || !save(cloud)) error=DROP_E_IO;
    }
done:
    secret_json_free(json); cJSON_Delete(json); drop_cloud_reply_free(&reply); return error;
}
drop_error drop_cloud_refresh(drop_cloud *cloud) {
    const char *id=drop_json_string(cloud->credentials,"client_id",4096),*secret=drop_json_string(cloud->credentials,"client_secret",4096),*refresh=drop_json_string(cloud->credentials,"refresh_token",4096);
    char *a=NULL,*b=NULL,*c=NULL,*form=NULL; drop_error error=DROP_E_AUTH;
    if(!id || !refresh || !*refresh) goto done;
    a=encode(id); b=encode(secret?secret:""); c=encode(refresh); form=malloc(40000);
    if(!a || !b || !c || !form) { error=DROP_E_IO; goto done; }
    sprintf_s(form,40000,"grant_type=refresh_token&client_id=%s&client_secret=%s&refresh_token=%s",a,b,c); error=exchange(cloud,form);
done:
    if(b) sodium_memzero(b,strlen(b)); if(c) sodium_memzero(c,strlen(c)); if(form) sodium_memzero(form,strlen(form)); free(a); free(b); free(c); free(form); return error;
}
drop_error drop_cloud_open(drop_cloud *cloud,drop_server *server) {
    unsigned char *bytes=NULL; DWORD size=0; const char *origin,*ca; drop_error error=DROP_E_AUTH;
    memset(cloud,0,sizeof(*cloud)); cloud->server=server;
    wcscpy_s(cloud->origin,2049,L"https://www.googleapis.com"); wcscpy_s(cloud->token_origin,2049,L"https://oauth2.googleapis.com");
    if(!token_path(cloud) || !drop_secret_load(cloud->secret,&bytes,&size) || !(cloud->credentials=drop_json_parse((char *)bytes,size))) goto done;
#if DROP_DIAGNOSTICS
    origin=drop_json_string(cloud->credentials,"test_origin",2048); ca=drop_json_string(cloud->credentials,"test_ca",131071);
    if(origin) { drop_url address; if(!drop_to_wide(origin,cloud->origin,2049) || !drop_url_parse(cloud->origin,&address) || wcscmp(address.host,L"localhost") || !ca || !drop_to_wide(ca,cloud->ca,32768)) { error=DROP_E_ARGUMENT; goto done; } wcscpy_s(cloud->token_origin,2049,cloud->origin); }
#else
    (void)origin; (void)ca;
#endif
    error=drop_cloud_refresh(cloud);
done:
    if(bytes) { sodium_memzero(bytes,size); free(bytes); } return error;
}
typedef struct auth_wait { HANDLE event; char state[65],code[4097]; } auth_wait;
static int callback(struct mg_connection *connection,void *opaque) {
    auth_wait *wait=opaque; const struct mg_request_info *request=mg_get_request_info(connection); char state[65],code[4097]; const char *query=request->query_string;
    if(strcmp(request->request_method,"GET") || !query || strlen(query)>8192 || mg_get_var(query,strlen(query),"state",state,sizeof(state))!=64 ||
        sodium_memcmp(state,wait->state,64) || mg_get_var(query,strlen(query),"code",code,sizeof(code))<=0) { mg_send_http_error(connection,400,"Invalid authorization response."); return 1; }
    strcpy_s(wait->code,sizeof(wait->code),code); sodium_memzero(code,sizeof(code));
    mg_printf(connection,"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 43\r\nConnection: close\r\n\r\nDrop authorization received. Close this tab."); SetEvent(wait->event); return 1;
}
int drop_cloud_authorize(drop_server *server,const WCHAR *file) {
    drop_cloud *cloud=calloc(1,sizeof(*cloud)); unsigned char *bytes=NULL; DWORD size=0; cJSON *json=NULL,*installed; const char *id,*secret;
    auth_wait *wait=calloc(1,sizeof(*wait)); struct mg_context *context=NULL; struct mg_callbacks callbacks={0}; SOCKET listener=INVALID_SOCKET; WSADATA wsa; int started=0,ok=0,n;
    struct sockaddr_in address={0}; char listen[48],redirect[96],verifier[65],challenge[65],random_state[65],*client=NULL,*redirect_encoded=NULL,*code=NULL,*secret_encoded=NULL,*form=NULL,*browser=NULL;
    unsigned char random[32],digest[32]; WCHAR *wide=NULL;
    if(!cloud || !wait || !drop_file_read(file,&bytes,&size,65536) || !(json=drop_json_parse((char *)bytes,size)) || !(installed=cJSON_GetObjectItemCaseSensitive(json,"installed"))) goto done;
    id=drop_json_string(installed,"client_id",4096); secret=drop_json_string(installed,"client_secret",4096); if(!id || !*id) goto done;
    cloud->server=server; cloud->credentials=cJSON_CreateObject(); wcscpy_s(cloud->origin,2049,L"https://www.googleapis.com"); wcscpy_s(cloud->token_origin,2049,L"https://oauth2.googleapis.com");
    if(!cloud->credentials || !token_path(cloud) || !cJSON_AddStringToObject(cloud->credentials,"client_id",id) || !cJSON_AddStringToObject(cloud->credentials,"client_secret",secret?secret:"")) goto done;
    client=encode(id); secret_encoded=encode(secret?secret:""); if(!client || !secret_encoded || WSAStartup(MAKEWORD(2,2),&wsa)) goto done; started=1;
    listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); if(listener==INVALID_SOCKET) goto done;
    address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); n=sizeof(address);
    if(bind(listener,(struct sockaddr *)&address,sizeof(address)) || getsockname(listener,(struct sockaddr *)&address,&n)) goto done;
    closesocket(listener); listener=INVALID_SOCKET;
    sprintf_s(listen,sizeof(listen),"127.0.0.1:%u",ntohs(address.sin_port)); sprintf_s(redirect,sizeof(redirect),"http://%s/callback",listen);
    randombytes_buf(random,32); sodium_bin2base64(verifier,sizeof(verifier),random,32,sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    crypto_hash_sha256(digest,(unsigned char *)verifier,strlen(verifier)); sodium_bin2base64(challenge,sizeof(challenge),digest,32,sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    drop_token_new(random_state); strcpy_s(wait->state,sizeof(wait->state),random_state); wait->event=CreateEventW(NULL,TRUE,FALSE,NULL); if(!wait->event) goto done;
    { const char *options[]={"listening_ports",listen,"num_threads","2","enable_keep_alive","no",NULL}; mg_init_library(0); context=mg_start(&callbacks,NULL,options); }
    if(!context) goto done; mg_set_request_handler(context,"/callback",callback,wait);
    redirect_encoded=encode(redirect); browser=malloc(20000); wide=malloc(20000*sizeof(WCHAR)); if(!redirect_encoded || !browser || !wide) goto done;
    sprintf_s(browser,20000,"https://accounts.google.com/o/oauth2/v2/auth?client_id=%s&redirect_uri=%s&response_type=code&scope=https%%3A%%2F%%2Fwww.googleapis.com%%2Fauth%%2Fdrive.appdata&access_type=offline&prompt=consent&state=%s&code_challenge=%s&code_challenge_method=S256",client,redirect_encoded,wait->state,challenge);
    if(!drop_to_wide(browser,wide,20000) || (INT_PTR)ShellExecuteW(NULL,L"open",wide,NULL,NULL,SW_SHOWNORMAL)<=32) goto done;
    { HANDLE events[2]={server->stop_event,wait->event}; if(WaitForMultipleObjects(2,events,FALSE,600000)!=WAIT_OBJECT_0+1) goto done; }
    code=encode(wait->code); form=malloc(60000); if(!code || !form) goto done;
    sprintf_s(form,60000,"grant_type=authorization_code&client_id=%s&client_secret=%s&code=%s&redirect_uri=%s&code_verifier=%s",client,secret_encoded,code,redirect_encoded,verifier);
    ok=exchange(cloud,form)==DROP_OK && drop_json_string(cloud->credentials,"refresh_token",4096)!=NULL;
done:
    if(context) mg_stop(context); mg_exit_library(); if(listener!=INVALID_SOCKET) closesocket(listener); if(started) WSACleanup();
    if(wait) { if(wait->event) CloseHandle(wait->event); sodium_memzero(wait,sizeof(*wait)); free(wait); }
    if(bytes) { sodium_memzero(bytes,size); free(bytes); } secret_json_free(json); cJSON_Delete(json);
    if(secret_encoded) sodium_memzero(secret_encoded,strlen(secret_encoded)); if(code) sodium_memzero(code,strlen(code)); if(form) sodium_memzero(form,strlen(form));
    sodium_memzero(verifier,sizeof(verifier)); free(client); free(secret_encoded); free(code); free(form); free(browser); free(wide); free(redirect_encoded); if(cloud) { drop_cloud_close(cloud); free(cloud); } return ok;
}
#if DROP_DIAGNOSTICS
int drop_cloud_test_authorize(drop_server *server,const WCHAR *origin,const WCHAR *ca) {
    drop_cloud *cloud=calloc(1,sizeof(*cloud)); char *text=malloc(131072),*url=malloc(8193); drop_url address; int ok=0;
    if(!cloud || !text || !url || !drop_url_parse(origin,&address) || wcscmp(address.host,L"localhost") || !drop_to_utf8(origin,url,8193) || !drop_to_utf8(ca,text,131072)) goto done;
    cloud->server=server; cloud->credentials=cJSON_CreateObject();
    if(!cloud->credentials || !token_path(cloud) || !cJSON_AddStringToObject(cloud->credentials,"client_id","isolated-drive-fixture") || !cJSON_AddStringToObject(cloud->credentials,"refresh_token","isolated-drive-refresh-fixture") || !cJSON_AddStringToObject(cloud->credentials,"test_origin",url) || !cJSON_AddStringToObject(cloud->credentials,"test_ca",text)) goto done;
    ok=save(cloud);
done:
    if(cloud) { drop_cloud_close(cloud); free(cloud); } free(text); free(url); return ok;
}
#endif

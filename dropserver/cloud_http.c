#include "cloud.h"
#include "../drop/trust.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int allowed(drop_cloud *cloud,const WCHAR *url,URL_COMPONENTS *parts,WCHAR host[256]) {
    size_t n; ZeroMemory(parts,sizeof(*parts)); parts->dwStructSize=sizeof(*parts);
    parts->dwHostNameLength=parts->dwUrlPathLength=parts->dwExtraInfoLength=parts->dwUserNameLength=parts->dwPasswordLength=(DWORD)-1;
    if(wcslen(url)>4096 || wcspbrk(url,L"\r\n\t ") || !WinHttpCrackUrl(url,0,0,parts) || parts->nScheme!=INTERNET_SCHEME_HTTPS ||
        !parts->dwHostNameLength || parts->dwHostNameLength>=256 || parts->dwUserNameLength || parts->dwPasswordLength) return 0;
    memcpy(host,parts->lpszHostName,parts->dwHostNameLength*sizeof(WCHAR)); host[parts->dwHostNameLength]=0;
    if(parts->nPort==443 && (!wcscmp(host,L"www.googleapis.com") || !wcscmp(host,L"oauth2.googleapis.com"))) return 1;
#if DROP_DIAGNOSTICS
    n=wcslen(cloud->origin); if(!wcsncmp(url,cloud->origin,n) && url[n]==L'/' && !wcscmp(host,L"localhost") && *cloud->ca) return 1;
#else
    (void)cloud; (void)n;
#endif
    return 0;
}
void drop_cloud_reply_free(cloud_reply *r) { if(r->data) { sodium_memzero(r->data,r->size); free(r->data); } memset(r,0,sizeof(*r)); }
drop_error drop_cloud_http(drop_cloud *cloud,const WCHAR *method,const WCHAR *url,const WCHAR *headers,const void *body,DWORD length,int authenticated,cloud_reply *r) {
    URL_COMPONENTS parts; WCHAR host[256],path[4097],authorization[4352]; HINTERNET session=NULL,connection=NULL,request=NULL;
    drop_trust trust={0}; DWORD option,size,received; size_t used=0; unsigned char *buffer=NULL; drop_error error=DROP_E_NETWORK; int async=0;
    memset(r,0,sizeof(*r)); if(length>DROP_MAX_JSON || !allowed(cloud,url,&parts,host) || (length && !body)) return DROP_E_ARGUMENT;
    if(WaitForSingleObject(cloud->server->stop_event,0)==WAIT_OBJECT_0) return DROP_E_NETWORK;
    if(parts.dwUrlPathLength+parts.dwExtraInfoLength>=4097) return DROP_E_ARGUMENT;
    memcpy(path,parts.lpszUrlPath,parts.dwUrlPathLength*sizeof(WCHAR)); used=parts.dwUrlPathLength;
    if(parts.dwExtraInfoLength) { memcpy(path+used,parts.lpszExtraInfo,parts.dwExtraInfoLength*sizeof(WCHAR)); used+=parts.dwExtraInfoLength; } path[used]=0; used=0;
    session=WinHttpOpen(L"DropDrive/0.1.0",!wcscmp(host,L"localhost")?WINHTTP_ACCESS_TYPE_NO_PROXY:WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,NULL,NULL,WINHTTP_FLAG_ASYNC);
    if(!session) goto done; WinHttpSetTimeouts(session,10000,10000,30000,30000);
    connection=WinHttpConnect(session,host,parts.nPort,0); if(!connection) goto done;
    request=WinHttpOpenRequest(connection,method,path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE); if(!request) goto done;
    option=WINHTTP_OPTION_REDIRECT_POLICY_NEVER; if(!WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&option,sizeof(option))) goto done;
    option=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH; if(!WinHttpSetOption(request,WINHTTP_OPTION_AUTOLOGON_POLICY,&option,sizeof(option))) goto done;
    if(!drop_trust_prepare(&trust,request,host,*cloud->ca?cloud->ca:NULL)) { error=DROP_E_CERTIFICATE; goto done; }
    if(!drop_trust_async(&trust,request)) goto done; async=1;
    if(authenticated) {
        WCHAR token[4097]; if(!*cloud->access || strpbrk(cloud->access,"\r\n") || !drop_to_wide(cloud->access,token,4097) || swprintf_s(authorization,4352,L"Authorization: Bearer %s\r\n",token)<0) { error=DROP_E_ARGUMENT; goto done; }
        sodium_memzero(token,sizeof(token)); if(!WinHttpAddRequestHeaders(request,authorization,(DWORD)-1,WINHTTP_ADDREQ_FLAG_ADD)) goto done;
    }
    drop_trust_begin(&trust);
    if(!WinHttpSendRequest(request,headers?headers:WINHTTP_NO_ADDITIONAL_HEADERS,headers?(DWORD)-1:0,(void *)body,length,length,(DWORD_PTR)&trust) ||
        !drop_trust_wait(&trust,request,WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,cloud->server->stop_event)) { if(trust.rejected || GetLastError()==ERROR_WINHTTP_SECURE_FAILURE) error=DROP_E_CERTIFICATE; goto done; }
    drop_trust_begin(&trust);
    if(!WinHttpReceiveResponse(request,NULL) || !drop_trust_wait(&trust,request,WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,cloud->server->stop_event)) goto done;
    if(trust.engine && (!trust.checked || trust.rejected)) { error=DROP_E_CERTIFICATE; goto done; }
    size=sizeof(r->status); if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,NULL,&r->status,&size,NULL)) goto done;
    size=sizeof(r->location); if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_LOCATION,NULL,r->location,&size,NULL)) r->location[0]=0;
    size=sizeof(r->range); if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_CUSTOM,L"Range",r->range,&size,NULL)) r->range[0]=0;
    buffer=malloc(DROP_MAX_JSON+1); if(!buffer) { error=DROP_E_IO; goto done; }
    for(;;) {
        drop_trust_begin(&trust); if(!WinHttpQueryDataAvailable(request,NULL) || !drop_trust_wait(&trust,request,WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE,cloud->server->stop_event)) goto done;
        size=trust.available; if(!size) break; if(size>DROP_MAX_JSON-used) { error=DROP_E_LIMIT; goto done; }
        drop_trust_begin(&trust); if(!WinHttpReadData(request,buffer+used,size,NULL) || !drop_trust_wait(&trust,request,WINHTTP_CALLBACK_STATUS_READ_COMPLETE,cloud->server->stop_event)) goto done;
        received=trust.available; if(!received) goto done; used+=received;
    }
    buffer[used]=0; r->data=buffer; r->size=(DWORD)used; buffer=NULL; error=DROP_OK;
done:
    if(async) drop_trust_close(&trust,request); else if(request) WinHttpCloseHandle(request);
    if(connection) WinHttpCloseHandle(connection); if(session) WinHttpCloseHandle(session); drop_trust_free(&trust);
    if(buffer) { sodium_memzero(buffer,used); free(buffer); } sodium_memzero(authorization,sizeof(authorization)); return error;
}
static drop_error response_error(const cloud_reply *r) {
    cJSON *json,*reason,*entry; drop_error error;
    if(r->status>=200 && r->status<300) return DROP_OK;
    if(r->status==401) return DROP_E_AUTH;
    if(r->status==429 || r->status>=500) return DROP_E_NETWORK;
    error=r->status==404?DROP_E_CONFLICT:r->status==403?DROP_E_AUTH:DROP_E_PROTOCOL;
    json=drop_json_parse((char *)r->data,r->size); reason=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json,"error"),"errors");
    cJSON_ArrayForEach(entry,reason) {
        const char *name=drop_json_string(entry,"reason",128);
        if(name && (!strcmp(name,"storageQuotaExceeded") || !strcmp(name,"quotaExceeded"))) error=DROP_E_SPACE;
        else if(name && (!strcmp(name,"rateLimitExceeded") || !strcmp(name,"userRateLimitExceeded"))) error=DROP_E_NETWORK;
        else if(r->status==403) error=DROP_E_AUTH;
    } cJSON_Delete(json); return error;
}
drop_error drop_cloud_json(drop_cloud *cloud,const WCHAR *method,const WCHAR *path,cJSON *input,cJSON **out) {
    WCHAR url[4097]; char *body=input?cJSON_PrintUnformatted(input):NULL; cloud_reply reply={0}; drop_error error; int attempt;
    if(out) *out=NULL;
    if((input && !body) || swprintf_s(url,4097,L"%s%s",cloud->origin,path)<0 || (body && strlen(body)>DROP_MAX_JSON)) { cJSON_free(body); return DROP_E_ARGUMENT; }
    for(attempt=0;attempt<2;attempt++) {
        error=drop_cloud_http(cloud,method,url,L"Content-Type: application/json\r\n",body,body?(DWORD)strlen(body):0,1,&reply);
        if(error==DROP_OK) error=response_error(&reply);
        if(error!=DROP_E_AUTH || attempt || drop_cloud_refresh(cloud)!=DROP_OK) break;
        drop_cloud_reply_free(&reply);
    }
    if(error==DROP_OK && out) { *out=drop_json_parse((char *)reply.data,reply.size); if(!*out) error=DROP_E_PROTOCOL; }
    drop_cloud_reply_free(&reply); if(body) { sodium_memzero(body,strlen(body)); cJSON_free(body); } return error;
}
drop_error drop_cloud_status(const cloud_reply *reply) { return response_error(reply); }

#include "network.h"
#include "trust.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE network_stop;
void drop_network_cancel_event(HANDLE event) { network_stop=event; }
int drop_network_wait(DWORD milliseconds) {
    if(network_stop) return WaitForSingleObject(network_stop,milliseconds)==WAIT_TIMEOUT;
    Sleep(milliseconds); return 1;
}
static HINTERNET open_session(const WCHAR *host,int asynchronous) {
    DWORD mode=(!_wcsicmp(host,L"localhost") || !wcscmp(host,L"127.0.0.1") || !wcscmp(host,L"::1") || !wcscmp(host,L"[::1]"))?
        WINHTTP_ACCESS_TYPE_NO_PROXY:WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    return WinHttpOpen(L"Drop/0.1.0",mode,NULL,NULL,asynchronous?WINHTTP_FLAG_ASYNC:0);
}

drop_error drop_http_json(const WCHAR *url, const WCHAR *method, const WCHAR *path,
    const char *token, const cJSON *body, cJSON **response, DWORD *status) {
    return drop_http_json_ca(url, method, path, token, body, response, status, NULL);
}
drop_error drop_http_json_ca(const WCHAR *url, const WCHAR *method, const WCHAR *path,
    const char *token, const cJSON *body, cJSON **response, DWORD *status, const WCHAR *ca_file) {
    char *serialized=body?cJSON_PrintUnformatted(body):NULL; unsigned char *bytes=NULL; DWORD length=0; drop_error error;
    if(!response || !status || (body && !serialized)) { cJSON_free(serialized); return DROP_E_ARGUMENT; }
    *response=NULL;
    if(serialized && strlen(serialized)>DROP_MAX_JSON) { sodium_memzero(serialized,strlen(serialized)); cJSON_free(serialized); return DROP_E_LIMIT; }
    error=drop_http_bytes(url,method,path,token,(unsigned char *)serialized,serialized?(DWORD)strlen(serialized):0,0,0,&bytes,&length,status,ca_file);
    if(error==DROP_OK) { *response=drop_json_parse((char *)bytes,length); if(!*response) error=DROP_E_PROTOCOL; }
    if(serialized) { sodium_memzero(serialized,strlen(serialized)); cJSON_free(serialized); }
    if(bytes) { sodium_memzero(bytes,length); free(bytes); } return error;
}
drop_error drop_http_bytes(const WCHAR *url,const WCHAR *method,const WCHAR *path,const char *token,
    const unsigned char *body,DWORD body_size,int binary_request,int binary_response,
    unsigned char **response,DWORD *response_size,DWORD *status,const WCHAR *ca_file) {
    drop_url address; HINTERNET session = NULL, connection = NULL, request = NULL;
    unsigned char *buffer = NULL; WCHAR headers[256]; DWORD available, received, status_size = sizeof(*status), redirects, autologon;
    size_t length = 0; drop_error result = DROP_E_NETWORK; DWORD last_error,phase=0;
    drop_trust trust = {0}; int asynchronous=0;
    if (!response || !response_size || !status || body_size>DROP_MAX_JSON || (body_size && !body) || !drop_url_parse(url, &address)) return DROP_E_ARGUMENT;
    *response = NULL; *response_size=0; *status = 0;
    if(network_stop && WaitForSingleObject(network_stop,0)==WAIT_OBJECT_0) return DROP_E_NETWORK;
    session = open_session(address.host,1);
    if (!session) goto cleanup;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);
    connection = WinHttpConnect(session, address.host, address.port, 0); if (!connection) goto cleanup;
    request = WinHttpOpenRequest(connection, method, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) goto cleanup;
    redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects))) goto cleanup;
    autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon))) goto cleanup;
    if (!drop_trust_prepare(&trust, request, address.host, ca_file)) { result = DROP_E_CERTIFICATE; goto cleanup; }
    if(!drop_trust_async(&trust,request)) goto cleanup; asynchronous=1;
    if (token) {
        WCHAR wide_token[DROP_TOKEN_CAP]; size_t i;
        if (strlen(token) != 64) { result = DROP_E_ARGUMENT; goto cleanup; }
        for (i = 0; i < 64; ++i) if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) { result = DROP_E_ARGUMENT; goto cleanup; }
        if (!drop_to_wide(token, wide_token, DROP_TOKEN_CAP) || swprintf_s(headers, 256, L"Authorization: Bearer %s\r\n", wide_token) < 0 ||
            !WinHttpAddRequestHeaders(request, headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD)) goto cleanup;
    }
    phase=1; drop_trust_begin(&trust);
    if (!WinHttpSendRequest(request, binary_request?L"Content-Type: application/octet-stream\r\n":L"Content-Type: application/json\r\n", (DWORD)-1, (void *)body,
        body_size, body_size,
        (DWORD_PTR)&trust) || !drop_trust_wait(&trust,request,WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,network_stop)) {
        last_error = GetLastError();
        if (last_error == ERROR_WINHTTP_SECURE_FAILURE || last_error == ERROR_WINHTTP_SECURE_CERT_CN_INVALID ||
            last_error == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID || last_error == ERROR_WINHTTP_SECURE_INVALID_CA || trust.rejected) result = DROP_E_CERTIFICATE;
        goto cleanup;
    }
    phase=2; drop_trust_begin(&trust);
    if(!WinHttpReceiveResponse(request,NULL) || !drop_trust_wait(&trust,request,WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,network_stop)) goto cleanup;
    if (trust.engine && (!trust.checked || trust.rejected)) { result = DROP_E_CERTIFICATE; goto cleanup; }
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, status, &status_size, NULL)) goto cleanup;
    {
        WCHAR type[256]; DWORD type_size=sizeof(type); const WCHAR *expected=(*status==200 && binary_response)?L"application/octet-stream":L"application/json";
        size_t expected_size=wcslen(expected);
        if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_CONTENT_TYPE,NULL,type,&type_size,NULL) ||
            _wcsnicmp(type,expected,expected_size) || (type[expected_size] && type[expected_size]!=L';')) { result=DROP_E_PROTOCOL; goto cleanup; }
    }
    buffer = malloc(DROP_MAX_JSON + 1); if (!buffer) { result = DROP_E_IO; goto cleanup; }
    for (;;) {
        phase=3; drop_trust_begin(&trust);
        if (!WinHttpQueryDataAvailable(request,NULL) || !drop_trust_wait(&trust,request,WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE,network_stop)) goto cleanup;
        available=trust.available;
        if (!available) break;
        if (available > DROP_MAX_JSON - length) { result = DROP_E_LIMIT; goto cleanup; }
        phase=4; drop_trust_begin(&trust);
        if (!WinHttpReadData(request, buffer + length, available, NULL) || !drop_trust_wait(&trust,request,WINHTTP_CALLBACK_STATUS_READ_COMPLETE,network_stop)) goto cleanup;
        received=trust.available; if(!received) goto cleanup;
        length += received;
    }
    buffer[length] = 0; *response=buffer; *response_size=(DWORD)length; buffer=NULL; result=DROP_OK;
cleanup:
    if(result==DROP_E_NETWORK) fprintf(stderr,"network_request_failed windows_error=%lu phase=%lu\n",GetLastError(),phase);
    if(asynchronous) drop_trust_close(&trust,request);
    else if (request && !trust.closed) { WinHttpSetStatusCallback(request, NULL, 0, 0); WinHttpCloseHandle(request); }
    if (buffer) { sodium_memzero(buffer, length); free(buffer); }
    sodium_memzero(headers, sizeof(headers));
    if (connection) WinHttpCloseHandle(connection); if (session) WinHttpCloseHandle(session); drop_trust_free(&trust);
    return result;
}
int drop_network_probe(const WCHAR *url) {
    cJSON *response = NULL; DWORD status; drop_error error;
    error = drop_http_json(url, L"GET", L"/api/v1/server-info", NULL, NULL, &response, &status);
    if (error != DROP_OK) { printf("Connection: %s\n", drop_error_name(error)); return 0; }
    if (status != 200 || !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(response, "protocol_version")) ||
        cJSON_GetObjectItemCaseSensitive(response, "protocol_version")->valuedouble != DROP_PROTOCOL_VERSION ||
        !drop_id_valid(drop_json_string(response, "server_id", 32)) ||
        !drop_id_valid(drop_json_string(response, "generation", 32))) {
        cJSON_Delete(response); puts("Connection: protocol_error"); return 0;
    }
    puts("Connection: HTTPS certificate verified; protocol v1; login required."); cJSON_Delete(response); return 1;
}
drop_error drop_ws_open(const WCHAR *url, const char *token, drop_ws *ws, const WCHAR *ca_file) {
    drop_url address; HINTERNET request = NULL; WCHAR wide[DROP_TOKEN_CAP], headers[128]; DWORD status = 0, size = sizeof(status), option;
    drop_error error = DROP_E_NETWORK; size_t i;
    drop_trust trust = {0}; int asynchronous=0;
    memset(ws, 0, sizeof(*ws));
    if (!drop_url_parse(url, &address) || !token || strlen(token) != 64) return DROP_E_ARGUMENT;
    for (i = 0; i < 64; ++i) if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) return DROP_E_ARGUMENT;
    if (!drop_to_wide(token, wide, DROP_TOKEN_CAP) || swprintf_s(headers, 128, L"Authorization: Bearer %s\r\n", wide) < 0) return DROP_E_ARGUMENT;
    ws->session = open_session(address.host,1);
    if (!ws->session || !WinHttpSetTimeouts(ws->session, 10000, 10000, 15000, 45000)) goto done;
    ws->connection = WinHttpConnect(ws->session, address.host, address.port, 0); if (!ws->connection) goto done;
    request = WinHttpOpenRequest(ws->connection, L"GET", L"/api/v1/events", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    option = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &option, sizeof(option)) ||
        !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) ||
        !WinHttpAddRequestHeaders(request, headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD)) goto done;
    option = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &option, sizeof(option))) goto done;
    if (!drop_trust_prepare(&trust, request, address.host, ca_file)) { error = DROP_E_CERTIFICATE; goto done; }
    if(!drop_trust_async(&trust,request)) goto done; asynchronous=1; drop_trust_begin(&trust);
    if (!WinHttpSendRequest(request, NULL, 0, NULL, 0, 0,(DWORD_PTR)&trust) || !drop_trust_wait(&trust,request,WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,network_stop)) {
        if (GetLastError() == ERROR_WINHTTP_SECURE_FAILURE || trust.rejected) error = DROP_E_CERTIFICATE;
        goto done;
    }
    drop_trust_begin(&trust);
    if(!WinHttpReceiveResponse(request,NULL) || !drop_trust_wait(&trust,request,WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,network_stop)) goto done;
    if (trust.engine && (!trust.checked || trust.rejected)) { error = DROP_E_CERTIFICATE; goto done; }
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL)) goto done;
    if (status != 101) { error = status == 401 ? DROP_E_AUTH : DROP_E_PROTOCOL; goto done; }
    ws->socket = WinHttpWebSocketCompleteUpgrade(request, 0); if (!ws->socket) goto done;
    WinHttpSetStatusCallback(ws->socket, NULL, 0, 0);
    if(!drop_ws_attach(ws)) goto done;
    error = DROP_OK;
done:
    if(asynchronous) drop_trust_close(&trust,request);
    else if (request && !trust.closed) { WinHttpSetStatusCallback(request, NULL, 0, 0); WinHttpCloseHandle(request); }
    drop_trust_free(&trust); sodium_memzero(wide, sizeof(wide)); sodium_memzero(headers, sizeof(headers));
    if (error != DROP_OK) drop_ws_close(ws); return error;
}

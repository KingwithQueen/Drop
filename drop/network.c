#include "network.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

drop_error drop_http_json(const WCHAR *url, const WCHAR *method, const WCHAR *path,
    const char *token, const cJSON *body, cJSON **response, DWORD *status) {
    drop_url address; HINTERNET session = NULL, connection = NULL, request = NULL;
    char *serialized = NULL, *buffer = NULL; WCHAR headers[256]; DWORD available, received, status_size = sizeof(*status), redirects;
    size_t length = 0; drop_error result = DROP_E_NETWORK; DWORD last_error;
    if (!response || !status || !drop_url_parse(url, &address)) return DROP_E_ARGUMENT;
    *response = NULL; *status = 0;
    session = WinHttpOpen(L"Drop/" L"0.1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!session) goto cleanup;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);
    connection = WinHttpConnect(session, address.host, address.port, 0); if (!connection) goto cleanup;
    request = WinHttpOpenRequest(connection, method, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) goto cleanup;
    redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects))) goto cleanup;
    if (token) {
        WCHAR wide_token[DROP_TOKEN_CAP]; size_t i;
        if (strlen(token) != 64) { result = DROP_E_ARGUMENT; goto cleanup; }
        for (i = 0; i < 64; ++i) if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) { result = DROP_E_ARGUMENT; goto cleanup; }
        if (!drop_to_wide(token, wide_token, DROP_TOKEN_CAP) || swprintf_s(headers, 256, L"Authorization: Bearer %s\r\n", wide_token) < 0 ||
            !WinHttpAddRequestHeaders(request, headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD)) goto cleanup;
    }
    if (body) {
        serialized = cJSON_PrintUnformatted(body); if (!serialized || strlen(serialized) > DROP_MAX_JSON) { result = DROP_E_LIMIT; goto cleanup; }
    }
    if (!WinHttpSendRequest(request, L"Content-Type: application/json\r\n", (DWORD)-1, serialized,
        serialized ? (DWORD)strlen(serialized) : 0, serialized ? (DWORD)strlen(serialized) : 0, 0) || !WinHttpReceiveResponse(request, NULL)) {
        last_error = GetLastError();
        if (last_error == ERROR_WINHTTP_SECURE_FAILURE || last_error == ERROR_WINHTTP_SECURE_CERT_CN_INVALID ||
            last_error == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID || last_error == ERROR_WINHTTP_SECURE_INVALID_CA) result = DROP_E_CERTIFICATE;
        goto cleanup;
    }
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, status, &status_size, NULL)) goto cleanup;
    buffer = malloc(DROP_MAX_JSON + 1); if (!buffer) { result = DROP_E_IO; goto cleanup; }
    for (;;) {
        if (!WinHttpQueryDataAvailable(request, &available)) goto cleanup;
        if (!available) break;
        if (available > DROP_MAX_JSON - length) { result = DROP_E_LIMIT; goto cleanup; }
        if (!WinHttpReadData(request, buffer + length, available, &received) || !received) goto cleanup;
        length += received;
    }
    buffer[length] = 0; *response = drop_json_parse(buffer, length);
    result = *response ? DROP_OK : DROP_E_PROTOCOL;
cleanup:
    if (serialized) { sodium_memzero(serialized, strlen(serialized)); cJSON_free(serialized); }
    if (buffer) { sodium_memzero(buffer, length); free(buffer); }
    sodium_memzero(headers, sizeof(headers));
    if (request) WinHttpCloseHandle(request); if (connection) WinHttpCloseHandle(connection); if (session) WinHttpCloseHandle(session);
    return result;
}
int drop_network_probe(const WCHAR *url) {
    cJSON *response = NULL; DWORD status; drop_error error;
    error = drop_http_json(url, L"GET", L"/api/v1/server-info", NULL, NULL, &response, &status);
    if (error != DROP_OK) { printf("Connection: %s\n", drop_error_name(error)); return 0; }
    if (status != 200 || !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(response, "protocol_version")) ||
        cJSON_GetObjectItemCaseSensitive(response, "protocol_version")->valueint != DROP_PROTOCOL_VERSION) {
        cJSON_Delete(response); puts("Connection: protocol_error"); return 0;
    }
    puts("Connection: HTTPS certificate verified; protocol v1; login required."); cJSON_Delete(response); return 1;
}

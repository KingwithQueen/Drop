#include "drop_common.h"
#include <winhttp.h>
#include <sodium.h>
#include <zstd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

const char *drop_error_name(drop_error error) {
    static const char *names[] = {"ok", "invalid_argument", "limit_exceeded", "io_error",
        "crypto_error", "database_error", "unauthorized", "conflict", "protocol_error",
        "network_error", "certificate_error", "unsupported"};
    return (unsigned)error < sizeof(names)/sizeof(names[0]) ? names[error] : "unknown_error";
}
int drop_init(void) { return sodium_init() >= 0; }
int drop_u64_parse(const char *text, uint64_t *value) {
    uint64_t result = 0;
    if (!text || !*text || !value || (text[0] == '0' && text[1])) return 0;
    for (; *text; ++text) {
        unsigned digit = (unsigned char)*text - (unsigned)'0';
        if (digit > 9 || result > (UINT64_MAX - digit) / 10) return 0;
        result = result * 10 + digit;
    }
    *value = result;
    return 1;
}
int drop_id_valid(const char *text) {
    size_t i;
    if (!text || strlen(text) != 32) return 0;
    for (i = 0; i < 32; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return 0;
    return 1;
}
void drop_id_new(char output[DROP_ID_CAP]) {
    unsigned char bytes[16]; randombytes_buf(bytes, sizeof(bytes));
    sodium_bin2hex(output, DROP_ID_CAP, bytes, sizeof(bytes));
}
void drop_token_new(char output[DROP_TOKEN_CAP]) {
    unsigned char bytes[32]; randombytes_buf(bytes, sizeof(bytes));
    sodium_bin2hex(output, DROP_TOKEN_CAP, bytes, sizeof(bytes)); sodium_memzero(bytes, sizeof(bytes));
}
int drop_utf8_valid(const char *text, size_t size) {
    if (!text || size > INT_MAX || memchr(text, 0, size)) return 0;
    if (!size) return 1;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)size, NULL, 0) > 0;
}
int drop_to_wide(const char *text, WCHAR *output, size_t capacity) {
    return text && output && capacity <= INT_MAX &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, output, (int)capacity) > 0;
}
int drop_to_utf8(const WCHAR *text, char *output, size_t capacity) {
    return text && output && capacity <= INT_MAX &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, output, (int)capacity, NULL, NULL) > 0;
}
int drop_url_parse(const WCHAR *text, drop_url *url) {
    URL_COMPONENTS parts; size_t length;
    if (!text || !url || (length = wcslen(text)) == 0 || length > 2048 ||
        wcspbrk(text, L"\r\n\t ") || wcsstr(text, L"@")) return 0;
    ZeroMemory(&parts, sizeof(parts)); parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength =
        parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(text, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
        !parts.nPort || !parts.dwHostNameLength || parts.dwHostNameLength >= 256 ||
        parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength ||
        (parts.dwUrlPathLength && !(parts.dwUrlPathLength == 1 && parts.lpszUrlPath[0] == L'/'))) return 0;
    memcpy(url->host, parts.lpszHostName, parts.dwHostNameLength * sizeof(WCHAR));
    url->host[parts.dwHostNameLength] = 0; url->port = parts.nPort;
    return 1;
}
static int json_valid(const cJSON *node, unsigned depth) {
    const cJSON *child, *other;
    if (depth > 16) return 0;
    if (cJSON_IsString(node) && !drop_utf8_valid(node->valuestring, strlen(node->valuestring))) return 0;
    for (child = node->child; child; child = child->next) {
        if (child->string) {
            if (!drop_utf8_valid(child->string, strlen(child->string))) return 0;
            for (other = child->next; other; other = other->next)
                if (other->string && !strcmp(child->string, other->string)) return 0;
        }
        if (!json_valid(child, depth + 1)) return 0;
    }
    return 1;
}
cJSON *drop_json_parse(const char *data, size_t size) {
    cJSON *result; const char *end; size_t i;
    if (!size || size > DROP_MAX_JSON || !drop_utf8_valid(data, size)) return NULL;
    /* cJSON uses zero-terminated strings. Refuse escaped NUL before parsing. */
    for (i = 0; i < size; ++i) {
        if (data[i] == '\\' && i + 1 < size) {
            if (i + 5 < size && !memcmp(data + i + 1, "u0000", 5)) return NULL;
            ++i;
        }
    }
    result = cJSON_ParseWithLengthOpts(data, size, &end, 0);
    if (!result) return NULL;
    while (end < data + size && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    if (end != data + size || !cJSON_IsObject(result) || !json_valid(result, 0)) {
        cJSON_Delete(result); return NULL;
    }
    return result;
}
const char *drop_json_string(const cJSON *object, const char *key, size_t maximum) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && strlen(value->valuestring) <= maximum ? value->valuestring : NULL;
}
int drop_text_pack(const char *text, int use_zstd, int level, drop_text *output) {
    size_t size, bound, compressed; unsigned char *data;
    if (!text || !output || (size = strlen(text)) > DROP_MAX_BODY || !drop_utf8_valid(text, size)) return 0;
    memset(output, 0, sizeof(*output));
    if (use_zstd && (level < 1 || level > 19)) return 0;
    bound = use_zstd ? ZSTD_compressBound(size) : size + 1;
    data = malloc(bound); if (!data) return 0;
    compressed = use_zstd ? ZSTD_compress(data, bound, text, size, level) : size;
    if (use_zstd && ZSTD_isError(compressed)) { free(data); return 0; }
    output->compressed = use_zstd && compressed < size;
    if (!output->compressed) { memcpy(data, text, size); compressed = size; }
    output->data = data; output->size = compressed; output->original_size = size;
    return 1;
}
int drop_text_unpack(const drop_text *input, char **output) {
    char *text; size_t size;
    if (!input || !output || input->original_size > DROP_MAX_BODY || input->size > DROP_MAX_BODY ||
        (!input->data && input->size) || (input->compressed != 0 && input->compressed != 1)) return 0;
    text = malloc(input->original_size + 1); if (!text) return 0;
    if (input->compressed) {
        if (ZSTD_getFrameContentSize(input->data, input->size) != input->original_size ||
            ZSTD_findFrameCompressedSize(input->data, input->size) != input->size) { free(text); return 0; }
        size = ZSTD_decompress(text, input->original_size, input->data, input->size);
        if (ZSTD_isError(size) || size != input->original_size) { free(text); return 0; }
    } else {
        if (input->size != input->original_size) { free(text); return 0; }
        memcpy(text, input->data, input->size); size = input->size;
    }
    if (!drop_utf8_valid(text, size)) { free(text); return 0; }
    text[size] = 0; *output = text; return 1;
}
void drop_text_free(drop_text *text) { if (text) { free(text->data); memset(text, 0, sizeof(*text)); } }

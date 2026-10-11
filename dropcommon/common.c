/* 跨客户端与服务器共用的严格输入校验、编码转换和正文压缩工具。 */
#include "drop_common.h"
#include <winhttp.h>
#include <sodium.h>
#include <zstd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* 验证 ASCII 账号长度和允许字符，保留大小写；客户端资料校验与服务器创建、登录使用相同规则。 */
int drop_account_login_valid(const char *login) {
    size_t i, length = login ? strlen(login) : 0;
    if (length < 3 || length > 64) return 0;
    for (i = 0; i < length; ++i)
        if (!((login[i] >= 'a' && login[i] <= 'z') || (login[i] >= 'A' && login[i] <= 'Z') || (login[i] >= '0' && login[i] <= '9') || login[i] == '_' || login[i] == '-' ||
                login[i] == '.'))
            return 0;
    return 1;
}

/* 返回协议使用的稳定错误名称；未知枚举值统一表示为 unknown_error，返回字符串无需释放。 */
const char *drop_error_name(drop_error error) {
    static const char *names[] = {"ok", "invalid_argument", "limit_exceeded", "io_error", "crypto_error", "database_error", "unauthorized", "conflict", "protocol_error",
        "network_error", "certificate_error", "unsupported", "space_unavailable", "in_progress"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown_error";
}
/* 将当前 Windows 文件错误转换为项目错误，单独识别磁盘空间不足。 */
drop_error drop_io_error(void) {
    DWORD error = GetLastError();
    return error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL || error == ERROR_DISK_QUOTA_EXCEEDED ? DROP_E_SPACE : DROP_E_IO;
}
/* 初始化 libsodium；进程使用随机数和加密接口前必须调用，失败返回 0。 */
int drop_init(void) {
    return sodium_init() >= 0;
}
/* 严格解析非空十进制无符号整数，拒绝非法字符及 64 位溢出；成功后才写入 value。 */
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
/* 检查固定长度的小写十六进制 ID，避免未经校验的标识进入协议和文件路径。 */
int drop_id_valid(const char *text) {
    size_t i;
    if (!text || strlen(text) != 32) return 0;
    for (i = 0; i < 32; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return 0;
    return 1;
}
/* 使用安全随机数生成 128 位 ID，并写成带终止符的小写十六进制字符串。 */
void drop_id_new(char output[DROP_ID_CAP]) {
    unsigned char bytes[16];
    randombytes_buf(bytes, sizeof(bytes));
    sodium_bin2hex(output, DROP_ID_CAP, bytes, sizeof(bytes));
}
/* 使用安全随机数生成 256 位凭据；调用者负责保护输出并在不再使用时清零。 */
void drop_token_new(char output[DROP_TOKEN_CAP]) {
    unsigned char bytes[32];
    randombytes_buf(bytes, sizeof(bytes));
    sodium_bin2hex(output, DROP_TOKEN_CAP, bytes, sizeof(bytes));
    sodium_memzero(bytes, sizeof(bytes));
}
/* 验证指定长度的 UTF-8 字节序列；同时拒绝内容中的 NUL，避免长度和字符串解释不一致。 */
int drop_utf8_valid(const char *text, size_t size) {
    if (!text || size > INT_MAX || memchr(text, 0, size)) return 0;
    if (!size) return 1;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)size, NULL, 0) > 0;
}
/* 在 Windows API 边界将 UTF-8 转为 UTF-16；容量按 WCHAR 个数计算，非法输入或容量不足返回 0。 */
int drop_to_wide(const char *text, WCHAR *output, size_t capacity) {
    return text && output && capacity <= INT_MAX && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, output, (int)capacity) > 0;
}
/* 将 Windows UTF-16 字符串转为 UTF-8；容量按字节计算，转换失败返回 0。 */
int drop_to_utf8(const WCHAR *text, char *output, size_t capacity) {
    return text && output && capacity <= INT_MAX && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, output, (int)capacity, NULL, NULL) > 0;
}
/* 解析 HTTP 或 HTTPS 服务器根地址，拒绝用户信息、查询和业务路径；保留端口及明确的加密模式。 */
int drop_url_parse(const WCHAR *text, drop_url *url) {
    URL_COMPONENTS parts;
    size_t length;
    if (!text || !url || (length = wcslen(text)) == 0 || length > 2048 || wcspbrk(text, L"\r\n\t ") || wcsstr(text, L"@")) return 0;
    ZeroMemory(&parts, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(text, 0, 0, &parts) || (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP) || !parts.nPort || !parts.dwHostNameLength ||
        parts.dwHostNameLength >= 256 || parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength ||
        (parts.dwUrlPathLength && !(parts.dwUrlPathLength == 1 && parts.lpszUrlPath[0] == L'/')))
        return 0;
    memcpy(url->host, parts.lpszHostName, parts.dwHostNameLength * sizeof(WCHAR));
    url->host[parts.dwHostNameLength] = 0;
    url->port = parts.nPort;
    url->secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return 1;
}
/* 递归检查 JSON 的深度、字符串及重复键，防止不同解析方式对同一请求产生歧义。 */
static int json_valid(const cJSON *node, unsigned depth) {
    const cJSON *child, *other;
    unsigned count = 0;
    if (depth > 16) return 0;
    if (cJSON_IsString(node) && !drop_utf8_valid(node->valuestring, strlen(node->valuestring))) return 0;
    for (child = node->child; child; child = child->next) {
        if (++count > 256) return 0;
        if (child->string) {
            if (!drop_utf8_valid(child->string, strlen(child->string))) return 0;
            for (other = child->next; other; other = other->next)
                if (other->string && !strcmp(child->string, other->string)) return 0;
        }
        if (!json_valid(child, depth + 1)) return 0;
    }
    return 1;
}
/* 在大小和结构限制内解析完整 JSON 对象，拒绝嵌入 NUL、尾随内容及重复键；调用者用 cJSON_Delete 释放结果。 */
cJSON *drop_json_parse(const char *data, size_t size) {
    cJSON *result;
    const char *end;
    size_t i;
    unsigned nesting = 0, commas = 0;
    int quoted = 0;
    if (!size || size > DROP_MAX_JSON || !drop_utf8_valid(data, size)) return NULL;
    /* cJSON 使用 NUL 结尾字符串；解析前拒绝转义 NUL，防止校验长度与实际字符串内容不一致。 */
    for (i = 0; i < size; ++i) {
        if (data[i] == '"') {
            quoted = !quoted;
            continue;
        }
        if (quoted && data[i] == '\\' && i + 1 < size) {
            if (i + 5 < size && !memcmp(data + i + 1, "u0000", 5)) return NULL;
            ++i;
        } else if (!quoted) {
            if ((data[i] == '{' || data[i] == '[') && ++nesting > 16) return NULL;
            if (data[i] == '}' || data[i] == ']') {
                if (!nesting) return NULL;
                --nesting;
            }
            if (data[i] == ',' && ++commas > 4096) return NULL;
        }
    }
    result = cJSON_ParseWithLengthOpts(data, size, &end, 0);
    if (!result) return NULL;
    while (end < data + size && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        ++end;
    if (end != data + size || !cJSON_IsObject(result) || !json_valid(result, 0)) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}
/* 读取大小写精确匹配且长度受限的字符串字段；返回 JSON 内部指针，其有效期随 object 结束。 */
const char *drop_json_string(const cJSON *object, const char *key, size_t maximum) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && strlen(value->valuestring) <= maximum ? value->valuestring : NULL;
}
/* 校验正文并按配置尝试 zstd，仅在压缩后变小时保存压缩形式；成功结果由 drop_text_free 释放。 */
int drop_text_pack(const char *text, int use_zstd, int level, drop_text *output) {
    size_t size, bound, compressed;
    unsigned char *data;
    if (!text || !output || (size = strlen(text)) > DROP_MAX_BODY || !drop_utf8_valid(text, size)) return 0;
    memset(output, 0, sizeof(*output));
    if (use_zstd && (level < 1 || level > 19)) return 0;
    bound = use_zstd ? ZSTD_compressBound(size) : size + 1;
    data = malloc(bound);
    if (!data) return 0;
    compressed = use_zstd ? ZSTD_compress(data, bound, text, size, level) : size;
    if (use_zstd && ZSTD_isError(compressed)) {
        free(data);
        return 0;
    }
    output->compressed = use_zstd && compressed < size;
    if (!output->compressed) {
        memcpy(data, text, size);
        compressed = size;
    }
    output->data = data;
    output->size = compressed;
    output->original_size = size;
    return 1;
}
/* 按记录的原始长度严格解压并验证 UTF-8，拒绝损坏或额外压缩帧；成功返回的字符串由调用者 free。 */
int drop_text_unpack(const drop_text *input, char **output) {
    char *text;
    size_t size;
    if (!input || !output || input->original_size > DROP_MAX_BODY || input->size > DROP_MAX_BODY || (!input->data && input->size) ||
        (input->compressed != 0 && input->compressed != 1))
        return 0;
    text = malloc(input->original_size + 1);
    if (!text) return 0;
    if (input->compressed) {
        if (ZSTD_getFrameContentSize(input->data, input->size) != input->original_size || ZSTD_findFrameCompressedSize(input->data, input->size) != input->size) {
            free(text);
            return 0;
        }
        size = ZSTD_decompress(text, input->original_size, input->data, input->size);
        if (ZSTD_isError(size) || size != input->original_size) {
            free(text);
            return 0;
        }
    } else {
        if (input->size != input->original_size) {
            free(text);
            return 0;
        }
        if (input->size) memcpy(text, input->data, input->size);
        size = input->size;
    }
    if (!drop_utf8_valid(text, size)) {
        free(text);
        return 0;
    }
    text[size] = 0;
    *output = text;
    return 1;
}
/* 释放正文缓冲并清空结构，允许传入空指针。 */
void drop_text_free(drop_text *text) {
    if (text) {
        free(text->data);
        memset(text, 0, sizeof(*text));
    }
}

/* POSIX 使用 Unicode 码点作为内部宽字符，磁盘和协议始终使用严格 UTF-8，不依赖进程 locale。 */
#include "drop_common.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <limits.h>

/* 消费一个完整 UTF-8 码点，拒绝过长编码、代理项、截断和超出 Unicode 范围的值。 */
static int decode(const unsigned char **cursor, const unsigned char *end, uint32_t *value) {
    const unsigned char *p = *cursor;
    uint32_t v, minimum;
    unsigned remaining;
    if (p == end) return 0;
    if (*p < 128) {
        v = *p++;
        remaining = 0;
        minimum = 0;
    } else if (*p >= 0xc2 && *p <= 0xdf) {
        v = *p++ & 31;
        remaining = 1;
        minimum = 128;
    } else if (*p >= 0xe0 && *p <= 0xef) {
        v = *p++ & 15;
        remaining = 2;
        minimum = 2048;
    } else if (*p >= 0xf0 && *p <= 0xf4) {
        v = *p++ & 7;
        remaining = 3;
        minimum = 65536;
    } else
        return 0;
    if ((size_t)(end - p) < remaining) return 0;
    while (remaining--) {
        if ((*p & 0xc0) != 0x80) return 0;
        v = (v << 6) | (*p++ & 63);
    }
    if (!v || v < minimum || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return 0;
    *cursor = p;
    *value = v;
    return 1;
}
int drop_utf8_valid(const char *text, size_t size) {
    const unsigned char *p = (const unsigned char *)text, *end;
    uint32_t v;
    if (!text || size > INT_MAX) return 0;
    end = p + size;
    while (p < end)
        if (!decode(&p, end, &v)) return 0;
    return 1;
}
int drop_to_wide(const char *text, WCHAR *output, size_t capacity) {
    const unsigned char *p = (const unsigned char *)text, *end;
    uint32_t v;
    size_t count = 0;
    if (!text || !output || !capacity) return 0;
    output[0] = 0;
    end = p + strlen(text);
    while (p < end) {
        if (count + 1 >= capacity || !decode(&p, end, &v)) {
            output[0] = 0;
            return 0;
        }
        output[count++] = (WCHAR)v;
    }
    output[count] = 0;
    return 1;
}
int drop_to_utf8(const WCHAR *text, char *output, size_t capacity) {
    size_t count = 0;
    if (!text || !output || !capacity) return 0;
    output[0] = 0;
    for (; *text; ++text) {
        uint32_t v = (uint32_t)*text;
        unsigned n = v < 128 ? 1 : v < 2048 ? 2 : v < 65536 ? 3 : 4;
        if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff) || n >= capacity - count) {
            output[0] = 0;
            return 0;
        }
        if (n == 1)
            output[count++] = (char)v;
        else {
            output[count++] = (char)((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | (v >> (6 * (n - 1))));
            while (--n)
                output[count++] = (char)(0x80 | ((v >> (6 * (n - 1))) & 63));
        }
    }
    output[count] = 0;
    return 1;
}
/* 只接受完整 HTTP(S) origin；libcurl 处理 IPv6 和端口，公开配置不能带凭据、查询、片段或业务路径。 */
int drop_url_parse(const WCHAR *text, drop_url *url) {
    char utf8[8193], *scheme = NULL, *host = NULL, *port = NULL, *path = NULL;
    CURLU *parsed = NULL;
    uint64_t number;
    int ok = 0;
    if (!text || !url || !*text || wcslen(text) > 2048 || wcspbrk(text, L"\r\n\t @?#") || !drop_to_utf8(text, utf8, sizeof(utf8))) return 0;
    /* 不能让 URL 库先消除业务路径的点段，再误将其接受为服务器根地址。 */
    const char *authority = strstr(utf8, "://"), *slash;
    if (!authority || ((slash = strchr(authority + 3, '/')) && strcmp(slash, "/"))) return 0;
    if (!(parsed = curl_url()) || curl_url_set(parsed, CURLUPART_URL, utf8, 0) || curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) ||
        (strcmp(scheme, "http") && strcmp(scheme, "https")) || curl_url_get(parsed, CURLUPART_HOST, &host, 0) || !*host ||
        curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) || !drop_u64_parse(port, &number) || !number || number > 65535 ||
        curl_url_get(parsed, CURLUPART_PATH, &path, 0) || strcmp(path, "/") || !drop_to_wide(host, url->host, 256))
        goto done;
    url->port = (INTERNET_PORT)number;
    url->secure = !strcmp(scheme, "https");
    ok = 1;
done:
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
    curl_free(path);
    curl_url_cleanup(parsed);
    return ok;
}

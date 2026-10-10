#include "drop_common.h"
#include <stdlib.h>
#include <string.h>

static char *trim(char *text) {
    char *end; while (*text == ' ' || *text == '\t' || *text == '\r') ++text;
    end = text + strlen(text); while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
    *end = 0; return text;
}
int drop_config_load(const WCHAR *path, drop_config *config) {
    unsigned char *bytes; DWORD length; char *line, *next, *key, *value, *equal; unsigned section = 0, seen = 0, bit;
    uint64_t number; drop_url url; int result = 0;
    if (!config || !drop_file_read(path, &bytes, &length, 65536)) return 0;
    if (!drop_utf8_valid((char *)bytes, length)) { free(bytes); return 0; }
    memset(config, 0, sizeof(*config));
    config->heartbeat_seconds = 30; config->connection_timeout_seconds = 90; config->reconcile_seconds = 300;
    config->zstd_level = 3;
    line = (char *)bytes;
    if (length >= 3 && !memcmp(bytes, "\xef\xbb\xbf", 3)) line += 3;
    while (*line) {
        next = strchr(line, '\n'); if (next) *next++ = 0;
        key = trim(line);
        if (!*key || *key == ';' || *key == '#') goto next_line;
        if (*key == '[') {
            if (!strcmp(key, "[server]")) section = 1;
            else if (!strcmp(key, "[sync]")) section = 2;
            else if (!strcmp(key, "[storage]")) section = 3;
            else goto cleanup;
            goto next_line;
        }
        equal = strchr(key, '='); if (!equal) goto cleanup;
        *equal = 0; value = trim(equal+1); key = trim(key); bit = 0;
        if (section == 1 && !strcmp(key, "url")) {
            bit = 1;
            if (!drop_to_wide(value, config->server_url, 2049) || !drop_url_parse(config->server_url, &url)) goto cleanup;
        } else if (section == 1 && !strcmp(key, "ca_file")) {
            bit = 64;
            if (!drop_to_wide(value, config->ca_file, 32768)) goto cleanup;
        } else if (section == 2) {
            if (!drop_u64_parse(value, &number) || number > 86400 || number < 1) goto cleanup;
            if (!strcmp(key, "heartbeat_seconds")) { bit = 2; config->heartbeat_seconds = (unsigned)number; }
            else if (!strcmp(key, "connection_timeout_seconds")) { bit = 4; config->connection_timeout_seconds = (unsigned)number; }
            else if (!strcmp(key, "reconcile_seconds")) { bit = 8; config->reconcile_seconds = (unsigned)number; }
            else goto cleanup;
        } else if (section == 3 && !strcmp(key, "text_compression")) {
            bit = 16;
            if (!strcmp(value, "none")) config->use_zstd = 0;
            else if (!strcmp(value, "zstd")) config->use_zstd = 1;
            else goto cleanup;
        } else if (section == 3 && !strcmp(key, "zstd_level")) {
            bit = 32;
            if (!drop_u64_parse(value, &number) || number < 1 || number > 19) goto cleanup;
            config->zstd_level = (int)number;
        } else goto cleanup;
        if (seen & bit) goto cleanup; seen |= bit;
next_line:
        if (!next) break; line = next;
    }
    result = (seen & 1) && config->connection_timeout_seconds > config->heartbeat_seconds;
cleanup:
    free(bytes); if (!result) memset(config, 0, sizeof(*config)); return result;
}

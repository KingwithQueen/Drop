#ifndef DROP_COMMON_H
#define DROP_COMMON_H
#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <stddef.h>
#include <cjson/cJSON.h>

#define DROP_VERSION "0.1.0"
#define DROP_PROTOCOL_VERSION 1
#define DROP_SCHEMA_VERSION 1
#define DROP_MAX_BODY (1024u * 1024u)
#define DROP_MAX_JSON (2u * 1024u * 1024u)
#define DROP_MAX_ATTACHMENTS 32
#define DROP_ID_CAP 33
#define DROP_TOKEN_CAP 65

typedef enum drop_error {
    DROP_OK = 0, DROP_E_ARGUMENT, DROP_E_LIMIT, DROP_E_IO, DROP_E_CRYPTO,
    DROP_E_DATABASE, DROP_E_AUTH, DROP_E_CONFLICT, DROP_E_PROTOCOL,
    DROP_E_NETWORK, DROP_E_CERTIFICATE, DROP_E_UNSUPPORTED
} drop_error;
typedef struct drop_url { WCHAR host[256]; INTERNET_PORT port; } drop_url;
typedef struct drop_text {
    unsigned char *data;
    size_t size;
    size_t original_size;
    int compressed;
} drop_text;

const char *drop_error_name(drop_error error);
int drop_init(void);
int drop_u64_parse(const char *text, uint64_t *value);
int drop_id_valid(const char *text);
void drop_id_new(char output[DROP_ID_CAP]);
void drop_token_new(char output[DROP_TOKEN_CAP]);
int drop_utf8_valid(const char *text, size_t size);
int drop_to_wide(const char *text, WCHAR *output, size_t capacity);
int drop_to_utf8(const WCHAR *text, char *output, size_t capacity);
int drop_url_parse(const WCHAR *text, drop_url *url);
cJSON *drop_json_parse(const char *data, size_t size);
const char *drop_json_string(const cJSON *object, const char *key, size_t maximum);
int drop_text_pack(const char *text, int use_zstd, int level, drop_text *output);
int drop_text_unpack(const drop_text *input, char **output);
void drop_text_free(drop_text *text);
int drop_path_join(const WCHAR *root, const WCHAR *name, WCHAR *path, size_t capacity);
int drop_private_directory(const WCHAR *path);
int drop_file_write(const WCHAR *path, const void *data, DWORD size, int replace);
int drop_file_read(const WCHAR *path, unsigned char **data, DWORD *size, DWORD limit);
int drop_secret_save(const WCHAR *path, const void *data, DWORD size);
int drop_secret_load(const WCHAR *path, unsigned char **data, DWORD *size);
int drop_password_read(const WCHAR *prompt, char *password, size_t capacity);
int drop_common_test(const WCHAR *directory);
#endif

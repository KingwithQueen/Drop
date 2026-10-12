/* Secret Service 保存随机主密钥，受桌面密钥环的锁定和解锁控制；无服务时明确失败。 */
#include <libsecret/secret.h>
#include <sodium.h>
#include <string.h>

static const SecretSchema schema = {
    "win.jsnowly.Drop", SECRET_SCHEMA_NONE, {{"directory", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}}, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/* 只在调用者持有目录锁时创建主密钥，避免两个实例同时覆盖同一目录的解密材料。 */
int drop_system_key(const char *identity, unsigned char key[32], int create) {
    GError *error = NULL;
    gchar *stored = secret_password_lookup_sync(&schema, NULL, &error, "directory", identity, NULL);
    char hex[65];
    int ok = 0;
    if (error) goto done;
    if (stored)
        ok = strlen(stored) == 64 && sodium_hex2bin(key, 32, stored, 64, NULL, NULL, NULL) == 0;
    else if (create) {
        randombytes_buf(key, 32);
        sodium_bin2hex(hex, sizeof(hex), key, 32);
        ok = secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, "Drop 登录凭据主密钥", hex, NULL, &error, "directory", identity, NULL);
        sodium_memzero(hex, sizeof(hex));
    }
done:
    if (stored) secret_password_free(stored);
    if (error) g_error_free(error);
    if (!ok) sodium_memzero(key, 32);
    return ok;
}

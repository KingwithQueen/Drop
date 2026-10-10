#include "drop_common.h"
#include "object.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL %s:%d\n", __FILE__, __LINE__); return 0; } } while (0)
#if DROP_DIAGNOSTICS
int drop_common_test(const WCHAR *directory) {
    uint64_t value; cJSON *json; WCHAR path[32768]; drop_url url; char id[DROP_ID_CAP];
    unsigned char *secret = NULL, *protected_data = NULL; DWORD size, protected_size;
    const char *sample = "中文消息与文件名 😀"; WCHAR wide[128]; char utf8[512], large[16385];
    drop_text packed; char *unpacked = NULL;
    drop_config config;
    const char *configuration = "[server]\nurl=https://[::1]:8443\n[storage]\ntext_compression=none\nzstd_level=3\n";
    const char *invalid_configuration = "[server]\nurl=https://localhost:8443\npassword=forbidden\n";
    unsigned char key[32], nonce[24], ciphertext[128], plaintext[128]; unsigned long long encrypted_size, plain_size;
    CHECK(drop_init()); CHECK(drop_object_test());
    CHECK(drop_u64_parse("18446744073709551615", &value) && value == UINT64_MAX);
    CHECK(drop_u64_parse("4294967296", &value) && value == UINT64_C(4294967296));
    CHECK(!drop_u64_parse("18446744073709551616", &value));
    CHECK(!drop_u64_parse("01", &value) && !drop_u64_parse("-1", &value) && !drop_u64_parse(" 1", &value));
    drop_id_new(id); CHECK(drop_id_valid(id) && !drop_id_valid("../object"));
    CHECK(drop_to_wide(sample, wide, 128) && drop_to_utf8(wide, utf8, 512) && !strcmp(sample, utf8));
    CHECK(!drop_utf8_valid("\xc0\xaf", 2));
    CHECK(drop_url_parse(L"https://localhost:8443", &url) && url.port == 8443);
    CHECK(drop_url_parse(L"https://[::1]:8443", &url));
    CHECK(!drop_url_parse(L"http://localhost", &url) && !drop_url_parse(L"https://user:pass@host", &url));
    CHECK(!drop_url_parse(L"https://host/api?token=x", &url));
    json = drop_json_parse("{\"sequence\":\"4294967296\"}", strlen("{\"sequence\":\"4294967296\"}")); CHECK(json); cJSON_Delete(json);
    CHECK(!drop_json_parse("{\"x\":1,\"x\":2}", strlen("{\"x\":1,\"x\":2}")));
    CHECK(!drop_json_parse("{\"x\":\"\\u0000\"}", strlen("{\"x\":\"\\u0000\"}")));
    CHECK(!drop_json_parse("{}junk", 6));
    json = drop_json_parse("{\"x\":\"\\\\u0000\"}", strlen("{\"x\":\"\\\\u0000\"}")); CHECK(json); cJSON_Delete(json);
    memset(large, 'a', sizeof(large)-1); large[sizeof(large)-1] = 0;
    CHECK(drop_text_pack(large, 1, 3, &packed) && packed.compressed);
    CHECK(drop_text_unpack(&packed, &unpacked) && !strcmp(large, unpacked)); free(unpacked);
    packed.data[0] ^= 1; CHECK(!drop_text_unpack(&packed, &unpacked)); drop_text_free(&packed);
    CHECK(drop_text_pack(sample, 0, 3, &packed) && !packed.compressed);
    CHECK(drop_text_unpack(&packed, &unpacked) && !strcmp(sample, unpacked)); free(unpacked); drop_text_free(&packed);
    CHECK(!drop_text_pack(sample, 1, 99, &packed));
    CHECK(drop_path_join(directory, L"test-config.ini", path, 32768));
    CHECK(drop_file_write(path, configuration, (DWORD)strlen(configuration), 1));
    CHECK(drop_config_load(path, &config) && !config.use_zstd && config.heartbeat_seconds == 30);
    CHECK(drop_file_write(path, invalid_configuration, (DWORD)strlen(invalid_configuration), 1));
    CHECK(!drop_config_load(path, &config)); CHECK(DeleteFileW(path));
    CHECK(drop_path_join(directory, L"user-secret.dpapi", path, 32768));
    DeleteFileW(path); CHECK(drop_secret_save(path, sample, (DWORD)strlen(sample)));
    CHECK(drop_secret_load(path, &secret, &size) && size == strlen(sample) && !memcmp(secret, sample, size));
    sodium_memzero(secret, size); free(secret);
    CHECK(drop_file_read(path, &protected_data, &protected_size, 65536));
    protected_data[protected_size/2] ^= 0x80; CHECK(drop_file_write(path, protected_data, protected_size, 1)); free(protected_data);
    CHECK(!drop_secret_load(path, &secret, &size)); CHECK(DeleteFileW(path));
    randombytes_buf(key, sizeof(key)); randombytes_buf(nonce, sizeof(nonce));
    CHECK(crypto_aead_xchacha20poly1305_ietf_encrypt(ciphertext, &encrypted_size,
        (const unsigned char *)sample, strlen(sample), (const unsigned char *)id, 32, NULL, nonce, key) == 0);
    CHECK(crypto_aead_xchacha20poly1305_ietf_decrypt(plaintext, &plain_size, NULL, ciphertext, encrypted_size,
        (const unsigned char *)id, 32, nonce, key) == 0 && plain_size == strlen(sample) && !memcmp(plaintext, sample, (size_t)plain_size));
    ciphertext[0] ^= 1;
    CHECK(crypto_aead_xchacha20poly1305_ietf_decrypt(plaintext, &plain_size, NULL, ciphertext, encrypted_size,
        (const unsigned char *)id, 32, nonce, key) != 0);
    sodium_memzero(key, sizeof(key));
    puts("PASS common: u64 UTF-8 URL strict-JSON zstd DPAPI AEAD-tamper"); return 1;
}

#endif

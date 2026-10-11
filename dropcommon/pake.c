/* PAKE 密码派生、方向密钥和认证帧工具；握手算法来自固定上游库，本文件只负责协议组合。 */
#include "pake.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 按小端字节序编码序号，保证会话帧在不同架构间一致。 */
static void put64(unsigned char *p, uint64_t n) {
    unsigned i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)(n >> (i * 8));
}
/* 按小端字节序读取帧序号；调用者保证输入长度已校验。 */
static uint64_t get64(const unsigned char *p) {
    unsigned i;
    uint64_t n = 0;
    for (i = 0; i < 8; i++)
        n |= (uint64_t)p[i] << (i * 8);
    return n;
}
/* 使用握手序号派生本次 WebSocket 的双向子密钥，隔离不同连接的 nonce 空间。 */
void drop_pake_channel_derive(drop_pake_keys *keys, uint64_t sequence) {
    unsigned char input[21], derived[32];
    unsigned i;
    memcpy(input, "ws-channel-v1", 13);
    put64(input + 13, sequence);
    for (i = DROP_PAKE_WS_TX; i <= DROP_PAKE_WS_RX; i++) {
        crypto_generichash(derived, 32, input, sizeof(input), keys->key[i], 32);
        memcpy(keys->key[i], derived, 32);
    }
    sodium_memzero(derived, sizeof(derived));
}
/* 先清零秘密或明文缓冲再释放，允许传入空指针。 */
void drop_pake_bytes_free(unsigned char *bytes, size_t size) {
    if (bytes) {
        sodium_memzero(bytes, size);
        free(bytes);
    }
}
/* 要求十六进制文本恰好匹配目标长度，拒绝部分解码和多余字符。 */
int drop_pake_hex(const char *text, unsigned char *bytes, size_t size) {
    size_t written = 0;
    return text && strlen(text) == size * 2 && sodium_hex2bin(bytes, size, text, size * 2, NULL, &written, NULL) == 0 && written == size;
}
/* 将固定服务器 ID 与大小写保留的账号组合成 SRP 身份，拒绝非法分隔符和超长账号。 */
int drop_pake_identity(const char *server, const char *login, char identity[128]) {
    size_t n = login ? strlen(login) : 0;
    return drop_id_valid(server) && n && n <= 64 && drop_utf8_valid(login, n) && !strpbrk(login, "\r\n:") && sprintf_s(identity, 128, "drop-pake-v1:%s:%s", server, login) > 0;
}
/* 使用账号盐和 Argon2id 派生 SRP 使用的密码材料；检查 UTF-8 和长度，调用者须清零输出。 */
int drop_pake_password(const char *password, const unsigned char salt[16], unsigned char secret[32]) {
    size_t n = password ? strlen(password) : 0;
    return n >= 12 && n <= 1024 && drop_utf8_valid(password, n) &&
        crypto_pwhash(secret, 32, password, n, salt, crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) == 0;
}
/* 绑定协议套件、服务器和会话 ID 派生六组方向密钥；失败清空密钥结构，临时主密钥始终清零。 */
int drop_pake_derive(drop_pake_keys *keys, const unsigned char secret[32], const char *server, const char *session) {
    unsigned char master[32], input[sizeof(DROP_PAKE_SUITE) + 32 + 16];
    unsigned i;
    int ok = 0;
    if (!drop_id_valid(server) || !drop_pake_hex(session, keys->id, 16)) return 0;
    memcpy(input, DROP_PAKE_SUITE, sizeof(DROP_PAKE_SUITE));
    memcpy(input + sizeof(DROP_PAKE_SUITE), server, 32);
    memcpy(input + sizeof(DROP_PAKE_SUITE) + 32, keys->id, 16);
    if (crypto_generichash(master, 32, input, sizeof(input), secret, 32)) goto done;
    for (i = 0; i < 6; i++)
        if (crypto_kdf_derive_from_key(keys->key[i], 32, (uint64_t)i, "DrpPAKE1", master)) goto done;
    ok = 1;
done:
    sodium_memzero(master, sizeof(master));
    if (!ok) sodium_memzero(keys, sizeof(*keys));
    return ok;
}
/* 将帧头与路由上下文摘要组合为附加认证数据，使密文无法换到其他接口使用。 */
static int aad(const unsigned char *header, const char *context, unsigned char output[60]) {
    size_t n = context ? strlen(context) : 0;
    if (!n || n > 4096) return 0;
    memcpy(output, header, DROP_PAKE_FRAME_HEADER);
    return crypto_hash_sha256(output + DROP_PAKE_FRAME_HEADER, (const unsigned char *)context, n) == 0;
}
/* 用指定方向密钥和序号生成认证帧；同一密钥下序号不得复用，成功输出用 drop_pake_bytes_free 释放。 */
int drop_pake_encrypt(
    const drop_pake_keys *keys, unsigned key, uint64_t sequence, const char *context, const unsigned char *plain, size_t size, unsigned char **frame, DWORD *length) {
    unsigned char nonce[24], associated[60], *out;
    unsigned long long written = 0;
    if (!keys || key >= 6 || !frame || !length || size > DROP_MAX_JSON + 65u || (size && !plain)) return 0;
    *frame = NULL;
    *length = 0;
    out = malloc(size + DROP_PAKE_FRAME_OVERHEAD);
    if (!out) return 0;
    memcpy(out, "DPK1", 4);
    memcpy(out + 4, keys->id, 16);
    put64(out + 20, sequence);
    memcpy(nonce, keys->id, 16);
    put64(nonce + 16, sequence);
    if (!aad(out, context, associated) ||
        crypto_aead_xchacha20poly1305_ietf_encrypt(out + DROP_PAKE_FRAME_HEADER, &written, plain, size, associated, sizeof(associated), NULL, nonce, keys->key[key])) {
        free(out);
        return 0;
    }
    *frame = out;
    *length = (DWORD)(written + DROP_PAKE_FRAME_HEADER);
    return 1;
}
/* 验证会话 ID、路由上下文及认证标签后返回明文和序号；调用者另行检查序号连续性并释放输出。 */
int drop_pake_decrypt(
    const drop_pake_keys *keys, unsigned key, const char *context, const unsigned char *frame, size_t length, uint64_t *sequence, unsigned char **plain, DWORD *size) {
    unsigned char nonce[24], associated[60], *out;
    unsigned long long written = 0;
    if (!keys || key >= 6 || !plain || !size || !sequence || !frame || length < DROP_PAKE_FRAME_OVERHEAD || length > DROP_PAKE_FRAME_LIMIT || memcmp(frame, "DPK1", 4) ||
        sodium_memcmp(frame + 4, keys->id, 16))
        return 0;
    *plain = NULL;
    *size = 0;
    out = malloc(length + 1);
    if (!out) return 0;
    memcpy(nonce, frame + 4, 16);
    memcpy(nonce + 16, frame + 20, 8);
    if (!aad(frame, context, associated) ||
        crypto_aead_xchacha20poly1305_ietf_decrypt(
            out, &written, NULL, frame + DROP_PAKE_FRAME_HEADER, length - DROP_PAKE_FRAME_HEADER, associated, sizeof(associated), nonce, keys->key[key])) {
        drop_pake_bytes_free(out, length + 1);
        return 0;
    }
    out[written] = 0;
    *plain = out;
    *size = (DWORD)written;
    *sequence = get64(frame + 20);
    return 1;
}

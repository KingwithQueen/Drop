#ifndef DROP_PAKE_H
#define DROP_PAKE_H
#include "drop_common.h"
#include "../vendor/csrp/srp.h"

#define DROP_PAKE_SUITE "srp6a-sha256-4096-argon2id-xchacha20poly1305-v1"
#define DROP_PAKE_FRAME_HEADER 28u
#define DROP_PAKE_FRAME_OVERHEAD 44u
#define DROP_PAKE_FRAME_LIMIT (DROP_MAX_JSON + 128u)
enum { DROP_PAKE_FINISH_TX, DROP_PAKE_FINISH_RX, DROP_PAKE_HTTP_TX, DROP_PAKE_HTTP_RX, DROP_PAKE_WS_TX, DROP_PAKE_WS_RX };
/* 绑定会话 ID 的六组方向密钥，分别用于握手完成、HTTP 和 WebSocket 的双向传输。 */
typedef struct drop_pake_keys {
    unsigned char id[16], key[6][32];
} drop_pake_keys;
/* 服务器持久账号 PAKE 材料；保存独立盐和验证器，version 标识重置后的材料代次，不含明文密码。 */
typedef struct drop_pake_record {
    unsigned char salt[16], srp_salt[32], verifier[512];
    unsigned verifier_size;
    char version[DROP_ID_CAP];
} drop_pake_record;
/* 通过 Windows 一次性初始化机制保证多个线程只初始化 SRP 随机源一次。 */
int drop_srp_initialize(void);
/* 校验 SRP 公值严格位于 0 与固定 4096 位模数之间，拒绝零值及越界编码。 */
int drop_srp_public_valid(const unsigned char *bytes, size_t size);
/* 将固定服务器 ID 与大小写保留的账号组合成 SRP 身份，拒绝非法分隔符和超长账号。 */
int drop_pake_identity(const char *server, const char *login, char identity[128]);
/* 使用账号盐和 Argon2id 派生 SRP 使用的密码材料；检查 UTF-8 和长度，调用者须清零输出。 */
int drop_pake_password(const char *password, const unsigned char salt[16], unsigned char secret[32]);
/* 要求十六进制文本恰好匹配目标长度，拒绝部分解码和多余字符。 */
int drop_pake_hex(const char *text, unsigned char *bytes, size_t size);
/* 绑定协议套件、服务器和会话 ID 派生六组方向密钥；失败清空密钥结构，临时主密钥始终清零。 */
int drop_pake_derive(drop_pake_keys *keys, const unsigned char secret[32], const char *server, const char *session);
/* 使用握手序号派生本次 WebSocket 的双向子密钥，隔离不同连接的 nonce 空间。 */
void drop_pake_channel_derive(drop_pake_keys *keys, uint64_t sequence);
/* 用指定方向密钥和序号生成认证帧；同一密钥下序号不得复用，成功输出用 drop_pake_bytes_free 释放。 */
int drop_pake_encrypt(
    const drop_pake_keys *keys, unsigned key, uint64_t sequence, const char *context, const unsigned char *plain, size_t size, unsigned char **frame, DWORD *length);
/* 验证会话 ID、路由上下文及认证标签后返回明文和序号；调用者另行检查序号连续性并释放输出。 */
int drop_pake_decrypt(
    const drop_pake_keys *keys, unsigned key, const char *context, const unsigned char *frame, size_t length, uint64_t *sequence, unsigned char **plain, DWORD *size);
/* 先清零秘密或明文缓冲再释放，允许传入空指针。 */
void drop_pake_bytes_free(unsigned char *bytes, size_t size);
#endif

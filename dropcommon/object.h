#ifndef DROP_OBJECT_H
#define DROP_OBJECT_H
#include "drop_common.h"
#define DROP_CHUNK_SIZE (1024u * 1024u)
#define DROP_OBJECT_HEADER_SIZE 260u
#define DROP_CHUNK_OVERHEAD 100u
/* 已认证的附件头及内存内容密钥；所有块绑定同一对象身份，结束使用后须清零密钥。 */
typedef struct drop_object {
    char id[DROP_ID_CAP];
    uint64_t size, chunks;
    unsigned char key[32], sha256[32], ciphertext_digest[32];
    unsigned char header[DROP_OBJECT_HEADER_SIZE];
} drop_object;
/* 检查 SHA-256 的固定长度小写十六进制表示。 */
int drop_hash_valid(const char *hex);
/* 将合法 SHA-256 文本转换为 32 字节摘要，失败返回 0。 */
int drop_hash_decode(const char *hex, unsigned char hash[32]);
/* 将 32 字节摘要写成带终止符的小写十六进制字符串。 */
void drop_hash_hex(const unsigned char hash[32], char hex[65]);
/* 流式计算文件 SHA-256 和 64 位长度，避免一次读入整个附件；读取失败不报告完成。 */
int drop_hash_file(const WCHAR *path, unsigned char hash[32], uint64_t *size);
/* 为附件生成独立内容密钥并用主密钥保护，构造绑定 ID、长度和摘要的版本化对象头。 */
int drop_object_create(const unsigned char master[32], const char *id, uint64_t size, const unsigned char sha256[32], drop_object *object);
/* 校验对象头格式与认证信息，并用主密钥解开内容密钥；失败不接受未认证元数据。 */
int drop_object_open(const unsigned char master[32], const unsigned char *header, size_t length, drop_object *object);
/* 将密文摘要写入对象头并认证完整清单，完成后对象才可作为完整附件发布。 */
int drop_object_seal(drop_object *object, const unsigned char ciphertext_digest[32]);
/* 计算指定块的明文长度，最后一块可短于标准块；越界块返回 0。 */
DWORD drop_object_chunk_length(const drop_object *object, uint64_t index);
/* 使用独立随机 nonce 加密并认证一个合法分块；成功分配的密文由调用者清理和释放。 */
int drop_chunk_encrypt(const drop_object *object, uint64_t index, const unsigned char *plain, DWORD length, unsigned char **encrypted, DWORD *encrypted_length);
/* 检查块头、长度和认证标签后返回明文；失败不交付内容，成功缓冲由调用者清零并 free。 */
int drop_chunk_decrypt(const drop_object *object, uint64_t index, const unsigned char *encrypted, DWORD length, unsigned char **plain, DWORD *plain_length);
/* 验证对象头和分块往返，以及篡改、截断和越界拒绝；仅用于本机诊断构建。 */
int drop_object_test(void);
#endif

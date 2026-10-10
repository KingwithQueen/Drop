#ifndef DROP_OBJECT_H
#define DROP_OBJECT_H
#include "drop_common.h"
#define DROP_CHUNK_SIZE (1024u * 1024u)
#define DROP_OBJECT_HEADER_SIZE 260u
#define DROP_CHUNK_OVERHEAD 100u
typedef struct drop_object {
    char id[DROP_ID_CAP];
    uint64_t size, chunks;
    unsigned char key[32], sha256[32], ciphertext_digest[32];
    unsigned char header[DROP_OBJECT_HEADER_SIZE];
} drop_object;
int drop_hash_valid(const char *hex);
int drop_hash_decode(const char *hex, unsigned char hash[32]);
void drop_hash_hex(const unsigned char hash[32], char hex[65]);
int drop_hash_file(const WCHAR *path,unsigned char hash[32],uint64_t *size);
int drop_object_create(const unsigned char master[32], const char *id, uint64_t size,
    const unsigned char sha256[32], drop_object *object);
int drop_object_open(const unsigned char master[32], const unsigned char *header, size_t length, drop_object *object);
int drop_object_seal(drop_object *object, const unsigned char ciphertext_digest[32]);
DWORD drop_object_chunk_length(const drop_object *object, uint64_t index);
int drop_chunk_encrypt(const drop_object *object, uint64_t index, const unsigned char *plain, DWORD length,
    unsigned char **encrypted, DWORD *encrypted_length);
int drop_chunk_decrypt(const drop_object *object, uint64_t index, const unsigned char *encrypted, DWORD length,
    unsigned char **plain, DWORD *plain_length);
int drop_object_test(void);
#endif

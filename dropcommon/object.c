#include "object.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void put32(unsigned char *p, uint32_t value) { unsigned i; for (i=0; i<4; ++i) p[i]=(unsigned char)(value>>(8*i)); }
static void put64(unsigned char *p, uint64_t value) { unsigned i; for (i=0; i<8; ++i) p[i]=(unsigned char)(value>>(8*i)); }
static uint32_t get32(const unsigned char *p) { unsigned i; uint32_t value=0; for(i=0;i<4;++i) value |= (uint32_t)p[i]<<(8*i); return value; }
static uint64_t get64(const unsigned char *p) { unsigned i; uint64_t value=0; for(i=0;i<8;++i) value |= (uint64_t)p[i]<<(8*i); return value; }
int drop_hash_valid(const char *hex) {
    size_t i; if (!hex || strlen(hex)!=64) return 0;
    for(i=0;i<64;++i) if (!((hex[i]>='0'&&hex[i]<='9')||(hex[i]>='a'&&hex[i]<='f'))) return 0;
    return 1;
}
int drop_hash_decode(const char *hex, unsigned char hash[32]) {
    return drop_hash_valid(hex) && sodium_hex2bin(hash,32,hex,64,NULL,NULL,NULL)==0;
}
void drop_hash_hex(const unsigned char hash[32], char hex[65]) { sodium_bin2hex(hex,65,hash,32); }
int drop_hash_file(const WCHAR *path,unsigned char hash[32],uint64_t *size) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    unsigned char *buffer=NULL; LARGE_INTEGER length; DWORD read=0; uint64_t total=0; crypto_hash_sha256_state state; int ok=0;
    if(file==INVALID_HANDLE_VALUE) return 0;
    buffer=malloc(DROP_CHUNK_SIZE); if(!buffer || !GetFileSizeEx(file,&length) || length.QuadPart<0) goto done;
    crypto_hash_sha256_init(&state);
    for(;;) { if(!ReadFile(file,buffer,DROP_CHUNK_SIZE,&read,NULL)) goto done; if(!read) break; crypto_hash_sha256_update(&state,buffer,read); total+=read; }
    if(total!=(uint64_t)length.QuadPart) goto done;
    crypto_hash_sha256_final(&state,hash); *size=total; ok=1;
done:
    if(buffer) { sodium_memzero(buffer,DROP_CHUNK_SIZE); free(buffer); } CloseHandle(file); return ok;
}
DWORD drop_object_chunk_length(const drop_object *o, uint64_t index) {
    uint64_t remaining; if(index>=o->chunks) return 0;
    remaining=o->size-index*DROP_CHUNK_SIZE;
    return (DWORD)(remaining>DROP_CHUNK_SIZE?DROP_CHUNK_SIZE:remaining);
}
int drop_object_seal(drop_object *o, const unsigned char digest[32]) {
    unsigned char manifest[80]; unsigned long long length;
    memcpy(o->ciphertext_digest,digest,32); memcpy(manifest,o->sha256,32); memcpy(manifest+32,digest,32);
    put64(manifest+64,o->size); put64(manifest+72,o->chunks);
    randombytes_buf(o->header+140,24);
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(o->header+164,&length,manifest,sizeof(manifest),
        o->header,140,NULL,o->header+140,o->key)!=0 || length!=96) return 0;
    sodium_memzero(manifest,sizeof(manifest)); return 1;
}
int drop_object_create(const unsigned char master[32],const char *id,uint64_t size,const unsigned char sha[32],drop_object *o) {
    unsigned char zero[32]={0}; unsigned long long length;
    if(!drop_id_valid(id) || size>INT64_MAX) return 0;
    memset(o,0,sizeof(*o)); strcpy_s(o->id,sizeof(o->id),id); o->size=size;
    o->chunks=size?1+(size-1)/DROP_CHUNK_SIZE:0; memcpy(o->sha256,sha,32); randombytes_buf(o->key,32);
    memcpy(o->header,"DROPOBJ1",8); put32(o->header+8,1); memcpy(o->header+12,id,32);
    put64(o->header+44,size); put32(o->header+52,DROP_CHUNK_SIZE); put64(o->header+56,o->chunks); put32(o->header+64,1);
    randombytes_buf(o->header+68,24);
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(o->header+92,&length,o->key,32,o->header,68,NULL,o->header+68,master)!=0 ||
        length!=48 || !drop_object_seal(o,zero)) { sodium_memzero(o,sizeof(*o)); return 0; }
    return 1;
}
int drop_object_open(const unsigned char master[32],const unsigned char *header,size_t size,drop_object *o) {
    unsigned char manifest[80]; unsigned long long length; int valid=0;
    memset(o,0,sizeof(*o));
    if(!header || size!=DROP_OBJECT_HEADER_SIZE || memcmp(header,"DROPOBJ1",8) || get32(header+8)!=1 ||
        get32(header+52)!=DROP_CHUNK_SIZE || get32(header+64)!=1) goto done;
    memcpy(o->id,header+12,32); o->id[32]=0; o->size=get64(header+44); o->chunks=get64(header+56);
    if(!drop_id_valid(o->id) || o->size>INT64_MAX || o->chunks!=(o->size?1+(o->size-1)/DROP_CHUNK_SIZE:0)) goto done;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(o->key,&length,NULL,header+92,48,header,68,header+68,master)!=0 || length!=32 ||
        crypto_aead_xchacha20poly1305_ietf_decrypt(manifest,&length,NULL,header+164,96,header,140,header+140,o->key)!=0 ||
        length!=80 || get64(manifest+64)!=o->size || get64(manifest+72)!=o->chunks) goto done;
    memcpy(o->sha256,manifest,32); memcpy(o->ciphertext_digest,manifest+32,32); memcpy(o->header,header,size); valid=1;
done:
    sodium_memzero(manifest,sizeof(manifest)); if(!valid) sodium_memzero(o,sizeof(*o)); return valid;
}
static void chunk_prefix(unsigned char *p,const drop_object *o,uint64_t index,DWORD length) {
    memcpy(p,"DROPCHK1",8); memcpy(p+8,o->id,32); put64(p+40,index); put64(p+48,o->size); put32(p+56,length);
}
int drop_chunk_encrypt(const drop_object *o,uint64_t index,const unsigned char *plain,DWORD size,unsigned char **out,DWORD *out_size) {
    unsigned char *buffer; unsigned long long length;
    if(size!=drop_object_chunk_length(o,index) || !size || !plain || !out || !out_size) return 0;
    buffer=malloc((size_t)size+DROP_CHUNK_OVERHEAD); if(!buffer) return 0;
    chunk_prefix(buffer,o,index,size); randombytes_buf(buffer+60,24);
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(buffer+84,&length,plain,size,buffer,60,NULL,buffer+60,o->key)!=0) { free(buffer); return 0; }
    *out=buffer; *out_size=(DWORD)length+84; return 1;
}
int drop_chunk_decrypt(const drop_object *o,uint64_t index,const unsigned char *encrypted,DWORD size,unsigned char **out,DWORD *out_size) {
    unsigned char prefix[60],*buffer; unsigned long long length; DWORD expected=drop_object_chunk_length(o,index);
    if(!encrypted || !expected || size!=expected+DROP_CHUNK_OVERHEAD || !out || !out_size) return 0;
    chunk_prefix(prefix,o,index,expected); if(memcmp(prefix,encrypted,60)) return 0;
    buffer=malloc(expected); if(!buffer) return 0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(buffer,&length,NULL,encrypted+84,size-84,encrypted,60,encrypted+60,o->key)!=0 ||
        length!=expected) { sodium_memzero(buffer,expected); free(buffer); return 0; }
    *out=buffer; *out_size=expected; return 1;
}
#if DROP_DIAGNOSTICS
int drop_object_test(void) {
    unsigned char master[32],sha[32],*encrypted=NULL,*plain=NULL; DWORD encrypted_size,plain_size; char id[DROP_ID_CAP];
    drop_object a,b; unsigned char data[3]={0,1,255}; int ok=0;
    randombytes_buf(master,32); crypto_hash_sha256(sha,data,sizeof(data)); drop_id_new(id);
    if(!drop_object_create(master,id,3,sha,&a) || !drop_object_open(master,a.header,sizeof(a.header),&b) ||
        !drop_chunk_encrypt(&a,0,data,3,&encrypted,&encrypted_size) ||
        !drop_chunk_decrypt(&b,0,encrypted,encrypted_size,&plain,&plain_size) || plain_size!=3 || memcmp(data,plain,3)) goto done;
    free(plain); plain=NULL; encrypted[encrypted_size-1]^=1;
    if(drop_chunk_decrypt(&b,0,encrypted,encrypted_size,&plain,&plain_size)) goto done;
    encrypted[encrypted_size-1]^=1;
    if(drop_chunk_decrypt(&b,1,encrypted,encrypted_size,&plain,&plain_size) ||
        drop_chunk_decrypt(&b,0,encrypted,encrypted_size-1,&plain,&plain_size)) goto done;
    master[0]^=1; if(drop_object_open(master,a.header,sizeof(a.header),&b)) goto done; master[0]^=1;
    a.header[60]^=1; if(drop_object_open(master,a.header,sizeof(a.header),&b)) goto done;
    if(!drop_object_create(master,id,UINT64_C(4294967297),sha,&a) || !drop_object_open(master,a.header,sizeof(a.header),&b) ||
        b.size!=UINT64_C(4294967297) || b.chunks!=4097 || drop_object_chunk_length(&b,4096)!=1) goto done;
    if(!drop_object_create(master,id,0,sha,&a) || !drop_object_open(master,a.header,sizeof(a.header),&b) || b.chunks) goto done;
    ok=1; puts("PASS object format: AEAD wrong-index truncation tamper empty 64-bit-length");
done:
    free(encrypted); free(plain); sodium_memzero(master,sizeof(master)); sodium_memzero(&a,sizeof(a)); sodium_memzero(&b,sizeof(b)); return ok;
}

#endif

/* 固定版本上游源码保持字节一致；以下适配器加固其内存、随机数和密码运算接口。 */
#include "pake.h"
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <sodium.h>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <stdlib.h>
#include <string.h>

/* 进程内 SRP 随机源的一次性初始化状态，供多个登录线程共用。 */
#ifdef _WIN32
static INIT_ONCE SRP初始化状态 = INIT_ONCE_STATIC_INIT;
/* 在一次性初始化回调中确认 OpenSSL 随机源就绪，再初始化上游 SRP 随机状态。 */
static BOOL CALLBACK srp_ready(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    if (RAND_status() != 1 && RAND_poll() != 1) return FALSE;
    srp_random_seed(NULL, 0);
    return TRUE;
}
/* 通过 Windows 一次性初始化机制保证多个线程只初始化 SRP 随机源一次。 */
int drop_srp_initialize(void) {
    return InitOnceExecuteOnce(&SRP初始化状态, srp_ready, NULL, NULL) != 0;
}
#else
static pthread_once_t SRP初始化状态 = PTHREAD_ONCE_INIT;
static int SRP就绪;
/* pthread_once 只发布完整初始化结果；随机源失败时所有调用者均拒绝认证。 */
static void srp_ready(void) {
    if (RAND_status() != 1 && RAND_poll() != 1) return;
    srp_random_seed(NULL, 0);
    SRP就绪 = 1;
}
int drop_srp_initialize(void) {
    return pthread_once(&SRP初始化状态, srp_ready) == 0 && SRP就绪;
}
/* 保存准确的分配长度并保持最大对齐，清零不能依赖某个平台的分配器内部大小。 */
typedef union secret_allocation {
    max_align_t alignment;
    size_t size;
} secret_allocation;
#endif
/* 为上游库提供检查分配结果的适配器；内存不足时终止进程，避免库继续使用空指针。 */
static void *checked_malloc(size_t size) {
#ifdef _WIN32
    void *p = malloc(size ? size : 1);
    if (!p) abort();
    return p;
#else
    secret_allocation *p;
    if (size > SIZE_MAX - sizeof(*p) || !(p = malloc(sizeof(*p) + (size ? size : 1)))) abort();
    p->size = size ? size : 1;
    return p + 1;
#endif
}
/* 为上游库提供检查结果的清零分配器；零尺寸也分配有效存储，失败终止进程。 */
static void *checked_calloc(size_t count, size_t size) {
#ifdef _WIN32
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) abort();
    return p;
#else
    void *p;
    size_t length;
    if (size && count > SIZE_MAX / size) abort();
    length = count * size;
    p = checked_malloc(length);
    memset(p, 0, length ? length : 1);
    return p;
#endif
}
/* 按实际分配大小清零上游库的秘密缓冲，再交还运行库；只接受本适配器分配的指针。 */
static void clear_free(void *p) {
    if (p) {
#ifdef _WIN32
        sodium_memzero(p, _msize(p));
        free(p);
#else
        secret_allocation *allocation = (secret_allocation *)p - 1;
        sodium_memzero(p, allocation->size);
        free(allocation);
#endif
    }
}
/* 使用 OpenSSL 私有随机数生成至少 256 位的秘密指数，失败终止进程。 */
static int checked_random(BIGNUM *r, int bits, int top, int bottom) {
    (void)top;
    if (BN_priv_rand(r, bits < 256 ? 256 : bits, BN_RAND_TOP_ONE, bottom) != 1) abort();
    return 1;
}
/* 将上游模幂运算适配到 OpenSSL 常量时间接口；密码运算失败时终止进程。 */
static int constant_exp(BIGNUM *r, const BIGNUM *a, const BIGNUM *p, const BIGNUM *m, BN_CTX *ctx) {
    if (BN_mod_exp_mont_consttime(r, a, p, m, ctx, NULL) != 1) abort();
    return 1;
}
#if defined(_WIN32) && !defined(WIN32)
#define WIN32
#endif
#define malloc checked_malloc
#define calloc checked_calloc
#define free clear_free
#define BN_free BN_clear_free
#define BN_rand checked_random
#define BN_mod_exp constant_exp
#define memcmp CRYPTO_memcmp
#ifdef _MSC_VER
#pragma warning(push, 0)
#pragma warning(disable : 4996)
#endif
#include "../vendor/csrp/srp.c"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#undef malloc
#undef calloc
#undef free
#undef BN_free
#undef BN_rand
#undef BN_mod_exp
#undef memcmp
/* 校验 SRP 公值严格位于 0 与固定 4096 位模数之间，拒绝零值及越界编码。 */
int drop_srp_public_valid(const unsigned char *bytes, size_t size) {
    BIGNUM *n = NULL, *value = NULL;
    int ok = 0;
    if (!bytes || !size || size > 512) return 0;
    if (!BN_hex2bn(&n, global_Ng_constants[SRP_NG_4096].n_hex) || !(value = BN_bin2bn(bytes, (int)size, NULL))) goto done;
    ok = !BN_is_zero(value) && !BN_is_negative(value) && BN_cmp(value, n) < 0;
done:
    BN_free(n);
    BN_free(value);
    return ok;
}

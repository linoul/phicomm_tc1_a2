/*
 * hk_bignum.h —— 模 3072 位素数的大数运算（Montgomery）
 *
 * 为什么自己写而不是用 SDK 里的 mbedTLS：
 *   内核库里确实有 mbedtls_mpi_exp_mod（见 doc/WEB_BL0937.md 第 11 节），
 *   但 **SDK 不提供 mbedTLS 头文件、也不提供 mbedTLS 的编译配置**
 *   （mbed_config.h 里只有 lwip/mbed-os 的项，一个 MBEDTLS_* 都没有）。
 *   看不到 MBEDTLS_MPI_MAX_SIZE 就等于不知道 mbedtls_mpi 结构体多大、
 *   exp_mod 支不支持 3072 位 —— 硬调它是在赌内存布局。
 *   自己写一份，还能在 PC 上对着 Python 的 pow() 逐位验证。
 *
 * 表示：96 个 32 位肢体（= 3072 位），小端存（d[0] 最低位）。
 * 对外与 SRP 交互时用大端字节串（SRP 的整数是大端的）。
 */
#ifndef HK_BIGNUM_H
#define HK_BIGNUM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HK_BN_LIMBS 96				  /* 96 * 32 = 3072 位 */
#define HK_BN_BYTES (HK_BN_LIMBS * 4) /* 384 字节 */

typedef struct _HK_BN
{
	uint32_t d[HK_BN_LIMBS];
} HK_BN;

/* Montgomery 上下文：模数 + 预计算量 */
typedef struct _HK_MONT
{
	HK_BN n;	  /* 模数 */
	uint32_t n0inv; /* -n^-1 mod 2^32 */
	HK_BN rmod;	 /* R mod n，R = 2^3072（Montgomery 域里的 1） */
	HK_BN r2;	 /* R^2 mod n（用于进出 Montgomery 域） */
} HK_MONT;

void hk_bn_zero(HK_BN *a);
void hk_bn_set(HK_BN *a, const HK_BN *b);
void hk_bn_from_u32(HK_BN *a, uint32_t v);
int hk_bn_is_zero(const HK_BN *a);
int hk_bn_cmp(const HK_BN *a, const HK_BN *b); /* a<b -> -1, = -> 0, > -> 1 */

/* 大端字节串 <-> HK_BN。长度必须 <= 384；to_be 会左补零到 len */
int hk_bn_from_be(HK_BN *a, const uint8_t *be, size_t len);
void hk_bn_to_be(const HK_BN *a, uint8_t *be, size_t len);

/* a = (a - b) mod 2^3072（不判借位，调用方自己保证语义） */
void hk_bn_sub_raw(HK_BN *a, const HK_BN *b);
/* a = 2a mod n */
void hk_bn_mod_double(HK_BN *a, const HK_BN *n);

/* 初始化 Montgomery 上下文（算 n0inv / R mod n / R^2 mod n） */
void hk_mont_init(HK_MONT *m, const HK_BN *n);

/* z = x*y*R^-1 mod n */
void hk_mont_mul(HK_BN *z, const HK_BN *x, const HK_BN *y, const HK_MONT *m);

/* z = x*y mod n（内部进出 Montgomery 域，比 exp 慢但语义直观） */
void hk_bn_mod_mul(HK_BN *z, const HK_BN *x, const HK_BN *y, const HK_MONT *m);

/* z = base^exp mod n，exp 是大端字节串（长度不限，最多 384 字节） */
void hk_mont_exp(HK_BN *z, const HK_BN *base, const uint8_t *exp_be, size_t exp_len,
				 const HK_MONT *m);

/*
 * 「不做模约减」的中间量大数（2048 位），只用于把几个哈希值拼成指数。
 * 典型场景：SRP 客户端要算 a + u*x —— u 和 x 都是 512 位，乘积 1024 位，
 * 这属于"指数"，绝不能对 N 取模。
 */
#define HK_BN_BIG_LIMBS 64 /* 64 * 32 = 2048 位 */
#define HK_BN_BIG_BYTES (HK_BN_BIG_LIMBS * 4)

typedef struct _HK_BN_BIG
{
	uint32_t d[HK_BN_BIG_LIMBS];
} HK_BN_BIG;

int hk_bnbig_from_be(HK_BN_BIG *a, const uint8_t *be, size_t len); /* 太长返回 -1 */
void hk_bnbig_to_be(const HK_BN_BIG *a, uint8_t *be, size_t len);
void hk_bnbig_mul(HK_BN_BIG *z, const HK_BN_BIG *a, const HK_BN_BIG *b);
void hk_bnbig_add(HK_BN_BIG *z, const HK_BN_BIG *a, const HK_BN_BIG *b);

#ifdef __cplusplus
}
#endif

#endif /* HK_BIGNUM_H */

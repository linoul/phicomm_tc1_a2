/*
 * hk_sha512.h —— SHA-512（FIPS 180-4）
 *
 * HomeKit 密码学内核的一部分。设计约束：
 *   1) 只依赖 C99 标准库（stdint/stddef/string），**不依赖任何 SDK 头文件**，
 *      这样同一份代码既能在目标板上编，也能在 PC 上用 gcc 编起来跑官方向量；
 *   2) 不用 malloc，全部状态放在调用者的结构体里（排插没有 MMU，堆越简单越好）；
 *   3) 只用 uint64_t 运算 —— Cortex-M4 没有 128 位乘，但 64 位加减/移位都是原生的。
 */
#ifndef HK_SHA512_H
#define HK_SHA512_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HK_SHA512_DIGEST_LEN 64
#define HK_SHA512_BLOCK_LEN 128

typedef struct _HK_SHA512_CTX
{
	uint64_t total;				 /* 已吃进的字节数（够用：2^64 字节） */
	uint64_t h[8];				 /* 中间状态 */
	uint8_t buf[HK_SHA512_BLOCK_LEN]; /* 未满一块的尾巴 */
	uint32_t n;					 /* buf 里已有的字节数，0..127 */
} HK_SHA512_CTX;

void hk_sha512_init(HK_SHA512_CTX *ctx);
void hk_sha512_update(HK_SHA512_CTX *ctx, const void *data, size_t len);
void hk_sha512_final(HK_SHA512_CTX *ctx, uint8_t out[HK_SHA512_DIGEST_LEN]);
void hk_sha512(const void *data, size_t len, uint8_t out[HK_SHA512_DIGEST_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* HK_SHA512_H */

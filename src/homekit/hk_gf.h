/*
 * hk_gf.h —— GF(2^255-19) 域运算（Curve25519 与 Ed25519 共用）
 *
 * 为什么单独抽出来：这两个算法用的是同一个素域 p = 2^255-19，
 * 域运算（乘、平方、逆、常数时间交换）完全一样。
 * 板上 flash 只剩几十 KB，两份拷贝是实打实的浪费。
 *
 * 表示法：16 个 16 位肢体，元素值放在 int64_t 里。
 * 允许"未规约"的中间值（可以超出 2^255），只在 pack 时才冻结 —— 
 * 这样每步省掉一次规约，在 160MHz 的 M4 上差别很明显。
 */
#ifndef HK_GF_H
#define HK_GF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef int64_t hk_gf[16];

extern const hk_gf hk_gf0;		/* 0 */
extern const hk_gf hk_gf1;		/* 1 */
extern const hk_gf hk_gf121665; /* (A-2)/4，X25519 的 a24 */
extern const hk_gf hk_gfD;		/* Ed25519 的曲线常数 d = -121665/121666 */
extern const hk_gf hk_gfD2;		/* 2d */
extern const hk_gf hk_gfX;		/* Ed25519 基点 x */
extern const hk_gf hk_gfY;		/* Ed25519 基点 y */
extern const hk_gf hk_gfI;		/* sqrt(-1) */

void hk_gf_set(hk_gf r, const hk_gf a);
void hk_gf_carry(hk_gf o);					 /* 进位传播（部分规约） */
void hk_gf_cswap(hk_gf p, hk_gf q, int b);	 /* 常数时间条件交换 */
void hk_gf_pack(uint8_t *o, const hk_gf n);	 /* 冻结并序列化成 32 字节小端 */
void hk_gf_unpack(hk_gf o, const uint8_t *n); /* 反序列化（丢最高位） */
void hk_gf_add(hk_gf o, const hk_gf a, const hk_gf b);
void hk_gf_sub(hk_gf o, const hk_gf a, const hk_gf b);
void hk_gf_mul(hk_gf o, const hk_gf a, const hk_gf b);
void hk_gf_sqr(hk_gf o, const hk_gf a);
void hk_gf_inv(hk_gf o, const hk_gf i);				/* o = i^(p-2) */
void hk_gf_pow2523(hk_gf o, const hk_gf i);			/* o = i^(2^252-3) */
int hk_gf_neq(const hk_gf a, const hk_gf b);		/* 不等返回 1 */
int hk_gf_isneg(const hk_gf a);						/* 序列化后最低位 */

/* 常数时间比较 32 字节：相等返回 0，不等返回 1 */
int hk_ct_neq32(const uint8_t *a, const uint8_t *b);

#ifdef __cplusplus
}
#endif

#endif /* HK_GF_H */

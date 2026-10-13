/*
 * hk_bignum.c —— 模 3072 位素数的 Montgomery 大数运算
 *
 * 乘法用 CIOS（Coarsely Integrated Operand Scanning，Koç/Acar/Kaliski）：
 * 乘法和 Montgomery 约减交织在同一个外层循环里，中间结果只有 s+2 个肢体，
 * 不需要 2s 的乘积缓冲 —— 3072 位下这能省一半的时间和内存。
 *
 * 所有实现都用 uint64_t 做中间量：Cortex-M4 上 UMULL 给 32x32->64 是单周期的，
 * 所以这个写法在目标板上是"用硬件刚好合适"，不依赖 64 位模拟。
 */
#include "hk_bignum.h"
#include <string.h>

void hk_bn_zero(HK_BN *a)
{
	memset(a->d, 0, sizeof(a->d));
}

void hk_bn_set(HK_BN *a, const HK_BN *b)
{
	memcpy(a->d, b->d, sizeof(a->d));
}

void hk_bn_from_u32(HK_BN *a, uint32_t v)
{
	hk_bn_zero(a);
	a->d[0] = v;
}

int hk_bn_is_zero(const HK_BN *a)
{
	uint32_t acc = 0;
	int i;

	for (i = 0; i < HK_BN_LIMBS; i++)
		acc |= a->d[i];
	return acc == 0;
}

int hk_bn_cmp(const HK_BN *a, const HK_BN *b)
{
	int i;

	for (i = HK_BN_LIMBS - 1; i >= 0; i--)
	{
		if (a->d[i] < b->d[i])
			return -1;
		if (a->d[i] > b->d[i])
			return 1;
	}
	return 0;
}

int hk_bn_from_be(HK_BN *a, const uint8_t *be, size_t len)
{
	size_t i;

	if (len > HK_BN_BYTES)
		return -1;

	hk_bn_zero(a);
	/* 大端字节串的最低位在最后，所以从末尾往前填 */
	for (i = 0; i < len; i++)
	{
		size_t byte_index = len - 1 - i; /* 从最低位开始数第 i 个字节 */
		uint32_t v = be[byte_index];

		a->d[i >> 2] |= v << ((i & 3) * 8);
	}
	return 0;
}

void hk_bn_to_be(const HK_BN *a, uint8_t *be, size_t len)
{
	size_t i;

	/* 先左补零，再按大端填 */
	memset(be, 0, len);
	for (i = 0; i < len && i < HK_BN_BYTES; i++)
	{
		size_t byte_index = len - 1 - i;
		uint32_t v = (a->d[i >> 2] >> ((i & 3) * 8)) & 0xff;

		be[byte_index] = (uint8_t)v;
	}
}

void hk_bn_sub_raw(HK_BN *a, const HK_BN *b)
{
	uint32_t borrow = 0;
	int i;

	for (i = 0; i < HK_BN_LIMBS; i++)
	{
		uint64_t sub = (uint64_t)b->d[i] + borrow;
		uint64_t cur = (uint64_t)a->d[i];

		a->d[i] = (uint32_t)(cur - sub);
		borrow = (cur < sub) ? 1u : 0u;
	}
}

void hk_bn_mod_double(HK_BN *a, const HK_BN *n)
{
	uint32_t carry = 0;
	int i;

	for (i = 0; i < HK_BN_LIMBS; i++)
	{
		uint32_t next = a->d[i] >> 31;

		a->d[i] = (a->d[i] << 1) | carry;
		carry = next;
	}

	/*
	 * 若左移溢出（carry=1），真实值是 A + 2^3072（A 是低 3072 位）。
	 * 因为 a < n < 2^3072，所以 2a < 2n，也就是说 2a - n 一定落在 [0, n)。
	 * 而 A - n 在模 2^3072 意义下恰好等于 2a - n（2a - n 本身就在 3072 位内），
	 * 所以这里不需要把 carry 特殊处理，直接减就行。
	 */
	if (carry || hk_bn_cmp(a, n) >= 0)
		hk_bn_sub_raw(a, n);
}

void hk_mont_init(HK_MONT *m, const HK_BN *n)
{
	uint32_t inv = 1;
	HK_BN one;
	int i;

	hk_bn_set(&m->n, n);

	/* Newton 迭代求 n^-1 mod 2^32（n 是奇数，从 1 出发 5 次即收敛到 32 位精度） */
	for (i = 0; i < 5; i++)
		inv *= 2u - n->d[0] * inv;
	m->n0inv = (uint32_t)(0u - inv); /* = -n^-1 mod 2^32 */

	/* R mod n = 2^3072 mod n：从 1 连续模加倍 3072 次 */
	hk_bn_from_u32(&one, 1);
	hk_bn_set(&m->rmod, &one);
	for (i = 0; i < HK_BN_LIMBS * 32; i++)
		hk_bn_mod_double(&m->rmod, n);

	/* R^2 mod n = (R mod n) 再模加倍 3072 次 */
	hk_bn_set(&m->r2, &m->rmod);
	for (i = 0; i < HK_BN_LIMBS * 32; i++)
		hk_bn_mod_double(&m->r2, n);
}

void hk_mont_mul(HK_BN *z, const HK_BN *x, const HK_BN *y, const HK_MONT *m)
{
	/* t 需要 s+2 个肢体：多出来的两个用于吸收进位 */
	uint32_t t[HK_BN_LIMBS + 2];
	uint32_t n0inv = m->n0inv;
	int i, j;

	memset(t, 0, sizeof(t));

	for (i = 0; i < HK_BN_LIMBS; i++)
	{
		uint32_t c = 0, mm;
		uint64_t uv;

		/* t = t + x[i] * y */
		for (j = 0; j < HK_BN_LIMBS; j++)
		{
			uv = (uint64_t)t[j] + (uint64_t)x->d[i] * y->d[j] + c;
			t[j] = (uint32_t)uv;
			c = (uint32_t)(uv >> 32);
		}
		uv = (uint64_t)t[HK_BN_LIMBS] + c;
		t[HK_BN_LIMBS] = (uint32_t)uv;
		t[HK_BN_LIMBS + 1] = (uint32_t)(uv >> 32);

		/* mm 使得 (t + mm*n) 的最低肢变成 0，从而可以右移 32 位 */
		mm = t[0] * n0inv;

		/* t = (t + mm * n) / 2^32 */
		uv = (uint64_t)t[0] + (uint64_t)mm * m->n.d[0];
		c = (uint32_t)(uv >> 32); /* 低 32 位必为 0，不用存 */
		for (j = 1; j < HK_BN_LIMBS; j++)
		{
			uv = (uint64_t)t[j] + (uint64_t)mm * m->n.d[j] + c;
			t[j - 1] = (uint32_t)uv;
			c = (uint32_t)(uv >> 32);
		}
		uv = (uint64_t)t[HK_BN_LIMBS] + c;
		t[HK_BN_LIMBS - 1] = (uint32_t)uv;
		t[HK_BN_LIMBS] = t[HK_BN_LIMBS + 1] + (uint32_t)(uv >> 32);
		t[HK_BN_LIMBS + 1] = 0;
	}

	{
		/*
		 * 此时结果在 t[0..s]，可能 >= n（最多比 n 大一位）。
		 * 先拷进 HK_BN 再判断 —— 不直接对 t 做类型转换，
		 * 免得踩到 alias 相关的未定义行为。
		 */
		HK_BN tmp;
		uint32_t borrow = 0;

		memcpy(tmp.d, t, sizeof(tmp.d));

		if (t[HK_BN_LIMBS] != 0 || hk_bn_cmp(&tmp, &m->n) >= 0)
		{
			for (j = 0; j < HK_BN_LIMBS; j++)
			{
				uint64_t sub = (uint64_t)m->n.d[j] + borrow;
				uint64_t cur = (uint64_t)tmp.d[j];

				tmp.d[j] = (uint32_t)(cur - sub);
				borrow = (cur < sub) ? 1u : 0u;
			}
		}
		hk_bn_set(z, &tmp);
	}

	memset(t, 0, sizeof(t));
}

void hk_bn_mod_mul(HK_BN *z, const HK_BN *x, const HK_BN *y, const HK_MONT *m)
{
	HK_BN t;

	/* mont_mul 给出 x*y*R^-1；再乘 R2 并做一次 mont_mul 就回到 x*y */
	hk_mont_mul(&t, x, y, m);
	hk_mont_mul(z, &t, &m->r2, m);
}

void hk_mont_exp(HK_BN *z, const HK_BN *base, const uint8_t *exp_be, size_t exp_len,
				 const HK_MONT *m)
{
	HK_BN bm, acc, one;
	size_t byte_i;
	int bit;

	/* 底数进 Montgomery 域：bm = base*R mod n = mont_mul(base, R^2) */
	hk_mont_mul(&bm, base, &m->r2, m);

	/* 累加器从「Montgomery 域里的 1」开始，即 R mod n */
	hk_bn_set(&acc, &m->rmod);

	/* 从指数最高位开始：平方 + 按需乘 */
	for (byte_i = 0; byte_i < exp_len; byte_i++)
	{
		uint8_t byte = exp_be[byte_i];

		for (bit = 7; bit >= 0; bit--)
		{
			hk_mont_mul(&acc, &acc, &acc, m);
			if ((byte >> bit) & 1)
				hk_mont_mul(&acc, &acc, &bm, m);
		}
	}

	/* 出 Montgomery 域：z = acc*R^-1 = mont_mul(acc, 1) */
	hk_bn_from_u32(&one, 1);
	hk_mont_mul(z, &acc, &one, m);

	memset(bm.d, 0, sizeof(bm.d));
	memset(acc.d, 0, sizeof(acc.d));
}

/* ---------------- 不做模约减的中间量大数（2048 位） ---------------- */

int hk_bnbig_from_be(HK_BN_BIG *a, const uint8_t *be, size_t len)
{
	size_t i;

	if (len > HK_BN_BIG_BYTES)
		return -1;

	memset(a->d, 0, sizeof(a->d));
	for (i = 0; i < len; i++)
	{
		uint32_t v = be[len - 1 - i];

		a->d[i >> 2] |= v << ((i & 3) * 8);
	}
	return 0;
}

void hk_bnbig_to_be(const HK_BN_BIG *a, uint8_t *be, size_t len)
{
	size_t i;

	memset(be, 0, len);
	for (i = 0; i < len && i < HK_BN_BIG_BYTES; i++)
	{
		uint32_t v = (a->d[i >> 2] >> ((i & 3) * 8)) & 0xff;

		be[len - 1 - i] = (uint8_t)v;
	}
}

void hk_bnbig_mul(HK_BN_BIG *z, const HK_BN_BIG *a, const HK_BN_BIG *b)
{
	HK_BN_BIG t;
	int i, j;

	memset(&t, 0, sizeof(t));

	for (i = 0; i < HK_BN_BIG_LIMBS; i++)
	{
		if (a->d[i] == 0)
			continue;
		for (j = 0; j + i < HK_BN_BIG_LIMBS; j++)
		{
			uint64_t uv = (uint64_t)a->d[i] * b->d[j];
			int k = i + j;
			uint64_t acc;
			uint64_t carry;

			if (uv == 0)
				continue;
			acc = (uint64_t)t.d[k] + (uint32_t)uv;
			carry = (uv >> 32) + (acc >> 32);
			t.d[k] = (uint32_t)acc;
			while (carry && k + 1 < HK_BN_BIG_LIMBS)
			{
				k++;
				acc = (uint64_t)t.d[k] + carry;
				t.d[k] = (uint32_t)acc;
				carry = acc >> 32;
			}
		}
	}

	memcpy(z->d, t.d, sizeof(z->d));
}

void hk_bnbig_add(HK_BN_BIG *z, const HK_BN_BIG *a, const HK_BN_BIG *b)
{
	uint64_t carry = 0;
	int i;

	for (i = 0; i < HK_BN_BIG_LIMBS; i++)
	{
		uint64_t acc = (uint64_t)a->d[i] + b->d[i] + carry;

		z->d[i] = (uint32_t)acc;
		carry = acc >> 32;
	}
}

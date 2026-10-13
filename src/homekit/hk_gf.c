/*
 * hk_gf.c —— GF(2^255-19) 域运算实现
 *
 * 这里的常数都是 RFC 7748 / RFC 8032 里给定的值，逐个字节核对过，
 * 写错任何一个的表现都是"自检向量不过"，不会静默。
 *
 * 两个必须无分支的地方：
 *   - hk_gf_cswap：交换与否等于密钥比特，有分支就漏私钥；
 *   - hk_ct_neq32：签名/签名的比较，提前 return 会漏信息。
 */
#include "hk_gf.h"

const hk_gf hk_gf0 = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
const hk_gf hk_gf1 = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

const hk_gf hk_gf121665 = {0xdb41, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

const hk_gf hk_gfD = {
	0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
	0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};

const hk_gf hk_gfD2 = {
	0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
	0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};

const hk_gf hk_gfX = {
	0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
	0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};

const hk_gf hk_gfY = {
	0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
	0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};

const hk_gf hk_gfI = {
	0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
	0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

void hk_gf_set(hk_gf r, const hk_gf a)
{
	int i;

	for (i = 0; i < 16; i++)
		r[i] = a[i];
}

/*
 * 进位传播。
 * 先给每肢加 2^16 再算术右移，是为了保证 c >= 1、(c-1) >= 0，
 * 从而避开"负数右移"这种实现定义行为。
 * 最高肢溢出按 2^256 ≡ 38 (mod p) 折回最低肢。
 */
void hk_gf_carry(hk_gf o)
{
	int i;

	for (i = 0; i < 16; i++)
	{
		int64_t c;

		o[i] += (int64_t)1 << 16;
		c = o[i] >> 16;
		o[i] -= c << 16;
		if (i < 15)
			o[i + 1] += c - 1;
		else
			o[0] += 38 * (c - 1);
	}
}

void hk_gf_cswap(hk_gf p, hk_gf q, int b)
{
	int64_t c = ~(int64_t)(b - 1); /* b=1 -> 全 1，b=0 -> 全 0 */
	int i;

	for (i = 0; i < 16; i++)
	{
		int64_t t = c & (p[i] ^ q[i]);

		p[i] ^= t;
		q[i] ^= t;
	}
}

void hk_gf_pack(uint8_t *o, const hk_gf n)
{
	int i, j, b;
	hk_gf m, t;

	hk_gf_set(t, n);
	hk_gf_carry(t);
	hk_gf_carry(t);
	hk_gf_carry(t);

	/* 两轮「减 p，借位则加回」，把结果压到 [0, p) */
	for (j = 0; j < 2; j++)
	{
		m[0] = t[0] - 0xffed;
		for (i = 1; i < 15; i++)
		{
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		b = (int)((m[15] >> 16) & 1);
		m[14] &= 0xffff;
		hk_gf_cswap(t, m, 1 - b);
	}

	for (i = 0; i < 16; i++)
	{
		o[2 * i] = (uint8_t)(t[i] & 0xff);
		o[2 * i + 1] = (uint8_t)(t[i] >> 8);
	}
}

void hk_gf_unpack(hk_gf o, const uint8_t *n)
{
	int i;

	for (i = 0; i < 16; i++)
		o[i] = (int64_t)n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
	o[15] &= 0x7fff; /* u 坐标/域元素都只有 255 位有效 */
}

void hk_gf_add(hk_gf o, const hk_gf a, const hk_gf b)
{
	int i;

	for (i = 0; i < 16; i++)
		o[i] = a[i] + b[i];
}

void hk_gf_sub(hk_gf o, const hk_gf a, const hk_gf b)
{
	int i;

	for (i = 0; i < 16; i++)
		o[i] = a[i] - b[i];
}

void hk_gf_mul(hk_gf o, const hk_gf a, const hk_gf b)
{
	int64_t t[31];
	int i, j;

	for (i = 0; i < 31; i++)
		t[i] = 0;
	for (i = 0; i < 16; i++)
		for (j = 0; j < 16; j++)
			t[i + j] += a[i] * b[j];

	/* x^16 ≡ 38 折回低位 */
	for (i = 0; i < 15; i++)
		t[i] += 38 * t[i + 16];
	for (i = 0; i < 16; i++)
		o[i] = t[i];

	hk_gf_carry(o);
	hk_gf_carry(o);
}

void hk_gf_sqr(hk_gf o, const hk_gf a)
{
	hk_gf_mul(o, a, a);
}

void hk_gf_inv(hk_gf o, const hk_gf i)
{
	hk_gf c;
	int a;

	hk_gf_set(c, i);
	/* RFC 7748 给定的固定平方-乘链：结果恰好是 i^(p-2)。
	 * 这里的 2 和 4 是链上的特例，别顺手改成别的数。 */
	for (a = 253; a >= 0; a--)
	{
		hk_gf_sqr(c, c);
		if (a != 2 && a != 4)
			hk_gf_mul(c, c, i);
	}
	hk_gf_set(o, c);
}

void hk_gf_pow2523(hk_gf o, const hk_gf i)
{
	hk_gf c;
	int a;

	hk_gf_set(c, i);
	/* o = i^(2^252-3)，Ed25519 解压点求平方根用 */
	for (a = 250; a >= 0; a--)
	{
		hk_gf_sqr(c, c);
		if (a != 1)
			hk_gf_mul(c, c, i);
	}
	hk_gf_set(o, c);
}

int hk_ct_neq32(const uint8_t *a, const uint8_t *b)
{
	uint32_t d = 0;
	int i;

	for (i = 0; i < 32; i++)
		d |= (uint32_t)(a[i] ^ b[i]);
	/* 折到 0/1，避免把差异的位数漏出去 */
	return (int)((d | (0u - d)) >> 31);
}

int hk_gf_neq(const hk_gf a, const hk_gf b)
{
	uint8_t c[32], d[32];

	hk_gf_pack(c, a);
	hk_gf_pack(d, b);
	return hk_ct_neq32(c, d);
}

int hk_gf_isneg(const hk_gf a)
{
	uint8_t d[32];

	hk_gf_pack(d, a);
	return d[0] & 1;
}

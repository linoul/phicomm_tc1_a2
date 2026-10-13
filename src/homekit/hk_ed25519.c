/*
 * hk_ed25519.c —— Ed25519（RFC 8032 实现）
 *
 * 结构：
 *   - 点用扩展坐标 (X:Y:Z:T) 表示，曲线方程 -x^2 + y^2 = 1 + d*x^2*y^2；
 *   - 域运算复用 hk_gf.c（和 X25519 同一份，省 flash）；
 *   - 标量规约用 modL（把 512 位的哈希值折到 mod L）。
 *
 * 为什么用扩展坐标而不是仿射：加法公式在扩展坐标下是"统一公式"，
 * 没有需要特判的例外点（除点本身），也就没有隐藏分支 —— 这在
 * 处理密钥相关数据时是必须的。
 */
#include "hk_ed25519.h"
#include "hk_gf.h"
#include "hk_sha512.h"

#include <string.h>

/* 群阶 L = 2^252 + 27742317777372353535851937790883648493 的小端字节 */
static const uint8_t HK_L[32] = {
	0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
	0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

/* 扩展坐标点：X, Y, Z, T */
typedef hk_gf hk_ge[4];

/* ------------------------- 点运算 ------------------------- */

static void point_set(hk_ge p, const hk_ge q)
{
	int i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 16; j++)
			p[i][j] = q[i][j];
}

static void point_cswap(hk_ge p, hk_ge q, int b)
{
	int i;

	for (i = 0; i < 4; i++)
		hk_gf_cswap(p[i], q[i], b);
}

/*
 * 统一加法（RFC 8032 §5.1.4 的"加"）。
 * 输入输出都允许是同一个点（p += q 里 q 也被读），所以每个 gf 都先算到临时量。
 */
static void point_add(hk_ge p, hk_ge q)
{
	hk_gf a, b, c, d, t, e, f, g, h;

	hk_gf_sub(a, p[1], p[0]);
	hk_gf_sub(t, q[1], q[0]);
	hk_gf_mul(a, a, t);

	hk_gf_add(b, p[0], p[1]);
	hk_gf_add(t, q[0], q[1]);
	hk_gf_mul(b, b, t);

	hk_gf_mul(c, p[3], q[3]);
	hk_gf_mul(c, c, hk_gfD2);

	hk_gf_mul(d, p[2], q[2]);
	hk_gf_add(d, d, d);

	hk_gf_sub(e, b, a);
	hk_gf_sub(f, d, c);
	hk_gf_add(g, d, c);
	hk_gf_add(h, b, a);

	hk_gf_mul(p[0], e, f);
	hk_gf_mul(p[1], h, g);
	hk_gf_mul(p[2], g, f);
	hk_gf_mul(p[3], e, h);
}

/* 序列化：y 的 32 字节小端，最高位塞 x 的最低位（RFC 8032 §5.1.2） */
static void point_pack(uint8_t *r, hk_ge p)
{
	hk_gf tx, ty, zi;

	hk_gf_inv(zi, p[2]);
	hk_gf_mul(tx, p[0], zi);
	hk_gf_mul(ty, p[1], zi);
	hk_gf_pack(r, ty);
	r[31] ^= (uint8_t)(hk_gf_isneg(tx) << 7);
}

/* p = s * q（s 是 32 字节标量，256 位全用） */
static void point_scalarmult(hk_ge p, hk_ge q, const uint8_t *s)
{
	int i;

	hk_gf_set(p[0], hk_gf0);
	hk_gf_set(p[1], hk_gf1);
	hk_gf_set(p[2], hk_gf1);
	hk_gf_set(p[3], hk_gf0);

	for (i = 255; i >= 0; --i)
	{
		uint8_t b = (uint8_t)((s[i / 8] >> (i & 7)) & 1);

		point_cswap(p, q, b);
		point_add(q, p);
		point_add(p, p);
		point_cswap(p, q, b);
	}
}

/* p = s * B（B 是基点） */
static void point_scalarbase(hk_ge p, const uint8_t *s)
{
	hk_ge q;

	hk_gf_set(q[0], hk_gfX);
	hk_gf_set(q[1], hk_gfY);
	hk_gf_set(q[2], hk_gf1);
	hk_gf_mul(q[3], hk_gfX, hk_gfY);
	point_scalarmult(p, q, s);
}

/*
 * 解压一个点，并取它的相反数（验签用 [-A]）。
 * 平方根不存在就说明不是一个合法的曲线点，直接拒绝。
 */
static int point_unpack_neg(hk_ge r, const uint8_t p[32])
{
	hk_gf t, chk, num, den, den2, den4, den6;

	hk_gf_set(r[2], hk_gf1);
	hk_gf_unpack(r[1], p);

	hk_gf_sqr(num, r[1]);
	hk_gf_mul(den, num, hk_gfD);
	hk_gf_sub(num, num, r[2]); /* num = y^2 - 1 */
	hk_gf_add(den, r[2], den); /* den = d*y^2 + 1 */

	hk_gf_sqr(den2, den);
	hk_gf_sqr(den4, den2);
	hk_gf_mul(den6, den4, den2);
	hk_gf_mul(t, den6, num);
	hk_gf_mul(t, t, den);

	hk_gf_pow2523(t, t); /* t = (num*den^7)^((p-5)/8) */
	hk_gf_mul(t, t, num);
	hk_gf_mul(t, t, den);
	hk_gf_mul(t, t, den);
	hk_gf_mul(r[0], t, den); /* 候选平方根 */

	hk_gf_sqr(chk, r[0]);
	hk_gf_mul(chk, chk, den);
	if (hk_gf_neq(chk, num))
		hk_gf_mul(r[0], r[0], hk_gfI); /* 乘 sqrt(-1) 修正 */

	hk_gf_sqr(chk, r[0]);
	hk_gf_mul(chk, chk, den);
	if (hk_gf_neq(chk, num))
		return -1; /* 不是曲线点 */

	/* 按序列化时塞进最高位的那一位选出正确的符号，然后取反 */
	if (hk_gf_isneg(r[0]) == (p[31] >> 7))
		hk_gf_sub(r[0], hk_gf0, r[0]);

	hk_gf_mul(r[3], r[0], r[1]);
	return 0;
}

/* --------------------- 标量规约 mod L --------------------- */

/*
 * 把 64 个字节（512 位）的哈希值规约成 mod L 的标量。
 * 做法：从高位向低位，每次用 L 的 16 倍把当前最高字节消掉，
 * 最后再统一做一次减 L 的修正。这是 RFC 8032 §5.1.6 的标准实现路线。
 */
static void modL(uint8_t *r, int64_t x[64])
{
	int64_t carry;
	int i, j;

	for (i = 63; i >= 32; --i)
	{
		carry = 0;
		for (j = i - 32; j < i - 12; ++j)
		{
			x[j] += carry - 16 * x[i] * (int64_t)HK_L[j - (i - 32)];
			carry = (x[j] + 128) >> 8;
			x[j] -= carry << 8;
		}
		x[j] += carry;
		x[i] = 0;
	}

	carry = 0;
	for (j = 0; j < 32; ++j)
	{
		x[j] += carry - (x[31] >> 4) * (int64_t)HK_L[j];
		carry = x[j] >> 8;
		x[j] &= 255;
	}
	for (j = 0; j < 32; ++j)
		x[j] -= carry * (int64_t)HK_L[j];

	for (i = 0; i < 32; ++i)
	{
		x[i + 1] += x[i] >> 8;
		r[i] = (uint8_t)(x[i] & 255);
	}
}

static void scalar_reduce(uint8_t *r)
{
	int64_t x[64];
	int i;

	for (i = 0; i < 64; i++)
		x[i] = (int64_t)(uint64_t)r[i];
	for (i = 0; i < 64; i++)
		r[i] = 0;
	modL(r, x);

	memset(x, 0, sizeof(x));
}

/* 判断 S 是否 < L（RFC 8032 要求，否则签名可锻造出多个变体） */
static int scalar_lt_L(const uint8_t s[32])
{
	int i;

	for (i = 31; i >= 0; i--)
	{
		if (s[i] < HK_L[i])
			return 1;
		if (s[i] > HK_L[i])
			return 0;
	}
	return 0; /* 相等也不行 */
}

/* ------------------------- 对外接口 ------------------------- */

/* 由种子展开出标量 a 与前缀（RFC 8032 §5.1.5） */
static void expand_seed(const uint8_t seed[32], uint8_t a_out[32], uint8_t prefix[32])
{
	uint8_t h[64];

	hk_sha512(seed, 32, h);
	memcpy(a_out, h, 32);
	a_out[0] &= 248;
	a_out[31] &= 127;
	a_out[31] |= 64;
	memcpy(prefix, h + 32, 32);
	memset(h, 0, sizeof(h));
}

int hk_ed25519_pubkey(uint8_t pk[HK_ED25519_PUB_LEN],
					  const uint8_t seed[HK_ED25519_SEED_LEN])
{
	uint8_t a[32], prefix[32];
	hk_ge p;

	expand_seed(seed, a, prefix);
	point_scalarbase(p, a);
	point_pack(pk, p);

	memset(a, 0, sizeof(a));
	memset(prefix, 0, sizeof(prefix));
	return 0;
}

int hk_ed25519_sign(uint8_t sig[HK_ED25519_SIG_LEN],
					const uint8_t *msg, size_t msg_len,
					const uint8_t seed[HK_ED25519_SEED_LEN],
					const uint8_t pk[HK_ED25519_PUB_LEN])
{
	uint8_t a[32], prefix[32];
	uint8_t r[64], k[64];
	int64_t x[64];
	hk_ge p;
	HK_SHA512_CTX ctx;
	int i, j;

	expand_seed(seed, a, prefix);

	/* r = SHA512(prefix || M) mod L，R = r*B */
	hk_sha512_init(&ctx);
	hk_sha512_update(&ctx, prefix, 32);
	if (msg_len)
		hk_sha512_update(&ctx, msg, msg_len);
	hk_sha512_final(&ctx, r);
	scalar_reduce(r);
	point_scalarbase(p, r);
	point_pack(sig, p);

	/* k = SHA512(R || A || M) mod L */
	hk_sha512_init(&ctx);
	hk_sha512_update(&ctx, sig, 32);
	hk_sha512_update(&ctx, pk, 32);
	if (msg_len)
		hk_sha512_update(&ctx, msg, msg_len);
	hk_sha512_final(&ctx, k);
	scalar_reduce(k);

	/* S = (r + k*a) mod L */
	for (i = 0; i < 64; i++)
		x[i] = 0;
	for (i = 0; i < 32; i++)
		x[i] = (int64_t)(uint64_t)r[i];
	for (i = 0; i < 32; i++)
		for (j = 0; j < 32; j++)
			x[i + j] += (int64_t)k[i] * (int64_t)a[j];
	modL(sig + 32, x);

	/* 私钥与中间态清零 */
	memset(a, 0, sizeof(a));
	memset(prefix, 0, sizeof(prefix));
	memset(r, 0, sizeof(r));
	memset(k, 0, sizeof(k));
	memset(x, 0, sizeof(x));
	return 0;
}

int hk_ed25519_verify(const uint8_t sig[HK_ED25519_SIG_LEN],
					  const uint8_t *msg, size_t msg_len,
					  const uint8_t pk[HK_ED25519_PUB_LEN])
{
	uint8_t k[64], t[32];
	hk_ge p, q;
	HK_SHA512_CTX ctx;

	/* RFC 8032 §5.1.7：S 必须先验范围，否则同一签名会有多个编码 */
	if (!scalar_lt_L(sig + 32))
		return -1;

	if (point_unpack_neg(q, pk))
		return -1;

	hk_sha512_init(&ctx);
	hk_sha512_update(&ctx, sig, 32);
	hk_sha512_update(&ctx, pk, 32);
	if (msg_len)
		hk_sha512_update(&ctx, msg, msg_len);
	hk_sha512_final(&ctx, k);
	scalar_reduce(k);

	/* 检查 [S]B == R + [k]A，等价于 [S]B - [k]A == R，
	 * 而 q 存的已经是 [-A]，所以这里做加法。 */
	point_scalarmult(p, q, k);   /* p = [k](-A) */
	point_scalarbase(q, sig + 32); /* q = [S]B */
	point_add(p, q);			 /* p = [S]B - [k]A */
	point_pack(t, p);

	memset(k, 0, sizeof(k));
	if (hk_ct_neq32(sig, t))
		return -1;

	return 0;
}

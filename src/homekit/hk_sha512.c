/*
 * hk_sha512.c —— SHA-512（FIPS 180-4 实现）
 *
 * 常数来自 FIPS 180-4：
 *   K[80]  = 前 80 个素数立方根小数部分的高 64 位
 *   IV[8]  = 前 8 个素数平方根小数部分的高 64 位
 * 只要一个数写错，本机自测的 NIST 向量就会失败，所以不做"看起来对"的假设。
 */
#include "hk_sha512.h"
#include <string.h>

static const uint64_t HK_K[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

static const uint64_t HK_IV[8] = {
	0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
	0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
	0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
	0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
/* 注意：宏参数一定要加括号，否则 ROR64(a^b, 1) 会算错 */
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROR64((x), 28) ^ ROR64((x), 34) ^ ROR64((x), 39))
#define BSIG1(x) (ROR64((x), 14) ^ ROR64((x), 18) ^ ROR64((x), 41))
#define SSIG0(x) (ROR64((x), 1) ^ ROR64((x), 8) ^ ((x) >> 7))
#define SSIG1(x) (ROR64((x), 19) ^ ROR64((x), 61) ^ ((x) >> 6))

/* 大端读取，不假设主机字节序 */
static uint64_t rbe64(const uint8_t *p)
{
	return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
		   ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
		   ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
		   ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static void wbe64(uint8_t *p, uint64_t v)
{
	p[0] = (uint8_t)(v >> 56);
	p[1] = (uint8_t)(v >> 48);
	p[2] = (uint8_t)(v >> 40);
	p[3] = (uint8_t)(v >> 32);
	p[4] = (uint8_t)(v >> 24);
	p[5] = (uint8_t)(v >> 16);
	p[6] = (uint8_t)(v >> 8);
	p[7] = (uint8_t)v;
}

/* 处理一块 128 字节 */
static void hk_sha512_block(HK_SHA512_CTX *ctx, const uint8_t *p)
{
	uint64_t w[80];
	uint64_t a, b, c, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = rbe64(p + i * 8);
	for (i = 16; i < 80; i++)
		w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

	a = ctx->h[0];
	b = ctx->h[1];
	c = ctx->h[2];
	d = ctx->h[3];
	e = ctx->h[4];
	f = ctx->h[5];
	g = ctx->h[6];
	h = ctx->h[7];

	for (i = 0; i < 80; i++)
	{
		uint64_t t1 = h + BSIG1(e) + CH(e, f, g) + HK_K[i] + w[i];
		uint64_t t2 = BSIG0(a) + MAJ(a, b, c);

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->h[0] += a;
	ctx->h[1] += b;
	ctx->h[2] += c;
	ctx->h[3] += d;
	ctx->h[4] += e;
	ctx->h[5] += f;
	ctx->h[6] += g;
	ctx->h[7] += h;
}

void hk_sha512_init(HK_SHA512_CTX *ctx)
{
	ctx->total = 0;
	ctx->n = 0;
	memcpy(ctx->h, HK_IV, sizeof(HK_IV));
}

void hk_sha512_update(HK_SHA512_CTX *ctx, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;

	ctx->total += (uint64_t)len;

	/* 先把上次剩下的尾巴补满 */
	if (ctx->n)
	{
		size_t need = HK_SHA512_BLOCK_LEN - ctx->n;

		if (len < need)
		{
			memcpy(ctx->buf + ctx->n, p, len);
			ctx->n += (uint32_t)len;
			return;
		}
		memcpy(ctx->buf + ctx->n, p, need);
		hk_sha512_block(ctx, ctx->buf);
		ctx->n = 0;
		p += need;
		len -= need;
	}

	/* 整块直接处理，尽量避开逐字节拷贝 */
	while (len >= HK_SHA512_BLOCK_LEN)
	{
		hk_sha512_block(ctx, p);
		p += HK_SHA512_BLOCK_LEN;
		len -= HK_SHA512_BLOCK_LEN;
	}

	if (len)
	{
		memcpy(ctx->buf, p, len);
		ctx->n = (uint32_t)len;
	}
}

void hk_sha512_final(HK_SHA512_CTX *ctx, uint8_t out[HK_SHA512_DIGEST_LEN])
{
	/* SHA-512 的长度字段是 128 位大端「比特数」。
	 * 我们的输入远小于 2^61 字节，所以高 64 位只有 total>>61 这一段。 */
	uint64_t bits_hi = ctx->total >> 61;
	uint64_t bits_lo = ctx->total << 3;
	uint8_t blk[HK_SHA512_BLOCK_LEN];
	uint32_t n = ctx->n;
	int i;

	/* 手工拼最后一块：message 尾巴 + 0x80 + 补零 + 长度。
	 * 不走 update()，是为了不让长度字段本身被算进 total。 */
	memcpy(blk, ctx->buf, n);
	blk[n++] = 0x80;

	if (n > 112)
	{
		/* 这一块塞不下长度字段，先把它处理掉，长度字段放到下一块 */
		memset(blk + n, 0, HK_SHA512_BLOCK_LEN - n);
		hk_sha512_block(ctx, blk);
		n = 0;
	}

	memset(blk + n, 0, 112 - n);
	wbe64(blk + 112, bits_hi);
	wbe64(blk + 120, bits_lo);
	hk_sha512_block(ctx, blk);

	for (i = 0; i < 8; i++)
		wbe64(out + i * 8, ctx->h[i]);

	/* 摘要里可能含敏感中间态（SRP/HMAC 会用到），用完擦掉 */
	memset(ctx, 0, sizeof(*ctx));
}

void hk_sha512(const void *data, size_t len, uint8_t out[HK_SHA512_DIGEST_LEN])
{
	HK_SHA512_CTX ctx;

	hk_sha512_init(&ctx);
	hk_sha512_update(&ctx, data, len);
	hk_sha512_final(&ctx, out);
}

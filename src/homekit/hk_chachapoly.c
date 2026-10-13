/*
 * hk_chachapoly.c —— ChaCha20 + Poly1305 + AEAD（RFC 8439 实现）
 *
 * Poly1305 用 5 个 26 位肢体（limb）做运算：
 * 32 位机上没有 128 位乘，26 位 × 26 位 = 52 位，正好落在 uint64_t 里，
 * 所以这是 32 位 MCU 上的标准做法（poly1305-donna-32 的思路）。
 *
 * 与安全相关的两点特意写清楚：
 *   - 解密时 tag 用"全量异或后判零"的方式比较，不提前 return，避免时序泄露；
 *   - 所有中间态（r/h/pad、一次性密钥）在收尾时清零，别把密钥留在栈上。
 */
#include "hk_chachapoly.h"
#include <string.h>

/* ---------- 小端读写（不假设主机字节序） ---------- */

static uint32_t ld32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void st32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t rotl32(uint32_t v, int c)
{
	return (v << c) | (v >> (32 - c));
}

#define QR(a, b, c, d)          \
	do                          \
	{                           \
		a += b;                 \
		d ^= a;                 \
		d = rotl32(d, 16);      \
		c += d;                 \
		b ^= c;                 \
		b = rotl32(b, 12);      \
		a += b;                 \
		d ^= a;                 \
		d = rotl32(d, 8);       \
		c += d;                 \
		b ^= c;                 \
		b = rotl32(b, 7);       \
	} while (0)

void hk_chacha20_block(const uint8_t key[HK_CHACHA20_KEY_LEN],
					   const uint8_t nonce[HK_CHACHA20_NONCE_LEN],
					   uint32_t counter,
					   uint8_t out[64])
{
	/* "expand 32-byte k" 拆成 4 个小端字 */
	uint32_t st[16];
	uint32_t w[16];
	int i;

	st[0] = 0x61707865u; /* "expa" */
	st[1] = 0x3320646eu; /* "nd 3" */
	st[2] = 0x79622d32u; /* "2-by" */
	st[3] = 0x6b206574u; /* "te k" */

	for (i = 0; i < 8; i++)
		st[4 + i] = ld32le(key + i * 4);

	st[12] = counter;
	st[13] = ld32le(nonce + 0);
	st[14] = ld32le(nonce + 4);
	st[15] = ld32le(nonce + 8);

	memcpy(w, st, sizeof(w));

	/* 20 轮 = 10 次「列轮 + 对角轮」 */
	for (i = 0; i < 10; i++)
	{
		/* 列 */
		QR(w[0], w[4], w[8], w[12]);
		QR(w[1], w[5], w[9], w[13]);
		QR(w[2], w[6], w[10], w[14]);
		QR(w[3], w[7], w[11], w[15]);
		/* 对角 */
		QR(w[0], w[5], w[10], w[15]);
		QR(w[1], w[6], w[11], w[12]);
		QR(w[2], w[7], w[8], w[13]);
		QR(w[3], w[4], w[9], w[14]);
	}

	for (i = 0; i < 16; i++)
		st32le(out + i * 4, w[i] + st[i]);

	memset(w, 0, sizeof(w));
	memset(st, 0, sizeof(st));
}

void hk_chacha20_xor(const uint8_t key[HK_CHACHA20_KEY_LEN],
					 const uint8_t nonce[HK_CHACHA20_NONCE_LEN],
					 uint32_t counter,
					 uint8_t *data, size_t len)
{
	uint8_t ks[64];
	size_t off = 0;

	while (off < len)
	{
		size_t i;
		size_t chunk = len - off;

		if (chunk > sizeof(ks))
			chunk = sizeof(ks);

		hk_chacha20_block(key, nonce, counter, ks);
		for (i = 0; i < chunk; i++)
			data[off + i] ^= ks[i];

		off += chunk;
		counter++;
	}

	memset(ks, 0, sizeof(ks));
}

/* ---------------------------- Poly1305 ---------------------------- */

void hk_poly1305_init(HK_POLY1305_CTX *ctx, const uint8_t key[32])
{
	uint32_t t0 = ld32le(key + 0);
	uint32_t t1 = ld32le(key + 4);
	uint32_t t2 = ld32le(key + 8);
	uint32_t t3 = ld32le(key + 12);

	/*
	 * r 必须先 clamp（RFC 8439 第 2.5 节）：砍掉高 4 位、以及每 32 位块的
	 * 第 2/3/4 位。用 26 位肢体表示后，掩码就是这个样子。
	 */
	ctx->r[0] = (t0) & 0x3ffffffu;
	ctx->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03u;
	ctx->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ffu;
	ctx->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fffu;
	ctx->r[4] = ((t3 >> 8)) & 0x00fffffu;

	ctx->pad[0] = ld32le(key + 16);
	ctx->pad[1] = ld32le(key + 20);
	ctx->pad[2] = ld32le(key + 24);
	ctx->pad[3] = ld32le(key + 28);

	ctx->h[0] = 0;
	ctx->h[1] = 0;
	ctx->h[2] = 0;
	ctx->h[3] = 0;
	ctx->h[4] = 0;
	ctx->leftover = 0;
	memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

/*
 * 处理整块数据。hibit 表示「这一块后面要不要补上 2^128」：
 *   完整 16 字节块 → 补 1（即 2^128）
 *   末尾的残块     → 不补（长度信息只靠补的那个 1 字节表达）
 */
static void poly1305_blocks(HK_POLY1305_CTX *ctx, const uint8_t *m, size_t bytes, uint32_t hibit)
{
	uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2], r3 = ctx->r[3], r4 = ctx->r[4];
	uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
	uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];

	while (bytes >= 16)
	{
		uint64_t d0, d1, d2, d3, d4;
		uint32_t c;

		/* h += m[i]（26 位一段地从字节流里取） */
		h0 += (ld32le(m + 0)) & 0x3ffffffu;
		h1 += (ld32le(m + 3) >> 2) & 0x3ffffffu;
		h2 += (ld32le(m + 6) >> 4) & 0x3ffffffu;
		h3 += (ld32le(m + 9) >> 6) & 0x3ffffffu;
		h4 += (ld32le(m + 12) >> 8) | hibit;

		/* h *= r（模 2^130-5 的乘法用「×5 折叠」实现） */
		d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
		d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
		d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
		d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
		d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

		/* 部分规约：把进位一路推到最高肢，再折回最低肢 */
		c = (uint32_t)(d0 >> 26);
		h0 = (uint32_t)d0 & 0x3ffffffu;
		d1 += c;
		c = (uint32_t)(d1 >> 26);
		h1 = (uint32_t)d1 & 0x3ffffffu;
		d2 += c;
		c = (uint32_t)(d2 >> 26);
		h2 = (uint32_t)d2 & 0x3ffffffu;
		d3 += c;
		c = (uint32_t)(d3 >> 26);
		h3 = (uint32_t)d3 & 0x3ffffffu;
		d4 += c;
		c = (uint32_t)(d4 >> 26);
		h4 = (uint32_t)d4 & 0x3ffffffu;
		h0 += c * 5;
		c = h0 >> 26;
		h0 &= 0x3ffffffu;
		h1 += c;

		m += 16;
		bytes -= 16;
	}

	ctx->h[0] = h0;
	ctx->h[1] = h1;
	ctx->h[2] = h2;
	ctx->h[3] = h3;
	ctx->h[4] = h4;
}

void hk_poly1305_update(HK_POLY1305_CTX *ctx, const uint8_t *m, size_t len)
{
	if (ctx->leftover)
	{
		size_t want = 16 - ctx->leftover;

		if (want > len)
			want = len;
		memcpy(ctx->buffer + ctx->leftover, m, want);
		ctx->leftover += (uint32_t)want;
		m += want;
		len -= want;
		if (ctx->leftover == 16)
		{
			poly1305_blocks(ctx, ctx->buffer, 16, 1u << 24);
			ctx->leftover = 0;
		}
	}

	if (len >= 16)
	{
		size_t n = len & ~(size_t)15;

		poly1305_blocks(ctx, m, n, 1u << 24);
		m += n;
		len -= n;
	}

	if (len)
	{
		memcpy(ctx->buffer, m, len);
		ctx->leftover = (uint32_t)len;
	}
}

void hk_poly1305_final(HK_POLY1305_CTX *ctx, uint8_t tag[HK_POLY1305_TAG_LEN])
{
	uint32_t h0, h1, h2, h3, h4, c;
	uint32_t g0, g1, g2, g3, g4;
	uint32_t mask;
	uint64_t f;

	/* 处理残块：末尾补一个 1 字节（这是「消息长度编码」的一部分） */
	if (ctx->leftover)
	{
		size_t i = ctx->leftover;

		ctx->buffer[i++] = 1;
		for (; i < 16; i++)
			ctx->buffer[i] = 0;
		poly1305_blocks(ctx, ctx->buffer, 16, 0);
	}

	h0 = ctx->h[0];
	h1 = ctx->h[1];
	h2 = ctx->h[2];
	h3 = ctx->h[3];
	h4 = ctx->h[4];

	/* 完全进位 */
	c = h1 >> 26;
	h1 &= 0x3ffffffu;
	h2 += c;
	c = h2 >> 26;
	h2 &= 0x3ffffffu;
	h3 += c;
	c = h3 >> 26;
	h3 &= 0x3ffffffu;
	h4 += c;
	c = h4 >> 26;
	h4 &= 0x3ffffffu;
	h0 += c * 5;
	c = h0 >> 26;
	h0 &= 0x3ffffffu;
	h1 += c;

	/* 算 h + (-p)，用掩码二选一，避免分支（时间侧信道） */
	g0 = h0 + 5;
	c = g0 >> 26;
	g0 &= 0x3ffffffu;
	g1 = h1 + c;
	c = g1 >> 26;
	g1 &= 0x3ffffffu;
	g2 = h2 + c;
	c = g2 >> 26;
	g2 &= 0x3ffffffu;
	g3 = h3 + c;
	c = g3 >> 26;
	g3 &= 0x3ffffffu;
	g4 = h4 + c - (1u << 26);

	mask = (g4 >> 31) - 1; /* g4 借位了 → 0xffffffff，否则 0 */
	g0 &= mask;
	g1 &= mask;
	g2 &= mask;
	g3 &= mask;
	g4 &= mask;
	mask = ~mask;
	h0 = (h0 & mask) | g0;
	h1 = (h1 & mask) | g1;
	h2 = (h2 & mask) | g2;
	h3 = (h3 & mask) | g3;
	h4 = (h4 & mask) | g4;

	/* h %= 2^128，压回 4 个 32 位字 */
	h0 = ((h0) | (h1 << 26));
	h1 = ((h1 >> 6) | (h2 << 20));
	h2 = ((h2 >> 12) | (h3 << 14));
	h3 = ((h3 >> 18) | (h4 << 8));

	/* tag = (h + pad) mod 2^128 */
	f = (uint64_t)h0 + ctx->pad[0];
	h0 = (uint32_t)f;
	f = (uint64_t)h1 + ctx->pad[1] + (f >> 32);
	h1 = (uint32_t)f;
	f = (uint64_t)h2 + ctx->pad[2] + (f >> 32);
	h2 = (uint32_t)f;
	f = (uint64_t)h3 + ctx->pad[3] + (f >> 32);
	h3 = (uint32_t)f;

	st32le(tag + 0, h0);
	st32le(tag + 4, h1);
	st32le(tag + 8, h2);
	st32le(tag + 12, h3);

	memset(ctx, 0, sizeof(*ctx));
}

void hk_poly1305(uint8_t tag[HK_POLY1305_TAG_LEN], const uint8_t key[32],
				 const uint8_t *m, size_t len)
{
	HK_POLY1305_CTX ctx;

	hk_poly1305_init(&ctx, key);
	hk_poly1305_update(&ctx, m, len);
	hk_poly1305_final(&ctx, tag);
}

/* ------------------------------ AEAD ------------------------------ */

/* 把 AAD / 密文各自补齐到 16 字节边界，长度用 64 位小端追加 */
static void poly1305_pad16(HK_POLY1305_CTX *ctx, size_t len)
{
	static const uint8_t zero[16] = {0};
	size_t rem = len & 15u;

	if (rem)
		hk_poly1305_update(ctx, zero, 16 - rem);
}

static void poly1305_le64(HK_POLY1305_CTX *ctx, uint64_t v)
{
	uint8_t b[8];

	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
	b[4] = (uint8_t)(v >> 32);
	b[5] = (uint8_t)(v >> 40);
	b[6] = (uint8_t)(v >> 48);
	b[7] = (uint8_t)(v >> 56);
	hk_poly1305_update(ctx, b, 8);
}

static void aead_tag(uint8_t tag[HK_POLY1305_TAG_LEN],
					 const uint8_t key[HK_CHACHA20_KEY_LEN],
					 const uint8_t nonce[HK_CHACHA20_NONCE_LEN],
					 const uint8_t *aad, size_t aad_len,
					 const uint8_t *ct, size_t ct_len)
{
	uint8_t otk[64];
	HK_POLY1305_CTX ctx;

	/* 一次性密钥 = counter=0 那个块的前 32 字节 */
	hk_chacha20_block(key, nonce, 0, otk);
	hk_poly1305_init(&ctx, otk);
	if (aad_len)
	{
		hk_poly1305_update(&ctx, aad, aad_len);
		poly1305_pad16(&ctx, aad_len);
	}
	if (ct_len)
	{
		hk_poly1305_update(&ctx, ct, ct_len);
		poly1305_pad16(&ctx, ct_len);
	}
	poly1305_le64(&ctx, (uint64_t)aad_len);
	poly1305_le64(&ctx, (uint64_t)ct_len);
	hk_poly1305_final(&ctx, tag);

	memset(otk, 0, sizeof(otk));
}

int hk_aead_encrypt(uint8_t *ct, uint8_t tag[HK_POLY1305_TAG_LEN],
					const uint8_t key[HK_CHACHA20_KEY_LEN],
					const uint8_t nonce[HK_CHACHA20_NONCE_LEN],
					const uint8_t *aad, size_t aad_len,
					const uint8_t *pt, size_t pt_len)
{
	/* 明文拷进密文缓冲后原地加密；这样 ct 和 pt 可以指向同一块内存 */
	if (pt_len)
	{
		if (pt != ct)
			memmove(ct, pt, pt_len);
		hk_chacha20_xor(key, nonce, 1, ct, pt_len);
	}

	aead_tag(tag, key, nonce, aad, aad_len, ct, pt_len);
	return 0;
}

int hk_aead_decrypt(uint8_t *pt, const uint8_t tag[HK_POLY1305_TAG_LEN],
					const uint8_t key[HK_CHACHA20_KEY_LEN],
					const uint8_t nonce[HK_CHACHA20_NONCE_LEN],
					const uint8_t *aad, size_t aad_len,
					const uint8_t *ct, size_t ct_len)
{
	uint8_t want[HK_POLY1305_TAG_LEN];
	uint8_t diff = 0;
	int i;

	aead_tag(want, key, nonce, aad, aad_len, ct, ct_len);

	/* 常数时间比较：先算完再判，不要提前 return */
	for (i = 0; i < HK_POLY1305_TAG_LEN; i++)
		diff |= (uint8_t)(want[i] ^ tag[i]);

	if (diff != 0)
	{
		/* 认证失败：明文一个字节都不吐出去 */
		memset(want, 0, sizeof(want));
		if (ct_len)
			memset(pt, 0, ct_len);
		return -1;
	}

	if (ct_len)
	{
		if (ct != pt)
			memmove(pt, ct, ct_len);
		hk_chacha20_xor(key, nonce, 1, pt, ct_len);
	}

	memset(want, 0, sizeof(want));
	return 0;
}

/*
 * hk_kdf.c —— HMAC-SHA512 + HKDF-SHA512
 *
 * 实现刻意保持"教科书式"，因为这两个东西写错的表现是：
 * 配对时 iOS 直接报"无法添加配件"，看不到任何中间值，极难排查。
 * 所以宁可直白，也要能被 RFC 向量钉死。
 */
#include "hk_kdf.h"
#include <string.h>

#define HK_IPAD 0x36
#define HK_OPAD 0x5c

void hk_hmac_sha512_init(HK_HMAC_SHA512_CTX *ctx, const uint8_t *key, size_t key_len)
{
	uint8_t k[HK_HMAC_SHA512_BLOCK];
	uint8_t pad[HK_HMAC_SHA512_BLOCK];
	size_t i;

	memset(k, 0, sizeof(k));

	/* 超长密钥先哈希成 64 字节（RFC 2104 第 2 节） */
	if (key_len > HK_HMAC_SHA512_BLOCK)
	{
		hk_sha512(key, key_len, k);
	}
	else if (key_len)
	{
		memcpy(k, key, key_len);
	}

	for (i = 0; i < HK_HMAC_SHA512_BLOCK; i++)
		pad[i] = (uint8_t)(k[i] ^ HK_IPAD);
	hk_sha512_init(&ctx->ictx);
	hk_sha512_update(&ctx->ictx, pad, sizeof(pad));

	for (i = 0; i < HK_HMAC_SHA512_BLOCK; i++)
		pad[i] = (uint8_t)(k[i] ^ HK_OPAD);
	hk_sha512_init(&ctx->octx);
	hk_sha512_update(&ctx->octx, pad, sizeof(pad));

	/* 密钥与其派生 pad 都是敏感数据 */
	memset(k, 0, sizeof(k));
	memset(pad, 0, sizeof(pad));
}

void hk_hmac_sha512_update(HK_HMAC_SHA512_CTX *ctx, const void *data, size_t len)
{
	hk_sha512_update(&ctx->ictx, data, len);
}

void hk_hmac_sha512_final(HK_HMAC_SHA512_CTX *ctx, uint8_t out[HK_HMAC_SHA512_LEN])
{
	uint8_t inner[HK_SHA512_DIGEST_LEN];

	hk_sha512_final(&ctx->ictx, inner);
	hk_sha512_update(&ctx->octx, inner, sizeof(inner));
	hk_sha512_final(&ctx->octx, out);

	memset(inner, 0, sizeof(inner));
	memset(ctx, 0, sizeof(*ctx));
}

void hk_hmac_sha512(const uint8_t *key, size_t key_len,
					const void *data, size_t len,
					uint8_t out[HK_HMAC_SHA512_LEN])
{
	HK_HMAC_SHA512_CTX ctx;

	hk_hmac_sha512_init(&ctx, key, key_len);
	hk_hmac_sha512_update(&ctx, data, len);
	hk_hmac_sha512_final(&ctx, out);
}

int hk_hkdf_sha512(const uint8_t *salt, size_t salt_len,
				   const uint8_t *ikm, size_t ikm_len,
				   const uint8_t *info, size_t info_len,
				   uint8_t *okm, size_t okm_len)
{
	uint8_t prk[HK_HMAC_SHA512_LEN];
	uint8_t t[HK_HMAC_SHA512_LEN];
	uint8_t zero_salt[HK_HMAC_SHA512_LEN];
	size_t done = 0;
	uint8_t counter = 1;
	size_t tlen = 0;

	if (okm_len > 255u * HK_HMAC_SHA512_LEN)
		return -1;

	/* --- Extract --- */
	if (salt == NULL || salt_len == 0)
	{
		memset(zero_salt, 0, sizeof(zero_salt));
		hk_hmac_sha512(zero_salt, sizeof(zero_salt), ikm, ikm_len, prk);
	}
	else
	{
		hk_hmac_sha512(salt, salt_len, ikm, ikm_len, prk);
	}

	/* --- Expand --- */
	while (done < okm_len)
	{
		HK_HMAC_SHA512_CTX ctx;
		size_t chunk;

		hk_hmac_sha512_init(&ctx, prk, sizeof(prk));
		if (tlen)
			hk_hmac_sha512_update(&ctx, t, tlen);
		if (info_len)
			hk_hmac_sha512_update(&ctx, info, info_len);
		hk_hmac_sha512_update(&ctx, &counter, 1);
		hk_hmac_sha512_final(&ctx, t);
		tlen = sizeof(t);

		chunk = okm_len - done;
		if (chunk > sizeof(t))
			chunk = sizeof(t);
		memcpy(okm + done, t, chunk);
		done += chunk;
		counter++;
	}

	memset(prk, 0, sizeof(prk));
	memset(t, 0, sizeof(t));
	return 0;
}

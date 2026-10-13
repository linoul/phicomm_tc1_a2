/*
 * hk_kdf.h —— HMAC-SHA512 与 HKDF-SHA512（RFC 2104 / RFC 5869）
 *
 * HomeKit 里这两个到处都在用：
 *   - SRP-6a 的 x/k 计算要 HMAC-SHA512
 *   - Pair Verify 的会话密钥派生要用 HKDF-SHA512（HAP 规定用 SHA-512 变体，不是 SHA-256）
 *   - Ed25519 内部要 SHA-512（直接用 hk_sha512_*）
 */
#ifndef HK_KDF_H
#define HK_KDF_H

#include "hk_sha512.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define HK_HMAC_SHA512_LEN 64
#define HK_HMAC_SHA512_BLOCK 128

typedef struct _HK_HMAC_SHA512_CTX
{
	HK_SHA512_CTX ictx; /* H(K^ipad | m) */
	HK_SHA512_CTX octx; /* H(K^opad | ...) */
} HK_HMAC_SHA512_CTX;

void hk_hmac_sha512_init(HK_HMAC_SHA512_CTX *ctx, const uint8_t *key, size_t key_len);
void hk_hmac_sha512_update(HK_HMAC_SHA512_CTX *ctx, const void *data, size_t len);
void hk_hmac_sha512_final(HK_HMAC_SHA512_CTX *ctx, uint8_t out[HK_HMAC_SHA512_LEN]);
void hk_hmac_sha512(const uint8_t *key, size_t key_len,
					const void *data, size_t len,
					uint8_t out[HK_HMAC_SHA512_LEN]);

/*
 * HKDF-SHA512。
 * salt 传 NULL 或 salt_len=0 时按 RFC 5869 用全零盐（等价于 hashlen 个 0 字节）。
 * info 传 NULL 表示无上下文串。
 * 返回 0 成功，-1 表示 okm_len 超出上限（255*64）。
 */
int hk_hkdf_sha512(const uint8_t *salt, size_t salt_len,
				   const uint8_t *ikm, size_t ikm_len,
				   const uint8_t *info, size_t info_len,
				   uint8_t *okm, size_t okm_len);

#ifdef __cplusplus
}
#endif

#endif /* HK_KDF_H */

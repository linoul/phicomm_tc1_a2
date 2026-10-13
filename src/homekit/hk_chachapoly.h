/*
 * hk_chachapoly.h —— ChaCha20-Poly1305 AEAD（RFC 8439）
 *
 * HomeKit 用它做两件事：
 *   1) 配对阶段：加解密 Pair Setup / Pair Verify 里的 TLV（都用同一套 AEAD）；
 *   2) 配对之后：每个 HAP 会话帧（"HTTP" 请求/响应）都用会话密钥加密+认证。
 *
 * 为什么不用 AES-CCM（HAP 也允许）：M4 上 ChaCha20 是纯 32 位加减/异或/移位，
 * 没有查表、没有 cache 侧信道问题，也不吃 AES 硬件（那部分要被 WiFi/WPA 用）。
 */
#ifndef HK_CHACHAPOLY_H
#define HK_CHACHAPOLY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HK_CHACHA20_KEY_LEN 32
#define HK_CHACHA20_NONCE_LEN 12
#define HK_POLY1305_TAG_LEN 16

typedef struct _HK_POLY1305_CTX
{
	uint32_t r[5];
	uint32_t h[5];
	uint32_t pad[4];
	uint32_t leftover;
	uint8_t buffer[16];
} HK_POLY1305_CTX;

/* 生成一个 64 字节 keystream 块（counter 是块序号） */
void hk_chacha20_block(const uint8_t key[HK_CHACHA20_KEY_LEN],
					   const uint8_t nonce[HK_CHACHA20_NONCE_LEN],
					   uint32_t counter,
					   uint8_t out[64]);

/* 原地加解密（ChaCha20 是对称流，加解密同一个函数） */
void hk_chacha20_xor(const uint8_t key[HK_CHACHA20_KEY_LEN],
					 const uint8_t nonce[HK_CHACHA20_NONCE_LEN],
					 uint32_t counter,
					 uint8_t *data, size_t len);

/* Poly1305 MAC */
void hk_poly1305_init(HK_POLY1305_CTX *ctx, const uint8_t key[32]);
void hk_poly1305_update(HK_POLY1305_CTX *ctx, const uint8_t *m, size_t len);
void hk_poly1305_final(HK_POLY1305_CTX *ctx, uint8_t tag[HK_POLY1305_TAG_LEN]);
void hk_poly1305(uint8_t tag[HK_POLY1305_TAG_LEN], const uint8_t key[32],
				 const uint8_t *m, size_t len);

/*
 * AEAD。ct 与 pt 可以是同一块内存（原地加解密），两个方向都支持。
 * nonce 用 RFC 8439 的 12 字节 IETF 形式：前 4 字节固定，后 8 字节计数器。
 *   —— HAP 的帧加密正是这个形式。
 * 返回 0 成功；decrypt 时 tag 不匹配返回 -1（比较是常数时间的）。
 */
int hk_aead_encrypt(uint8_t *ct, uint8_t tag[HK_POLY1305_TAG_LEN],
					const uint8_t key[HK_CHACHA20_KEY_LEN],
					const uint8_t nonce[HK_CHACHA20_NONCE_LEN],
					const uint8_t *aad, size_t aad_len,
					const uint8_t *pt, size_t pt_len);

int hk_aead_decrypt(uint8_t *pt, const uint8_t tag[HK_POLY1305_TAG_LEN],
					const uint8_t key[HK_CHACHA20_KEY_LEN],
					const uint8_t nonce[HK_CHACHA20_NONCE_LEN],
					const uint8_t *aad, size_t aad_len,
					const uint8_t *ct, size_t ct_len);

#ifdef __cplusplus
}
#endif

#endif /* HK_CHACHAPOLY_H */

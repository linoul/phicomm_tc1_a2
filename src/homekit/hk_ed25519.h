/*
 * hk_ed25519.h —— Ed25519 签名/验签（RFC 8032）
 *
 * HomeKit 里它的作用是"配件证明自己是自己"：
 *   - 配件出厂（或首次启动）生成一对长期密钥 LTSK/LTPK；
 *   - LTPK 通过 mDNS TXT 记录公布给控制器；
 *   - Pair Verify 过程中配件用 LTSK 对握手数据签名，控制器用 LTPK 验签。
 * 所以这里的验签既要用在"配件签给控制器"，也要能验"控制器签给配件"。
 */
#ifndef HK_ED25519_H
#define HK_ED25519_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HK_ED25519_SEED_LEN 32
#define HK_ED25519_PUB_LEN 32
#define HK_ED25519_SIG_LEN 64

/* 由 32 字节种子（即私钥）导出公钥 */
int hk_ed25519_pubkey(uint8_t pk[HK_ED25519_PUB_LEN],
					  const uint8_t seed[HK_ED25519_SEED_LEN]);

/*
 * 签名。pk 必须是对应 seed 的公钥（传进来了就不用在内部重算一遍，
 * 省一次标量乘 —— M4 上一次标量乘是毫秒级的）。
 * sig = R(32) || S(32)。
 */
int hk_ed25519_sign(uint8_t sig[HK_ED25519_SIG_LEN],
					const uint8_t *msg, size_t msg_len,
					const uint8_t seed[HK_ED25519_SEED_LEN],
					const uint8_t pk[HK_ED25519_PUB_LEN]);

/* 验签：合法返回 0，不合法返回 -1。拒绝签名可锻造的 S >= L 的畸形值。 */
int hk_ed25519_verify(const uint8_t sig[HK_ED25519_SIG_LEN],
					  const uint8_t *msg, size_t msg_len,
					  const uint8_t pk[HK_ED25519_PUB_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* HK_ED25519_H */

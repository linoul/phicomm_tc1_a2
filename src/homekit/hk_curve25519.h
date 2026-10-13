/*
 * hk_curve25519.h —— X25519（RFC 7748）
 *
 * HomeKit Pair Verify 用它做密钥协商：
 *   - 配件用长期密钥派生的标量 + 临时公钥，控制器用它的临时公私钥；
 *   - 双方各算一次 X25519 得到共享密钥，再 HKDF 出会话密钥。
 * 也就是说这个函数的正确性直接决定"能不能配上对"。
 */
#ifndef HK_CURVE25519_H
#define HK_CURVE25519_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HK_X25519_LEN 32

/*
 * 通用标量乘法：out = scalar * point（scalar 会按 RFC 7748 做 clamp）。
 * point 是 u 坐标的 32 字节小端表示。out 可以与 point 或 scalar 重叠。
 * 返回 0。注意：本函数**不做**"结果是全零就拒绝"的检查 ——
 * HAP 自己会在上层校验；如果你的场景需要防小阶点攻击，自己要加。
 */
int hk_x25519(uint8_t out[HK_X25519_LEN],
			  const uint8_t scalar[HK_X25519_LEN],
			  const uint8_t point[HK_X25519_LEN]);

/* out = scalar * 基点9，即由私钥求公钥 */
int hk_x25519_base(uint8_t out[HK_X25519_LEN], const uint8_t scalar[HK_X25519_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* HK_CURVE25519_H */

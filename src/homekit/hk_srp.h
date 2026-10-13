/*
 * hk_srp.h —— SRP-6a（HomeKit Pair Setup 的核心）
 *
 * 这是 HomeKit 配对里最关键也最容易写错的一步：控制器（iPhone）用
 * 8 位配对码向配件证明"我知道配对码"，但配对码本身**不在网络上传输**。
 * SRP 保证的就是这一点 —— 即使抓包也拿不到配对码，也无法离线爆破
 * （每次尝试都要和配件做一次完整的 3072 位指数运算）。
 *
 * 两个必须注意的实现差异：
 *   1) RFC 5054 用 **SHA-1**，HomeKit 规定同一套流程改用 **SHA-512**；
 *      本实现用 SHA-512，不要拿 RFC 5054 附录 B 的向量来对。
 *   2) 群是 RFC 5054 附录 A 的 3072 位组（见 hk_srp_group.h），g=5。
 *
 * 随机数 a/b 由调用方传入（目标板上取硬件 TRNG）。这样这个模块完全不
 * 依赖平台，能在 PC 上跑固定向量的端到端自测。
 */
#ifndef HK_SRP_H
#define HK_SRP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HK_SRP_HASH_LEN 64 /* SHA-512 */
#define HK_SRP_RAND_LEN 32 /* 随机标量长度（HAP 用 256 位） */

/*
 * x = H(salt | H(user | ":" | password))
 * 注意 user/password 只在**配件本地**参与计算，不上网。
 */
void hk_srp_x(uint8_t x[HK_SRP_HASH_LEN],
			  const uint8_t *salt, size_t salt_len,
			  const uint8_t *user, size_t user_len,
			  const uint8_t *pass, size_t pass_len);

/* v = g^x mod N，即"验证元"。配件持久化保存它，配对时直接用，不必每次重算 */
void hk_srp_verifier(uint8_t v[384], const uint8_t x[HK_SRP_HASH_LEN]);

/* 客户端（控制器）侧：A = g^a mod N */
void hk_srp_client_public(uint8_t A[384], const uint8_t a[HK_SRP_RAND_LEN]);

/* 服务端（配件）侧：B = (k*v + g^b) mod N，k = H(N | PAD(g)) */
void hk_srp_server_public(uint8_t B[384], const uint8_t v[384], const uint8_t b[HK_SRP_RAND_LEN]);

/* u = H(PAD(A) | PAD(B)) */
void hk_srp_u(uint8_t u[HK_SRP_HASH_LEN], const uint8_t A[384], const uint8_t B[384]);

/* 服务端会话：S = (A * v^u)^b mod N，K = H(S) */
void hk_srp_server_session(uint8_t S[384], uint8_t K[HK_SRP_HASH_LEN],
						   const uint8_t A[384], const uint8_t v[384],
						   const uint8_t u[HK_SRP_HASH_LEN], const uint8_t b[HK_SRP_RAND_LEN]);

/* 客户端会话：S = (B - k*g^x)^(a + u*x) mod N，K = H(S) */
void hk_srp_client_session(uint8_t S[384], uint8_t K[HK_SRP_HASH_LEN],
						   const uint8_t B[384], const uint8_t x[HK_SRP_HASH_LEN],
						   const uint8_t u[HK_SRP_HASH_LEN], const uint8_t a[HK_SRP_RAND_LEN]);

/*
 * 双方一致性证明（SRP-6a 的 M1/M2）：
 *   M1 = H( (H(N) XOR H(g)) | H(user) | salt | A | B | K )
 *   M2 = H( A | M1 | K )
 * 服务端收到客户端算的 M1 后必须与自己的 M1 做**常数时间**比较；
 * 不相等说明配对码不对，此时应返回错误并**丢弃会话**。
 */
void hk_srp_m1(uint8_t m1[HK_SRP_HASH_LEN],
			   const uint8_t *user, size_t user_len,
			   const uint8_t *salt, size_t salt_len,
			   const uint8_t A[384], const uint8_t B[384],
			   const uint8_t K[HK_SRP_HASH_LEN]);

void hk_srp_m2(uint8_t m2[HK_SRP_HASH_LEN],
			   const uint8_t A[384], const uint8_t m1[HK_SRP_HASH_LEN],
			   const uint8_t K[HK_SRP_HASH_LEN]);

/* 常数时间比较（相等返回 1）。比较证明值时**不要**用 memcmp */
int hk_srp_ct_eq(const uint8_t *a, const uint8_t *b, size_t len);

/* 校验 A/B 是否落在合法区间 (1, N-1)，拒绝退化输入 */
int hk_srp_public_ok(const uint8_t pub[384]);

#ifdef __cplusplus
}
#endif

#endif /* HK_SRP_H */

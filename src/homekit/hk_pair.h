/*
 * hk_pair.h —— HAP 的 Pair Setup / Pair Verify 状态机（配件侧）
 *
 * 这是 HomeKit 的"进门"流程。它用到的密码学全在第一阶段里，
 * 这一层负责的是**顺序、拼接和校验** —— 也就是最容易"自洽地错"的地方。
 *
 * 关于正确性：本实现的每一步都对着 HAP-NodeJS（Homebridge 的实现）
 * 的 HAPServer.ts 核对过，见 doc/HOMEKIT.md 第 10 节。有两条特别容易记错，
 * 写在最前面防止以后有人"凭印象改回去"：
 *
 *   1) M5/M6 的 Ed25519 签名**不是**直接签 "ID || 公钥"，
 *      而是签 "HKDF派生值(32) || 本方ID || 本方公钥"：
 *        M5（控制器签，配件验）：HKDF(K, "Pair-Setup-Controller-Sign-Salt",
 *                                "Pair-Setup-Controller-Sign-Info") || iOS_ID || iOS_LTPK
 *        M6（配件签）：        HKDF(K, "Pair-Setup-Accessory-Sign-Salt",
 *                                "Pair-Setup-Accessory-Sign-Info") || 配件ID || 配件LTPK
 *      少拼那个 32 字节的派生值，表现是 iOS 在最后一步报"配对失败"。
 *   2) AEAD 的 nonce 是 **4 个零字节 + 8 字节 ASCII**（"PS-Msg05"/"PS-Msg06"/
 *      "PV-Msg02"/"PV-Msg03"），不是 12 字节随机。
 *
 * 设计约束：
 *   - 纯 C99，不碰 SDK，可在 PC 上跑完整流程自测；
 *   - **随机数由调用方注入**（salt / SRP 的 b / 临时 X25519 私钥）。
 *     这样主机自测能用固定值复现，固件侧则填硬件 TRNG；
 *   - 持久化由调用方做（本层只把结果留在结构里，不写 flash）。
 *
 * 内存账（RAM，只在使用期间存在）：
 *   HK_SETUP ≈ 1.3KB（v 384 + B 384 + S 384 + K 64 + salt 16 + b 32 + ...）
 *   HK_VERIFY ≈ 200B
 *
 * 为什么 B 和 S 不合并成一个缓冲：M3 要先算 u=H(A|B)，再算 S=(A·v^u)^b
 * 得到 K=H(S)，**然后**算 M1 —— 而 M1 里又要用 B。也就是说 B 的生命周期
 * 一直延续到 K 算出来之后，和 S 是重叠的，没法复用。
 */
#ifndef HK_PAIR_H
#define HK_PAIR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HK_SRP_N_LEN 384		/* SRP 的 A/B/S 都是 384 字节 */
#define HK_SRP_K_LEN 64			/* K = H(S)，也是 HKDF 的 ikm */
#define HK_PAIR_SALT_LEN 16		/* SRP salt */
#define HK_PAIR_RAND_LEN 32		/* SRP 的 b、X25519 私钥 */
#define HK_PAIR_ID_LEN_MAX 36	/* iOS 的 pairing ID 是 36 字节 UUID 字符串 */
#define HK_SETUP_CODE_MAX 12	/* "XXX-XX-XXX" */

/* 返回码 */
#define HK_PAIR_OK 0
#define HK_PAIR_ERR_INVALID -1 /* 报文格式错 / 缺字段 / 状态不对 */
#define HK_PAIR_ERR_AUTH -2	   /* 配对码错、签名不对、未配对 */
#define HK_PAIR_ERR_NOMEM -3   /* 响应缓冲装不下 */

/* 响应 TLV 的建议缓冲：M2 最大（state + salt + 384 字节 B ≈ 410） */
#define HK_PAIR_RESP_MIN 512

/* 配件的长期身份（Ed25519）。LTSK 必须来自真随机数，且要持久化。 */
typedef struct _HK_ACCESSORY
{
	uint8_t ltsk[32];
	uint8_t ltpk[32];
	const char *id; /* 配件 pairing ID，如 "F0:FE:6B:E6:91:40"；不拷贝，调用方保证生命周期 */
} HK_ACCESSORY;

/*
 * Pair Setup 会话。
 * pub 在 M2 阶段存 B（发给控制器），M3 阶段算完 u 之后被 S 覆盖 —— 因为
 * 算出 S 之后 B 就再也没用了，而 S 只需要活到算出 K 为止。
 */
typedef struct _HK_SETUP
{
	int state;						/* 0=未开始 2=已发M2 4=已发M4 6=完成 */
	uint8_t salt[HK_PAIR_SALT_LEN]; /* 配件生成的 SRP salt */
	uint8_t b[HK_PAIR_RAND_LEN];	/* 配件的 SRP 秘密指数（随机） */
	uint8_t v[HK_SRP_N_LEN];		/* 验证元 g^x mod N */
	uint8_t B[HK_SRP_N_LEN];		/* M2 发出的服务端公钥（M3 算 M1 时还要用） */
	uint8_t S[HK_SRP_N_LEN];		/* 共享秘密（算出 K 之后就没用了） */
	uint8_t K[HK_SRP_K_LEN];		/* 会话秘密 H(S)，M5/M6 的 HKDF 输入 */
	uint8_t ios_id[HK_PAIR_ID_LEN_MAX]; /* M5 拿到的控制器 ID */
	size_t ios_id_len;
	uint8_t ios_ltpk[32];			/* M5 拿到的控制器长期公钥 */
} HK_SETUP;

/*
 * Pair Verify 会话。
 * a2c = 配件发给控制器的加密密钥；c2a = 控制器发给配件的解密密钥。
 * 注意命名容易反：HAP 的 info 串是站在"控制器"视角写的，
 * 所以"控制器读的那个密钥"（Control-Read-Encryption-Key）是**配件发出**用的。
 */
typedef struct _HK_VERIFY
{
	int state;				   /* 0=未开始 2=已发M2 4=完成 */
	uint8_t epriv[HK_PAIR_RAND_LEN]; /* 本会话临时 X25519 私钥（随机） */
	uint8_t epub[HK_PAIR_RAND_LEN];	 /* 本会话临时 X25519 公钥 */
	uint8_t cpub[HK_PAIR_RAND_LEN];	 /* 控制器的临时公钥 */
	uint8_t shared[HK_PAIR_RAND_LEN];/* X25519 共享秘密 */
	uint8_t a2c[32];			   /* Accessory -> Controller 会话密钥 */
	uint8_t c2a[32];			   /* Controller -> Accessory 会话密钥 */
	uint8_t ios_id[HK_PAIR_ID_LEN_MAX];
	size_t ios_id_len;
} HK_VERIFY;

void hk_setup_init(HK_SETUP *s);
void hk_verify_init(HK_VERIFY *v);

/*
 * Pair Setup M1 -> M2。
 *   in : 配对码（"XXX-XX-XXX"，含连字符）、salt、b（都要来自真随机数）
 *   out: resp = [state=2][salt][publicKey=B]
 *
 * 注意：M1 请求本身不带有效载荷（只有 [state=1][method=0]），所以这一步
 * 不需要解析请求，调用方确认 method/state 正确即可。SRP 的 user 恒为
 * "Pair-Setup"（HAP 规定），与配件 ID 无关。
 */
int hk_setup_m1_to_m2(HK_SETUP *s, const char *setup_code,
					  const uint8_t salt[HK_PAIR_SALT_LEN],
					  const uint8_t b[HK_PAIR_RAND_LEN],
					  uint8_t *resp, size_t *resp_len, size_t resp_cap);

/*
 * Pair Setup M3 -> M4（这一步校验配对码）。
 *   in : A（384）、proof（控制器算的 M1，64）
 *   out: resp = [state=4][proof=M2]
 * 校验失败返回 HK_PAIR_ERR_AUTH，*resp_len 会被写成一条错误 TLV。
 */
int hk_setup_m3_to_m4(HK_SETUP *s, const uint8_t A[HK_SRP_N_LEN],
					  const uint8_t proof[HK_SRP_K_LEN],
					  uint8_t *resp, size_t *resp_len, size_t resp_cap);

/*
 * Pair Setup M5 -> M6。
 *   in : enc = 请求里的 encryptedData（密文 + 16 字节 tag）
 *   out: resp = [state=6][encryptedData]（内含配件 ID、LTPK、签名）
 *   成功后 s->ios_id / s->ios_ltpk 里有控制器的身份，调用方应持久化。
 */
int hk_setup_m5_to_m6(HK_SETUP *s, const HK_ACCESSORY *acc,
					  const uint8_t *enc, size_t enc_len,
					  uint8_t *resp, size_t *resp_len, size_t resp_cap);

/*
 * Pair Verify M1 -> M2。
 *   in : 控制器的临时公钥 cpub（32）、本方临时私钥 epriv（32，随机）
 *   out: resp = [state=2][encryptedData][publicKey]
 */
int hk_verify_m1_to_m2(HK_VERIFY *v, const HK_ACCESSORY *acc,
					   const uint8_t cpub[HK_PAIR_RAND_LEN],
					   const uint8_t epriv[HK_PAIR_RAND_LEN],
					   uint8_t *resp, size_t *resp_len, size_t resp_cap);

/*
 * Pair Verify M3 -> M4。
 *   in : enc = 请求里的 encryptedData；ios_ltpk = **已存储的**该控制器的长期公钥
 *   out: resp = [state=4]
 *   成功后 v->a2c / v->c2a 是会话密钥，之后所有 HAP 通信都用它。
 * ios_ltpk 传 NULL 表示"没配过对" -> 返回 HK_PAIR_ERR_AUTH。
 */
int hk_verify_m3_to_m4(HK_VERIFY *v, const uint8_t ios_ltpk[32],
					   const uint8_t *enc, size_t enc_len,
					   uint8_t *resp, size_t *resp_len, size_t resp_cap);

/*
 * 由 Ed25519 私钥种子直接生成配件身份（LTSK/LTPK）。
 * 种子必须来自硬件 TRNG；同一颗种子每次开机都必须一样，所以要持久化。
 * 返回 0 成功。
 */
int hk_accessory_from_seed(HK_ACCESSORY *acc, const char *id, const uint8_t seed[32]);

#ifdef __cplusplus
}
#endif

#endif /* HK_PAIR_H */

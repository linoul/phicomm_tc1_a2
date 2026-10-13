/*
 * hk_pair.c —— HAP 的 Pair Setup / Pair Verify 状态机（配件侧）
 *
 * 每一步的拼接规则都对着 HAP-NodeJS 的 HAPServer.ts 核对过（见 doc/HOMEKIT.md
 * 第 10 节）。这个文件里几乎每一处"看起来可以想当然"的地方其实都有讲究，
 * 所以注释写得比较啰嗦 —— 它们都是从参考实现里抄下来的事实，不是推测。
 *
 * 三条贯穿全文件的约定：
 *   1) 随机数不在这里生成。salt / b / 临时 X25519 私钥都由调用方传入，
 *      固件侧填硬件 TRNG，自测填固定值。
 *   2) 失败时也要填好 *resp_len（一条错误 TLV），因为 HAP 的错误是靠
 *      "正常 HTTP 200 + TLV 里的 error 字段"表达的，不是靠 HTTP 状态码。
 *   3) 所有密钥材料用完不清零（本层不知道什么时候算"用完"）。
 *      调用方在配对会话结束时应 memset 掉 HK_SETUP / HK_VERIFY。
 */
#include "hk_pair.h"

#include "hk_chachapoly.h"
#include "hk_curve25519.h"
#include "hk_ed25519.h"
#include "hk_kdf.h"
#include "hk_srp.h"
#include "hk_tlv.h"

#include <string.h>

/* SRP 的 user 恒为 "Pair-Setup"（HAP 规定，与配件 ID 无关） */
static const uint8_t HK_SRP_USER[] = "Pair-Setup";
#define HK_SRP_USER_LEN 10

/* AEAD 的 nonce = 4 个零字节 + 8 字节 ASCII。这 8 个字节错一个字母，
 * 表现就是"解密失败"，而且没有别的信息可查。 */
static void hk_nonce(uint8_t nonce[12], const char *tag8)
{
	memset(nonce, 0, 4);
	memcpy(nonce + 4, tag8, 8);
}

/* 统一的错误出口：把 [state][error] 写进 resp。写不下就什么都不写。 */
static int hk_fail(uint8_t *resp, size_t *resp_len, size_t resp_cap,
				   uint8_t state, uint8_t err)
{
	size_t p = 0;

	if (hk_tlv_put_error(resp, &p, resp_cap, state, err) != 0)
	{
		if (resp_len)
			*resp_len = 0;
		return HK_PAIR_ERR_NOMEM;
	}
	if (resp_len)
		*resp_len = p;
	return (err == HK_ERR_AUTH) ? HK_PAIR_ERR_AUTH : HK_PAIR_ERR_INVALID;
}

void hk_setup_init(HK_SETUP *s)
{
	memset(s, 0, sizeof(*s));
}

void hk_verify_init(HK_VERIFY *v)
{
	memset(v, 0, sizeof(*v));
}

int hk_accessory_from_seed(HK_ACCESSORY *acc, const char *id, const uint8_t seed[32])
{
	if (acc == NULL || id == NULL || seed == NULL)
		return HK_PAIR_ERR_INVALID;
	memcpy(acc->ltsk, seed, 32);
	if (hk_ed25519_pubkey(acc->ltpk, seed) != 0)
		return HK_PAIR_ERR_INVALID;
	acc->id = id;
	return HK_PAIR_OK;
}

/* ------------------------------------------------------------------ */
/* Pair Setup                                                          */
/* ------------------------------------------------------------------ */

int hk_setup_m1_to_m2(HK_SETUP *s, const char *setup_code,
					  const uint8_t salt[HK_PAIR_SALT_LEN],
					  const uint8_t b[HK_PAIR_RAND_LEN],
					  uint8_t *resp, size_t *resp_len, size_t resp_cap)
{
	uint8_t x[HK_SRP_K_LEN];
	size_t p = 0;
	size_t code_len;

	/* state(3) + salt(2+16) + publicKey(2*2 + 384) */
	if (s == NULL || setup_code == NULL || salt == NULL || b == NULL)
		return HK_PAIR_ERR_INVALID;
	if (resp == NULL || resp_cap < 3 + 18 + 388)
		return HK_PAIR_ERR_NOMEM;

	code_len = strlen(setup_code);
	if (code_len == 0 || code_len > HK_SETUP_CODE_MAX)
		return HK_PAIR_ERR_INVALID;

	memcpy(s->salt, salt, HK_PAIR_SALT_LEN);
	memcpy(s->b, b, HK_PAIR_RAND_LEN);

	/* x = H(salt | H("Pair-Setup" | ":" | 配对码)) */
	hk_srp_x(x, salt, HK_PAIR_SALT_LEN,
			 HK_SRP_USER, HK_SRP_USER_LEN,
			 (const uint8_t *)setup_code, code_len);
	/* v = g^x mod N（持久化它就能避免每次配对都做一次 3072 位模幂） */
	hk_srp_verifier(s->v, x);
	/* B = k*v + g^b mod N */
	hk_srp_server_public(s->B, s->v, s->b);

	if (hk_tlv_put_u8(resp, &p, resp_cap, HK_TLV_STATE, 2) != 0 ||
		hk_tlv_put(resp, &p, resp_cap, HK_TLV_SALT, salt, HK_PAIR_SALT_LEN) != 0 ||
		hk_tlv_put(resp, &p, resp_cap, HK_TLV_PUBLICKEY, s->B, HK_SRP_N_LEN) != 0)
	{
		*resp_len = 0;
		return HK_PAIR_ERR_NOMEM;
	}

	*resp_len = p;
	s->state = 2;
	return HK_PAIR_OK;
}

int hk_setup_m3_to_m4(HK_SETUP *s, const uint8_t A[HK_SRP_N_LEN],
					  const uint8_t proof[HK_SRP_K_LEN],
					  uint8_t *resp, size_t *resp_len, size_t resp_cap)
{
	uint8_t u[HK_SRP_K_LEN];
	uint8_t m1[HK_SRP_K_LEN];
	uint8_t m2[HK_SRP_K_LEN];
	size_t p = 0;

	/* state(3) + proof(2+64) */
	if (s == NULL || A == NULL || proof == NULL)
		return HK_PAIR_ERR_INVALID;
	if (resp == NULL || resp_cap < 3 + 66)
		return HK_PAIR_ERR_NOMEM;

	/* 状态机：只有刚发过 M2 的连接才能走到 M3。
	 * HAP 要求乱序请求直接拒绝 —— 否则可以用旧报文重放。 */
	if (s->state != 2)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_UNKNOWN);

	/* 退化输入（A=0 / A>=N）会让 S 退化成可预测的值，先挡掉 */
	if (!hk_srp_public_ok(A))
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_AUTH);

	/* u = H(PAD(A) | PAD(B)) */
	hk_srp_u(u, A, s->B);
	/* S = (A * v^u)^b mod N，K = H(S) */
	hk_srp_server_session(s->S, s->K, A, s->v, u, s->b);

	/* 只有配对码正确时，双方算出的 M1 才会相等。
	 * 必须用常数时间比较 —— 用 memcmp 会泄漏"前几个字节是对的"。 */
	hk_srp_m1(m1, HK_SRP_USER, HK_SRP_USER_LEN, s->salt, HK_PAIR_SALT_LEN,
			  A, s->B, s->K);
	if (!hk_srp_ct_eq(m1, proof, HK_SRP_K_LEN))
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_AUTH);

	hk_srp_m2(m2, A, m1, s->K);

	if (hk_tlv_put_u8(resp, &p, resp_cap, HK_TLV_STATE, 4) != 0 ||
		hk_tlv_put(resp, &p, resp_cap, HK_TLV_PASSWORD_PROOF, m2, HK_SRP_K_LEN) != 0)
	{
		*resp_len = 0;
		return HK_PAIR_ERR_NOMEM;
	}

	*resp_len = p;
	s->state = 4;
	return HK_PAIR_OK;
}

int hk_setup_m5_to_m6(HK_SETUP *s, const HK_ACCESSORY *acc,
					  const uint8_t *enc, size_t enc_len,
					  uint8_t *resp, size_t *resp_len, size_t resp_cap)
{
	uint8_t key[32];  /* Pair-Setup-Encrypt：M5/M6 的报文加密 */
	uint8_t skey[32]; /* Pair-Setup-Controller-Sign：验控制器的签名 */
	uint8_t akey[32]; /* Pair-Setup-Accessory-Sign：配件自己的签名 */
	uint8_t nonce[12];
	uint8_t pt[160];  /* 解密后的 subTLV：id(38) + pk(34) + proof(66) = 138 */
	uint8_t msg[160]; /* M6 的明文 subTLV */
	uint8_t ct[160];
	uint8_t tag[HK_POLY1305_TAG_LEN];
	uint8_t sig[HK_ED25519_SIG_LEN];
	uint8_t ios_id[HK_PAIR_ID_LEN_MAX];
	uint8_t ios_ltpk[HK_ED25519_PUB_LEN];
	uint8_t ios_proof[HK_ED25519_SIG_LEN];
	uint8_t material[32 + HK_PAIR_ID_LEN_MAX + HK_ED25519_PUB_LEN];
	size_t pt_len, msg_len, id_len, mlen, p = 0;
	int n;
	const uint8_t *aad = NULL;
	size_t aad_len = 0;

	if (s == NULL || acc == NULL || acc->id == NULL || enc == NULL)
		return HK_PAIR_ERR_INVALID;
	if (resp == NULL || resp_cap < 3 + 2 + sizeof(msg) + 16)
		return HK_PAIR_ERR_NOMEM;

	if (s->state != 4)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_UNKNOWN);

	/* encryptedData 至少要有 16 字节的 tag，否则没法认证 */
	if (enc_len < HK_POLY1305_TAG_LEN)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_AUTH);
	pt_len = enc_len - HK_POLY1305_TAG_LEN;
	if (pt_len > sizeof(pt))
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_AUTH);

	/* 会话加密密钥：ikm 是 SRP 的 K（64 字节），不是 S */
	if (hk_hkdf_sha512((const uint8_t *)"Pair-Setup-Encrypt-Salt", 23,
					   s->K, HK_SRP_K_LEN,
					   (const uint8_t *)"Pair-Setup-Encrypt-Info", 23,
					   key, 32) != 0)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_UNKNOWN);

	hk_nonce(nonce, "PS-Msg05");
	if (hk_aead_decrypt(pt, enc + pt_len, key, nonce, aad, aad_len, enc, pt_len) != 0)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_AUTH);

	n = hk_tlv_find(pt, pt_len, HK_TLV_IDENTIFIER, ios_id, sizeof(ios_id));
	if (n <= 0 || (size_t)n > HK_PAIR_ID_LEN_MAX)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_UNKNOWN);
	id_len = (size_t)n;

	n = hk_tlv_find(pt, pt_len, HK_TLV_PUBLICKEY, ios_ltpk, sizeof(ios_ltpk));
	if (n != HK_ED25519_PUB_LEN)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_UNKNOWN);

	n = hk_tlv_find(pt, pt_len, HK_TLV_SIGNATURE, ios_proof, sizeof(ios_proof));
	if (n != HK_ED25519_SIG_LEN)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_UNKNOWN);

	/*
	 * 验控制器签名。签名内容是：
	 *   HKDF(K, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info")
	 *   || 控制器 ID || 控制器 LTPK
	 * 那个 32 字节的派生值很容易被漏掉，漏掉的表现是 iOS 报"无法配对"。
	 */
	if (hk_hkdf_sha512((const uint8_t *)"Pair-Setup-Controller-Sign-Salt", 31,
					   s->K, HK_SRP_K_LEN,
					   (const uint8_t *)"Pair-Setup-Controller-Sign-Info", 31,
					   skey, 32) != 0)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_UNKNOWN);

	mlen = 0;
	memcpy(material + mlen, skey, 32);
	mlen += 32;
	memcpy(material + mlen, ios_id, id_len);
	mlen += id_len;
	memcpy(material + mlen, ios_ltpk, HK_ED25519_PUB_LEN);
	mlen += HK_ED25519_PUB_LEN;

	if (hk_ed25519_verify(ios_proof, material, mlen, ios_ltpk) != 0)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_AUTH);

	/* ---- 到此配对成功：先把控制器的身份存下来（调用方负责持久化） ---- */
	memcpy(s->ios_id, ios_id, id_len);
	s->ios_id_len = id_len;
	memcpy(s->ios_ltpk, ios_ltpk, HK_ED25519_PUB_LEN);

	/* M6：配件签名。签名内容同样是"派生值 || 配件ID || 配件LTPK"。 */
	if (hk_hkdf_sha512((const uint8_t *)"Pair-Setup-Accessory-Sign-Salt", 30,
					   s->K, HK_SRP_K_LEN,
					   (const uint8_t *)"Pair-Setup-Accessory-Sign-Info", 30,
					   akey, 32) != 0)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_UNKNOWN);

	{
		size_t alen = strlen(acc->id);
		if (alen == 0 || alen > HK_PAIR_ID_LEN_MAX)
			return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_UNKNOWN);
		mlen = 0;
		memcpy(material + mlen, akey, 32);
		mlen += 32;
		memcpy(material + mlen, acc->id, alen);
		mlen += alen;
		memcpy(material + mlen, acc->ltpk, HK_ED25519_PUB_LEN);
		mlen += HK_ED25519_PUB_LEN;
	}

	if (hk_ed25519_sign(sig, material, mlen, acc->ltsk, acc->ltpk) != 0)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_UNKNOWN);

	msg_len = 0;
	if (hk_tlv_put(msg, &msg_len, sizeof(msg), HK_TLV_IDENTIFIER,
				   (const uint8_t *)acc->id, strlen(acc->id)) != 0 ||
		hk_tlv_put(msg, &msg_len, sizeof(msg), HK_TLV_PUBLICKEY,
				   acc->ltpk, HK_ED25519_PUB_LEN) != 0 ||
		hk_tlv_put(msg, &msg_len, sizeof(msg), HK_TLV_SIGNATURE, sig, HK_ED25519_SIG_LEN) != 0)
	{
		*resp_len = 0;
		return HK_PAIR_ERR_NOMEM;
	}

	hk_nonce(nonce, "PS-Msg06");
	if (hk_aead_encrypt(ct, tag, key, nonce, aad, aad_len, msg, msg_len) != 0)
		return hk_fail(resp, resp_len, resp_cap, 6, HK_ERR_UNKNOWN);

	/* encryptedData = 密文 || tag（HAP 的约定，tag 直接拼在后面） */
	if (hk_tlv_put_u8(resp, &p, resp_cap, HK_TLV_STATE, 6) != 0)
	{
		*resp_len = 0;
		return HK_PAIR_ERR_NOMEM;
	}
	{
		uint8_t blob[sizeof(ct) + HK_POLY1305_TAG_LEN];
		size_t blob_len = 0;
		memcpy(blob + blob_len, ct, msg_len);
		blob_len += msg_len;
		memcpy(blob + blob_len, tag, HK_POLY1305_TAG_LEN);
		blob_len += HK_POLY1305_TAG_LEN;
		if (hk_tlv_put(resp, &p, resp_cap, HK_TLV_ENCRYPTEDDATA, blob, blob_len) != 0)
		{
			*resp_len = 0;
			return HK_PAIR_ERR_NOMEM;
		}
	}

	*resp_len = p;
	s->state = 6;
	return HK_PAIR_OK;
}

/* ------------------------------------------------------------------ */
/* Pair Verify                                                         */
/* ------------------------------------------------------------------ */

int hk_verify_m1_to_m2(HK_VERIFY *v, const HK_ACCESSORY *acc,
					   const uint8_t cpub[HK_PAIR_RAND_LEN],
					   const uint8_t epriv[HK_PAIR_RAND_LEN],
					   uint8_t *resp, size_t *resp_len, size_t resp_cap)
{
	uint8_t material[32 + HK_PAIR_ID_LEN_MAX + 32];
	uint8_t sig[HK_ED25519_SIG_LEN];
	uint8_t msg[128];
	uint8_t ct[128];
	uint8_t tag[HK_POLY1305_TAG_LEN];
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t blob[sizeof(ct) + HK_POLY1305_TAG_LEN];
	size_t msg_len = 0, blob_len = 0, mlen = 0, p = 0, alen;
	const uint8_t *aad = NULL;
	size_t aad_len = 0;

	if (v == NULL || acc == NULL || acc->id == NULL || cpub == NULL || epriv == NULL)
		return HK_PAIR_ERR_INVALID;
	if (resp == NULL || resp_cap < 3 + 2 + sizeof(msg) + 16 + 2 + 32)
		return HK_PAIR_ERR_NOMEM;

	alen = strlen(acc->id);
	if (alen == 0 || alen > HK_PAIR_ID_LEN_MAX)
		return HK_PAIR_ERR_INVALID;

	memcpy(v->epriv, epriv, HK_PAIR_RAND_LEN);
	memcpy(v->cpub, cpub, HK_PAIR_RAND_LEN);

	/* 本会话的临时密钥对（每次 verify 都换新的） */
	hk_x25519_base(v->epub, epriv);
	/* 共享秘密 = X25519(我方临时私钥, 控制器临时公钥) */
	hk_x25519(v->shared, epriv, cpub);

	/*
	 * 签名内容 = 我方临时公钥 || 配件ID || 控制器临时公钥
	 * 注意这里是**裸拼接**，没有 HKDF 派生值 —— 和 Pair Setup 的 M5/M6 不同。
	 */
	memcpy(material + mlen, v->epub, 32);
	mlen += 32;
	memcpy(material + mlen, acc->id, alen);
	mlen += alen;
	memcpy(material + mlen, cpub, 32);
	mlen += 32;

	if (hk_ed25519_sign(sig, material, mlen, acc->ltsk, acc->ltpk) != 0)
		return hk_fail(resp, resp_len, resp_cap, 2, HK_ERR_UNKNOWN);

	if (hk_tlv_put(msg, &msg_len, sizeof(msg), HK_TLV_IDENTIFIER,
				   (const uint8_t *)acc->id, alen) != 0 ||
		hk_tlv_put(msg, &msg_len, sizeof(msg), HK_TLV_SIGNATURE, sig, HK_ED25519_SIG_LEN) != 0)
	{
		*resp_len = 0;
		return HK_PAIR_ERR_NOMEM;
	}

	if (hk_hkdf_sha512((const uint8_t *)"Pair-Verify-Encrypt-Salt", 24,
					   v->shared, 32,
					   (const uint8_t *)"Pair-Verify-Encrypt-Info", 24,
					   key, 32) != 0)
		return hk_fail(resp, resp_len, resp_cap, 2, HK_ERR_UNKNOWN);

	hk_nonce(nonce, "PV-Msg02");
	if (hk_aead_encrypt(ct, tag, key, nonce, aad, aad_len, msg, msg_len) != 0)
		return hk_fail(resp, resp_len, resp_cap, 2, HK_ERR_UNKNOWN);

	memcpy(blob + blob_len, ct, msg_len);
	blob_len += msg_len;
	memcpy(blob + blob_len, tag, HK_POLY1305_TAG_LEN);
	blob_len += HK_POLY1305_TAG_LEN;

	if (hk_tlv_put_u8(resp, &p, resp_cap, HK_TLV_STATE, 2) != 0 ||
		hk_tlv_put(resp, &p, resp_cap, HK_TLV_ENCRYPTEDDATA, blob, blob_len) != 0 ||
		hk_tlv_put(resp, &p, resp_cap, HK_TLV_PUBLICKEY, v->epub, 32) != 0)
	{
		*resp_len = 0;
		return HK_PAIR_ERR_NOMEM;
	}

	*resp_len = p;
	v->state = 2;
	return HK_PAIR_OK;
}

int hk_verify_m3_to_m4(HK_VERIFY *v, const uint8_t ios_ltpk[32],
					   const uint8_t *enc, size_t enc_len,
					   uint8_t *resp, size_t *resp_len, size_t resp_cap)
{
	uint8_t material[32 + HK_PAIR_ID_LEN_MAX + 32];
	uint8_t pt[128];
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t ios_id[HK_PAIR_ID_LEN_MAX];
	uint8_t proof[HK_ED25519_SIG_LEN];
	size_t pt_len, id_len, mlen = 0, p = 0;
	int n;
	const uint8_t *aad = NULL;
	size_t aad_len = 0;

	if (v == NULL || enc == NULL)
		return HK_PAIR_ERR_INVALID;
	if (resp == NULL || resp_cap < 3)
		return HK_PAIR_ERR_NOMEM;

	if (v->state != 2)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_UNKNOWN);

	if (enc_len < HK_POLY1305_TAG_LEN)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_AUTH);
	pt_len = enc_len - HK_POLY1305_TAG_LEN;
	if (pt_len > sizeof(pt))
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_AUTH);

	if (hk_hkdf_sha512((const uint8_t *)"Pair-Verify-Encrypt-Salt", 24,
					   v->shared, 32,
					   (const uint8_t *)"Pair-Verify-Encrypt-Info", 24,
					   key, 32) != 0)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_UNKNOWN);

	hk_nonce(nonce, "PV-Msg03");
	if (hk_aead_decrypt(pt, enc + pt_len, key, nonce, aad, aad_len, enc, pt_len) != 0)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_AUTH);

	n = hk_tlv_find(pt, pt_len, HK_TLV_IDENTIFIER, ios_id, sizeof(ios_id));
	if (n <= 0 || (size_t)n > HK_PAIR_ID_LEN_MAX)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_UNKNOWN);
	id_len = (size_t)n;

	n = hk_tlv_find(pt, pt_len, HK_TLV_SIGNATURE, proof, sizeof(proof));
	if (n != HK_ED25519_SIG_LEN)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_UNKNOWN);

	/* 没配过对（或对方不在配对表里）-> 拒绝。这不是"认证失败"那么简单，
	 * 说明对方以为配过而我们不认账。 */
	if (ios_ltpk == NULL)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_AUTH);

	/*
	 * 签名内容 = 控制器临时公钥 || 控制器ID || 我方临时公钥
	 * （注意第一个是 **控制器的** 临时公钥，顺序与 M2 相反）
	 */
	memcpy(material + mlen, v->cpub, 32);
	mlen += 32;
	memcpy(material + mlen, ios_id, id_len);
	mlen += id_len;
	memcpy(material + mlen, v->epub, 32);
	mlen += 32;

	if (hk_ed25519_verify(proof, material, mlen, ios_ltpk) != 0)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_AUTH);

	/* ---- 验证通过：派生会话密钥 ----
	 * 命名容易反：HAP 的 info 串是站在控制器视角写的，所以
	 * "控制器读用的密钥"(Control-Read-Encryption-Key) 是**配件发送**时用的。 */
	if (hk_hkdf_sha512((const uint8_t *)"Control-Salt", 12, v->shared, 32,
					   (const uint8_t *)"Control-Read-Encryption-Key", 27,
					   v->a2c, 32) != 0)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_UNKNOWN);
	if (hk_hkdf_sha512((const uint8_t *)"Control-Salt", 12, v->shared, 32,
					   (const uint8_t *)"Control-Write-Encryption-Key", 28,
					   v->c2a, 32) != 0)
		return hk_fail(resp, resp_len, resp_cap, 4, HK_ERR_UNKNOWN);

	memcpy(v->ios_id, ios_id, id_len);
	v->ios_id_len = id_len;

	if (hk_tlv_put_u8(resp, &p, resp_cap, HK_TLV_STATE, 4) != 0)
	{
		*resp_len = 0;
		return HK_PAIR_ERR_NOMEM;
	}

	*resp_len = p;
	v->state = 4;
	return HK_PAIR_OK;
}

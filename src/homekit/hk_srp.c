/*
 * hk_srp.c —— SRP-6a（3072 位群 + SHA-512）
 *
 * 流程（配件 = 服务端，iPhone = 客户端）：
 *
 *   控制器                              配件
 *   ────────────────────────────────────────────────────────
 *                                       持有 v = g^x mod N（持久化）
 *   a ← 随机, A = g^a mod N
 *                             A ────►
 *                              ◄────     b ← 随机, B = k*v + g^b
 *   u = H(A|B)                            u = H(A|B)
 *   S = (B - k*g^x)^(a+u*x)               S = (A * v^u)^b
 *   K = H(S)                              K = H(S)
 *   M1 = H(...K)  ────►                   校验 M1（配对码对不对）
 *                              ◄────      M2 = H(A|M1|K)
 *
 * 两边算出的 S 必须完全一致 —— 这是整个配对的数学地基。
 * 下面的自测就是把这条链路在 PC 上完整跑一遍。
 */
#include "hk_srp.h"
#include "hk_srp_group.h"
#include "hk_bignum.h"
#include "hk_sha512.h"

#include <string.h>

/*
 * Montgomery 预计算（n0inv / R mod n / R^2 mod n）要跑 6144 次模加倍，
 * 每做一次配对都重算是浪费，所以缓存一次。注意：这里没有加锁，
 * 调用方必须保证 SRP 只在一个线程里用（本工程里只有 HAP 会话线程会用）。
 */
static HK_MONT s_mont;
static int s_mont_ready = 0;

static const HK_MONT *srp_mont(void)
{
	if (!s_mont_ready)
	{
		HK_BN n;

		hk_bn_from_be(&n, HK_SRP_N_BE, HK_SRP_N_BYTES);
		hk_mont_init(&s_mont, &n);
		s_mont_ready = 1;
	}
	return &s_mont;
}

/* z = (a + b) mod n，前提 a,b < n */
static void bn_add_mod(HK_BN *z, const HK_BN *a, const HK_BN *b, const HK_BN *n)
{
	HK_BN t;
	uint64_t carry = 0;
	int i;

	for (i = 0; i < HK_BN_LIMBS; i++)
	{
		uint64_t s = (uint64_t)a->d[i] + b->d[i] + carry;

		t.d[i] = (uint32_t)s;
		carry = s >> 32;
	}

	/*
	 * carry=1 时真实值是 t + 2^3072；因为 a,b < n < 2^3072，所以
	 * a+b < 2n，即真实值 - n < n 且仍在 3072 位内，
	 * 因此截断后的 t - n 就等于 (a+b) mod n。
	 */
	if (carry || hk_bn_cmp(&t, n) >= 0)
		hk_bn_sub_raw(&t, n);

	hk_bn_set(z, &t);
}

/* k = H(N | PAD(g)) */
static void srp_k(uint8_t k[HK_SRP_HASH_LEN])
{
	uint8_t padg[HK_SRP_N_BYTES];
	HK_SHA512_CTX ctx;

	memset(padg, 0, sizeof(padg));
	padg[HK_SRP_N_BYTES - 1] = (uint8_t)HK_SRP_G;

	hk_sha512_init(&ctx);
	hk_sha512_update(&ctx, HK_SRP_N_BE, HK_SRP_N_BYTES);
	hk_sha512_update(&ctx, padg, sizeof(padg));
	hk_sha512_final(&ctx, k);

	memset(padg, 0, sizeof(padg));
}

/* z = (a - b) mod n，前提 a,b < n */
static void bn_sub_mod(HK_BN *z, const HK_BN *a, const HK_BN *b, const HK_BN *n)
{
	HK_BN t;

	if (hk_bn_cmp(a, b) >= 0)
	{
		hk_bn_set(&t, a);
		hk_bn_sub_raw(&t, b);
	}
	else
	{
		/*
		 * a < b，直接减会下溢成巨大的值 —— 而 Montgomery 乘法假定
		 * 输入 < n，一个越界的底数会算出垃圾（配对照样失败，但原因难查）。
		 * 所以走 a + (n - b)：因为 b < n，n-b 无借位；而 a + (n-b) < 2n，
		 * 最多再减一次 n。
		 */
		HK_BN nb;
		uint64_t carry = 0;
		int i;

		hk_bn_set(&nb, n);
		hk_bn_sub_raw(&nb, b); /* nb = n - b */

		for (i = 0; i < HK_BN_LIMBS; i++)
		{
			uint64_t s = (uint64_t)a->d[i] + nb.d[i] + carry;

			t.d[i] = (uint32_t)s;
			carry = s >> 32;
		}
		if (carry || hk_bn_cmp(&t, n) >= 0)
			hk_bn_sub_raw(&t, n);
	}
	hk_bn_set(z, &t);
}

void hk_srp_x(uint8_t x[HK_SRP_HASH_LEN],
			  const uint8_t *salt, size_t salt_len,
			  const uint8_t *user, size_t user_len,
			  const uint8_t *pass, size_t pass_len)
{
	uint8_t inner[HK_SRP_HASH_LEN];
	HK_SHA512_CTX ctx;

	hk_sha512_init(&ctx);
	if (user_len)
		hk_sha512_update(&ctx, user, user_len);
	hk_sha512_update(&ctx, ":", 1);
	if (pass_len)
		hk_sha512_update(&ctx, pass, pass_len);
	hk_sha512_final(&ctx, inner);

	hk_sha512_init(&ctx);
	if (salt_len)
		hk_sha512_update(&ctx, salt, salt_len);
	hk_sha512_update(&ctx, inner, sizeof(inner));
	hk_sha512_final(&ctx, x);

	memset(inner, 0, sizeof(inner));
}

void hk_srp_verifier(uint8_t v[384], const uint8_t x[HK_SRP_HASH_LEN])
{
	const HK_MONT *m = srp_mont();
	HK_BN g, res;

	hk_bn_from_u32(&g, HK_SRP_G);
	hk_mont_exp(&res, &g, x, HK_SRP_HASH_LEN, m);
	hk_bn_to_be(&res, v, HK_SRP_N_BYTES);

	memset(res.d, 0, sizeof(res.d));
}

void hk_srp_client_public(uint8_t A[384], const uint8_t a[HK_SRP_RAND_LEN])
{
	const HK_MONT *m = srp_mont();
	HK_BN g, res;

	hk_bn_from_u32(&g, HK_SRP_G);
	hk_mont_exp(&res, &g, a, HK_SRP_RAND_LEN, m);
	hk_bn_to_be(&res, A, HK_SRP_N_BYTES);

	memset(res.d, 0, sizeof(res.d));
}

void hk_srp_server_public(uint8_t B[384], const uint8_t v[384], const uint8_t b[HK_SRP_RAND_LEN])
{
	const HK_MONT *m = srp_mont();
	HK_BN g, gb, vbn, kbn, kv, sum;
	uint8_t k[HK_SRP_HASH_LEN];

	srp_k(k);

	/* g^b */
	hk_bn_from_u32(&g, HK_SRP_G);
	hk_mont_exp(&gb, &g, b, HK_SRP_RAND_LEN, m);

	/* k*v mod N */
	hk_bn_from_be(&kbn, k, sizeof(k));
	hk_bn_from_be(&vbn, v, HK_SRP_N_BYTES);
	hk_bn_mod_mul(&kv, &kbn, &vbn, m);

	/* B = (k*v + g^b) mod N */
	bn_add_mod(&sum, &kv, &gb, &m->n);
	hk_bn_to_be(&sum, B, HK_SRP_N_BYTES);

	memset(k, 0, sizeof(k));
	memset(kv.d, 0, sizeof(kv.d));
	memset(gb.d, 0, sizeof(gb.d));
}

void hk_srp_u(uint8_t u[HK_SRP_HASH_LEN], const uint8_t A[384], const uint8_t B[384])
{
	HK_SHA512_CTX ctx;

	/* PAD(A) | PAD(B)：我们的 A/B 本来就是 384 字节，已经"pad 过" */
	hk_sha512_init(&ctx);
	hk_sha512_update(&ctx, A, HK_SRP_N_BYTES);
	hk_sha512_update(&ctx, B, HK_SRP_N_BYTES);
	hk_sha512_final(&ctx, u);
}

void hk_srp_server_session(uint8_t S[384], uint8_t K[HK_SRP_HASH_LEN],
						   const uint8_t A[384], const uint8_t v[384],
						   const uint8_t u[HK_SRP_HASH_LEN], const uint8_t b[HK_SRP_RAND_LEN])
{
	const HK_MONT *m = srp_mont();
	HK_BN abn, vbn, vu, base;

	hk_bn_from_be(&abn, A, HK_SRP_N_BYTES);
	hk_bn_from_be(&vbn, v, HK_SRP_N_BYTES);

	hk_mont_exp(&vu, &vbn, u, HK_SRP_HASH_LEN, m);	/* v^u */
	hk_bn_mod_mul(&base, &abn, &vu, m);				/* A * v^u */
	hk_mont_exp(&base, &base, b, HK_SRP_RAND_LEN, m); /* ^b */

	hk_bn_to_be(&base, S, HK_SRP_N_BYTES);
	hk_sha512(S, HK_SRP_N_BYTES, K);

	memset(vu.d, 0, sizeof(vu.d));
	memset(base.d, 0, sizeof(base.d));
}

void hk_srp_client_session(uint8_t S[384], uint8_t K[HK_SRP_HASH_LEN],
						   const uint8_t B[384], const uint8_t x[HK_SRP_HASH_LEN],
						   const uint8_t u[HK_SRP_HASH_LEN], const uint8_t a[HK_SRP_RAND_LEN])
{
	const HK_MONT *m = srp_mont();
	HK_BN g, gx, bbn, kbn, kgx, base;
	HK_BN_BIG ub, xb, ux, ab, exp;
	uint8_t k[HK_SRP_HASH_LEN];
	uint8_t exp_be[HK_SRP_HASH_LEN + HK_SRP_HASH_LEN + HK_SRP_RAND_LEN];
	size_t exp_len;

	srp_k(k);

	/* g^x */
	hk_bn_from_u32(&g, HK_SRP_G);
	hk_mont_exp(&gx, &g, x, HK_SRP_HASH_LEN, m);

	/* base = (B - k*g^x) mod N */
	hk_bn_from_be(&kbn, k, sizeof(k));
	hk_bn_mod_mul(&kgx, &kbn, &gx, m);
	hk_bn_from_be(&bbn, B, HK_SRP_N_BYTES);
	bn_sub_mod(&base, &bbn, &kgx, &m->n);

	/*
	 * 指数是 a + u*x。注意：u 与 x 都是 512 位，乘积 1024 位，
	 * 这是**指数**，绝不能对 N 取模 —— 只能按普通大整数算。
	 */
	hk_bnbig_from_be(&ub, u, HK_SRP_HASH_LEN);
	hk_bnbig_from_be(&xb, x, HK_SRP_HASH_LEN);
	hk_bnbig_mul(&ux, &ub, &xb);
	hk_bnbig_from_be(&ab, a, HK_SRP_RAND_LEN);
	hk_bnbig_add(&exp, &ux, &ab);

	hk_bnbig_to_be(&exp, exp_be, sizeof(exp_be));
	/* 裁掉前导零字节：指数值不变（只是少几次无意义的平方），后面循环更快 */
	{
		size_t off = 0;

		while (off + 1 < sizeof(exp_be) && exp_be[off] == 0)
			off++;
		exp_len = sizeof(exp_be) - off;
		if (off)
			memmove(exp_be, exp_be + off, exp_len);
	}

	hk_mont_exp(&base, &base, exp_be, exp_len, m);

	hk_bn_to_be(&base, S, HK_SRP_N_BYTES);
	hk_sha512(S, HK_SRP_N_BYTES, K);

	memset(k, 0, sizeof(k));
	memset(exp_be, 0, sizeof(exp_be));
	memset(gx.d, 0, sizeof(gx.d));
	memset(kgx.d, 0, sizeof(kgx.d));
	memset(base.d, 0, sizeof(base.d));
}

void hk_srp_m1(uint8_t m1[HK_SRP_HASH_LEN],
			   const uint8_t *user, size_t user_len,
			   const uint8_t *salt, size_t salt_len,
			   const uint8_t A[384], const uint8_t B[384],
			   const uint8_t K[HK_SRP_HASH_LEN])
{
	uint8_t hn[HK_SRP_HASH_LEN], hg[HK_SRP_HASH_LEN];
	uint8_t hx[HK_SRP_HASH_LEN], xr[HK_SRP_HASH_LEN];
	static const uint8_t g_byte = (uint8_t)HK_SRP_G;
	HK_SHA512_CTX ctx;
	int i;

	hk_sha512(HK_SRP_N_BE, HK_SRP_N_BYTES, hn);

	/*
	 * H(g) 用 g 的**最短大端表示**（g=5 即单字节 0x05），不是补零到 N 的长度。
	 * 这是 HAP 生态各参考实现（fast-srp / HAP-NodeJS / esp-homekit）的约定；
	 * 补零版 PAD(g) 只用在 srp_k() 的 k = H(N | PAD(g)) 里。
	 * 若这里也用补零版，配对时 iOS 会报 M1 校验失败（码 2/未知错误）。
	 */
	hk_sha512((const uint8_t *)&g_byte, sizeof(g_byte), hg);

	/* H(N) XOR H(g)：注意两者长度相同，逐字节异或即可 */
	for (i = 0; i < HK_SRP_HASH_LEN; i++)
		xr[i] = (uint8_t)(hn[i] ^ hg[i]);

	hk_sha512(user, user_len, hx);

	hk_sha512_init(&ctx);
	hk_sha512_update(&ctx, xr, sizeof(xr));
	hk_sha512_update(&ctx, hx, sizeof(hx));
	if (salt_len)
		hk_sha512_update(&ctx, salt, salt_len);
	hk_sha512_update(&ctx, A, HK_SRP_N_BYTES);
	hk_sha512_update(&ctx, B, HK_SRP_N_BYTES);
	hk_sha512_update(&ctx, K, HK_SRP_HASH_LEN);
	hk_sha512_final(&ctx, m1);

	memset(hn, 0, sizeof(hn));
	memset(hg, 0, sizeof(hg));
	memset(hx, 0, sizeof(hx));
	memset(xr, 0, sizeof(xr));
}

void hk_srp_m2(uint8_t m2[HK_SRP_HASH_LEN],
			   const uint8_t A[384], const uint8_t m1[HK_SRP_HASH_LEN],
			   const uint8_t K[HK_SRP_HASH_LEN])
{
	HK_SHA512_CTX ctx;

	hk_sha512_init(&ctx);
	hk_sha512_update(&ctx, A, HK_SRP_N_BYTES);
	hk_sha512_update(&ctx, m1, HK_SRP_HASH_LEN);
	hk_sha512_update(&ctx, K, HK_SRP_HASH_LEN);
	hk_sha512_final(&ctx, m2);
}

int hk_srp_ct_eq(const uint8_t *a, const uint8_t *b, size_t len)
{
	uint32_t d = 0;
	size_t i;

	for (i = 0; i < len; i++)
		d |= (uint32_t)(a[i] ^ b[i]);
	/* 把差异折成 0/1，相等返回 1 */
	return ((d | (0u - d)) >> 31) == 0;
}

int hk_srp_public_ok(const uint8_t pub[384])
{
	HK_BN t, n;
	int i;
	uint32_t acc = 0;

	for (i = 0; i < HK_SRP_N_BYTES; i++)
		acc |= pub[i];
	if (acc == 0)
		return 0; /* 全零 */

	hk_bn_from_be(&t, pub, HK_SRP_N_BYTES);
	hk_bn_from_be(&n, HK_SRP_N_BE, HK_SRP_N_BYTES);

	/* 只接受 1 < pub < N-1 */
	if (hk_bn_cmp(&t, &n) >= 0)
		return 0;
	{
		HK_BN one;

		hk_bn_from_u32(&one, 1);
		if (hk_bn_cmp(&t, &one) <= 0)
			return 0;
	}
	return 1;
}

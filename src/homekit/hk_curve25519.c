/*
 * hk_curve25519.c —— X25519（RFC 7748 §5 实现）
 *
 * 主循环严格照 RFC 7748 §5 的伪码写（Montgomery 阶梯 + 常数时间条件交换），
 * 这样每个变量的含义都能和 RFC 逐行对上，出了错不用猜。
 *
 * 域运算在 hk_gf.c（与 Ed25519 共用，省 flash）。
 * 这里只负责"阶梯"本身 —— 也就是把标量比特一位位喂进曲线运算。
 */
#include "hk_curve25519.h"
#include "hk_gf.h"
#include <string.h>

static const uint8_t HK_BASE_POINT[32] = {9};

int hk_x25519(uint8_t out[HK_X25519_LEN],
			  const uint8_t scalar[HK_X25519_LEN],
			  const uint8_t point[HK_X25519_LEN])
{
	uint8_t k[HK_X25519_LEN];
	hk_gf x1, x2, z2, x3, z3;
	hk_gf a, aa, b, bb, e, c, d, da, cb, t0, t1;
	int pos, swap;

	/* decodeScalar25519：清低 3 位、清最高位、置次高位（RFC 7748 §5） */
	memcpy(k, scalar, HK_X25519_LEN);
	k[0] &= 248;
	k[31] &= 127;
	k[31] |= 64;

	hk_gf_unpack(x1, point);

	/* 阶梯初值：x2=1, z2=0, x3=x1, z3=1 */
	hk_gf_set(x2, hk_gf1);
	memset(z2, 0, sizeof(hk_gf));
	hk_gf_set(x3, x1);
	hk_gf_set(z3, hk_gf1);

	swap = 0;
	for (pos = 254; pos >= 0; pos--)
	{
		int kt = (k[pos >> 3] >> (pos & 7)) & 1;

		swap ^= kt;
		hk_gf_cswap(x2, x3, swap);
		hk_gf_cswap(z2, z3, swap);
		swap = kt;

		hk_gf_add(a, x2, z2);	/* a  = x2 + z2 */
		hk_gf_sqr(aa, a);		/* aa = a^2 */
		hk_gf_sub(b, x2, z2);	/* b  = x2 - z2 */
		hk_gf_sqr(bb, b);		/* bb = b^2 */
		hk_gf_sub(e, aa, bb);	/* e  = aa - bb */
		hk_gf_add(c, x3, z3);	/* c  = x3 + z3 */
		hk_gf_sub(d, x3, z3);	/* d  = x3 - z3 */
		hk_gf_mul(da, d, a);	/* da = d  * a */
		hk_gf_mul(cb, c, b);	/* cb = c  * b */

		/* 先算 z3 再算 x3：两者都要用 da/cb，顺序不敏感；
		 * 但必须都用「旧的 da/cb」，所以先落 t0 再覆盖。 */
		hk_gf_sub(t0, da, cb); /* t0 = da - cb */
		hk_gf_sqr(t0, t0);
		hk_gf_mul(z3, x1, t0); /* z3 = x1 * (da - cb)^2 */

		hk_gf_add(t0, da, cb);
		hk_gf_sqr(x3, t0);	    /* x3 = (da + cb)^2 */

		hk_gf_mul(x2, aa, bb);	   /* x2 = aa * bb */
		hk_gf_mul(t0, hk_gf121665, e); /* t0 = a24 * e */
		hk_gf_add(t1, aa, t0);	   /* t1 = aa + a24*e */
		hk_gf_mul(z2, e, t1);	   /* z2 = e * (aa + a24*e) */
	}

	hk_gf_cswap(x2, x3, swap);
	hk_gf_cswap(z2, z3, swap);

	hk_gf_inv(z2, z2);
	hk_gf_mul(x2, x2, z2);
	hk_gf_pack(out, x2);

	/* 标量与中间态都含私钥信息，用完擦掉 */
	memset(k, 0, sizeof(k));
	memset(a, 0, sizeof(a));
	memset(aa, 0, sizeof(aa));
	memset(b, 0, sizeof(b));
	memset(bb, 0, sizeof(bb));
	memset(e, 0, sizeof(e));
	memset(c, 0, sizeof(c));
	memset(d, 0, sizeof(d));
	memset(da, 0, sizeof(da));
	memset(cb, 0, sizeof(cb));
	memset(t0, 0, sizeof(t0));
	memset(t1, 0, sizeof(t1));
	memset(x2, 0, sizeof(x2));
	memset(z2, 0, sizeof(z2));
	memset(x3, 0, sizeof(x3));
	memset(z3, 0, sizeof(z3));
	return 0;
}

int hk_x25519_base(uint8_t out[HK_X25519_LEN], const uint8_t scalar[HK_X25519_LEN])
{
	return hk_x25519(out, scalar, HK_BASE_POINT);
}

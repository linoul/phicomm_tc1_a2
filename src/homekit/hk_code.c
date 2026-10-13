/*
 * hk_code.c —— 配对码的校验、归一化与随机生成（规则见 hk_code.h）
 */
#include "hk_code.h"

#include <string.h>

/* ------------------------------------------------------------------
 * "XXX-XX-XXX" 里 8 个数字分别落在下标 0,1,2,4,5,7,8,9 ——
 * 两个连字符在 3 和 6。别用 "i + (i>=3)" 这类算术去推，推一个错一个：
 * 第二个连字符会把后半段的映射整体挪歪。
 */
static const uint8_t k_digit_off[8] = {0, 1, 2, 4, 5, 7, 8, 9};

/* ------------------------------------------------------------------ */

int hk_code_valid(const char *code)
{
	size_t len;
	int i;
	int first;
	int same = 1;

	if (code == NULL)
		return 0;

	len = strlen(code);

	if (len == (size_t)HK_CODE_LEN)
	{
		/* 带连字符的写法：先把两个连字符的位置也验了 */
		if (code[3] != '-' || code[6] != '-')
			return 0;
		for (i = 0; i < 8; i++)
		{
			char c = code[k_digit_off[i]];
			if (c < '0' || c > '9')
				return 0;
		}
		first = code[0];
	}
	else if (len == 8)
	{
		for (i = 0; i < 8; i++)
			if (code[i] < '0' || code[i] > '9')
				return 0;
		first = code[0];
	}
	else
		return 0;

	/* 规则 1：首位不能是 0 或 1 */
	if (first < '2' || first > '9')
		return 0;

	/* 规则 2：八位全同的码禁用 */
	for (i = 1; i < 8; i++)
	{
		char a = (len == (size_t)HK_CODE_LEN) ? code[k_digit_off[i]] : code[i];
		if (a != first)
		{
			same = 0;
			break;
		}
	}
	if (same)
		return 0;

	/* 规则 3：两个顺子禁用（两种写法都堵住） */
	if (strcmp(code, "123-45-678") == 0 || strcmp(code, "876-54-321") == 0)
		return 0;
	if (strcmp(code, "12345678") == 0 || strcmp(code, "87654321") == 0)
		return 0;

	return 1;
}

int hk_code_normalize(const char *in, char *out, size_t cap)
{
	size_t i;
	int n = 0;
	char d[8];

	if (in == NULL || out == NULL || cap < (size_t)HK_CODE_MAX)
		return -1;

	for (i = 0; in[i] != '\0' && n < 8; i++)
	{
		if (in[i] >= '0' && in[i] <= '9')
			d[n++] = in[i];
	}
	if (n < 8)
		return -1;

	out[0] = d[0];
	out[1] = d[1];
	out[2] = d[2];
	out[3] = '-';
	out[4] = d[3];
	out[5] = d[4];
	out[6] = '-';
	out[7] = d[5];
	out[8] = d[6];
	out[9] = d[7];
	out[10] = '\0';
	return 0;
}

/* 拒绝采样取一个 [0,9] 的数字 */
static int rand_digit(void (*rand)(uint8_t *buf, size_t n, void *ctx), void *ctx)
{
	uint8_t b;
	int guard;

	if (rand == NULL)
		return -1;

	/* 250 是 <= 256 的最大 10 的倍数；>=250 的字节丢弃重取，保证均匀 */
	for (guard = 0; guard < 64; guard++)
	{
		rand(&b, 1, ctx);
		if (b < 250)
			return b % 10;
	}
	return -1;
}

/* 拒绝采样取一个 [2,9] 的数字（首位用） */
static int rand_first(void (*rand)(uint8_t *buf, size_t n, void *ctx), void *ctx)
{
	uint8_t b;
	int guard;

	if (rand == NULL)
		return -1;

	/* 240 是 <= 256 的最大 8 的倍数 */
	for (guard = 0; guard < 64; guard++)
	{
		rand(&b, 1, ctx);
		if (b < 240)
			return 2 + (b % 8);
	}
	return -1;
}

int hk_code_random(char *out, size_t cap,
				   void (*rand)(uint8_t *buf, size_t n, void *ctx), void *ctx)
{
	int attempt;

	if (out == NULL || cap < (size_t)HK_CODE_MAX || rand == NULL)
		return -1;

	/*
	 * 最多试 16 轮。命中禁用码的概率极低（全同码 10 个、顺子 2 个，
	 * 共 12 个 / 8×10^7 种），连续 16 轮都中说明随机源有问题。
	 */
	for (attempt = 0; attempt < 16; attempt++)
	{
		int i;
		int v;

		/* 先把两个连字符摆好，再逐个填数字位 */
		out[3] = '-';
		out[6] = '-';
		out[10] = '\0';

		v = rand_first(rand, ctx);
		if (v < 2 || v > 9)
			return -2;
		out[k_digit_off[0]] = (char)('0' + v);

		for (i = 1; i < 8; i++)
		{
			v = rand_digit(rand, ctx);
			if (v < 0)
				return -2;
			out[k_digit_off[i]] = (char)('0' + v);
		}

		if (hk_code_valid(out))
			return 0;
	}

	out[0] = '\0';
	return -2;
}

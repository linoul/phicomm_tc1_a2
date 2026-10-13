/*
 * hk_tlv.c —— HAP 的 TLV8 编解码
 *
 * 说明与设计取舍见 hk_tlv.h。这里只补充三条实现上的约定：
 *
 *   1) 分片合并规则：**连续**的同 type 条目才合并。中间被别的 type 隔开
 *      的同 type 值视为两条独立条目（HAP 里只有 identifier 可能出现两次，
 *      而且从不跨类型穿插）。
 *   2) 所有写入函数都是"原子"的：空间不够就一点都不写。上层经常是
 *      "构造成功响应失败 -> 改成构造错误响应"，半截的 buf 会污染后者。
 *   3) 不动态分配。配对阶段的缓冲是一次性分配好的静态数组。
 */
#include "hk_tlv.h"

#include <string.h>

void hk_tlv_iter_init(HK_TLV_ITER *it, const uint8_t *buf, size_t len)
{
	it->buf = buf;
	it->len = len;
	it->pos = 0;
}

int hk_tlv_iter_next(HK_TLV_ITER *it, uint8_t *type, const uint8_t **val, size_t *vlen)
{
	size_t l;

	if (it == NULL || it->buf == NULL)
		return 0;

	/* 剩余不足 2 字节：要么正常结束，要么是残尾。
	 * 残尾必须报错 —— 静默吞掉会让上层拿一个"看起来合法"的错误结果。 */
	if (it->pos == it->len)
		return 0;
	if (it->pos + 2 > it->len)
		return -1; /* 只有 type 没有 length */

	l = it->buf[it->pos + 1];
	if (it->pos + 2 + l > it->len)
		return -1; /* length 声称的长度超出消息边界 */

	if (type)
		*type = it->buf[it->pos];
	if (val)
		*val = it->buf + it->pos + 2;
	if (vlen)
		*vlen = l;

	it->pos += 2 + l;
	return 1;
}

int hk_tlv_validate(const uint8_t *buf, size_t len)
{
	HK_TLV_ITER it;
	uint8_t t;
	const uint8_t *v;
	size_t vl;
	int rc;

	hk_tlv_iter_init(&it, buf, len);
	while ((rc = hk_tlv_iter_next(&it, &t, &v, &vl)) == 1)
	{
		(void)t;
		(void)v;
		(void)vl;
	}
	return (rc < 0) ? -1 : 0;
}

int hk_tlv_find_len(const uint8_t *buf, size_t len, uint8_t type)
{
	HK_TLV_ITER it;
	uint8_t t;
	const uint8_t *v;
	size_t vl;
	size_t total = 0;
	int found = 0; /* 已经命中目标 type */
	int rc;

	hk_tlv_iter_init(&it, buf, len);
	while ((rc = hk_tlv_iter_next(&it, &t, &v, &vl)) == 1)
	{
		(void)v;
		if (t == type)
		{
			if (!found)
				found = 1;
			total += vl;
		}
		else if (found)
		{
			break; /* 连续分片结束：后面再有同 type 也不属于第一个值 */
		}
	}
	if (rc < 0)
		return -1;
	if (!found)
		return 0;
	if (total > 0x7fffffffu)
		return -1;
	return (int)total;
}

int hk_tlv_find(const uint8_t *buf, size_t len, uint8_t type, uint8_t *out, size_t out_cap)
{
	HK_TLV_ITER it;
	uint8_t t;
	const uint8_t *v;
	size_t vl;
	size_t off = 0;
	int found = 0;
	int rc;
	int need;

	if (out == NULL || out_cap == 0)
		return -1;

	/* 先扫一遍拿总长度：这样"装不下"时能一点都不写（原子性） */
	need = hk_tlv_find_len(buf, len, type);
	if (need < 0)
		return -1;
	if (need == 0)
		return 0;
	if ((size_t)need > out_cap)
		return -1;

	hk_tlv_iter_init(&it, buf, len);
	while ((rc = hk_tlv_iter_next(&it, &t, &v, &vl)) == 1)
	{
		if (t == type)
		{
			if (!found)
				found = 1;
			memcpy(out + off, v, vl);
			off += vl;
		}
		else if (found)
		{
			break;
		}
	}
	if (rc < 0)
		return -1;
	return (int)off;
}

int hk_tlv_put(uint8_t *buf, size_t *pos, size_t cap,
			   uint8_t type, const uint8_t *val, size_t vlen)
{
	size_t frags, need, off = 0;

	if (buf == NULL || pos == NULL)
		return -1;
	if (*pos > cap)
		return -1;

	if (vlen == 0)
	{
		/* 空值条目：仍然要写，因为"有这个字段但为空"和"没这个字段"语义不同 */
		if (*pos + 2 > cap)
			return -1;
		buf[(*pos)++] = type;
		buf[(*pos)++] = 0;
		return 0;
	}
	if (val == NULL)
		return -1;

	/* 片数 = ceil(vlen / 255)，每片 2 字节头 */
	frags = (vlen + HK_TLV_MAX_FRAGMENT - 1) / HK_TLV_MAX_FRAGMENT;
	need = vlen + 2 * frags;
	if (need > cap - *pos) /* 这么写避免 *pos + need 溢出 */
		return -1;

	while (off < vlen)
	{
		size_t n = vlen - off;
		if (n > HK_TLV_MAX_FRAGMENT)
			n = HK_TLV_MAX_FRAGMENT;
		buf[(*pos)++] = type;
		buf[(*pos)++] = (uint8_t)n;
		memcpy(buf + *pos, val + off, n);
		*pos += n;
		off += n;
	}
	return 0;
}

int hk_tlv_put_u8(uint8_t *buf, size_t *pos, size_t cap, uint8_t type, uint8_t val)
{
	return hk_tlv_put(buf, pos, cap, type, &val, 1);
}

int hk_tlv_put_error(uint8_t *buf, size_t *pos, size_t cap, uint8_t state, uint8_t err)
{
	/* 先在临时区里拼好，再整体搬过去，保证"要么全写要么不写"。
	 * 两条各占 3 字节，所以临时区要 6 字节（写成 4 会在第二步就失败）。 */
	uint8_t tmp[6];
	size_t p = 0;

	if (buf == NULL || pos == NULL || *pos > cap)
		return -1;

	/* 先在临时区里拼好，再整体搬过去，保证"要么全写要么不写" */
	if (hk_tlv_put_u8(tmp, &p, sizeof(tmp), HK_TLV_STATE, state) != 0)
		return -1;
	if (hk_tlv_put_u8(tmp, &p, sizeof(tmp), HK_TLV_ERROR, err) != 0)
		return -1;
	if (p > cap - *pos)
		return -1;

	memcpy(buf + *pos, tmp, p);
	*pos += p;
	return 0;
}

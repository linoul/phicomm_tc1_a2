/*
 * hk_store.c —— HomeKit 配对信息的落盘格式（纯 C99）
 *
 * 落盘布局（小端，逐字节拼，不依赖平台字节序）：
 *
 *   off  0 : magic   u32   "HKIT"
 *   off  4 : version u16
 *   off  6 : blob_len u16  （含末尾 CRC 的整段长度）
 *   off  8 : config_number u32
 *   off 12 : npeers  u8
 *   off 13 : reserved u8[3]
 *   off 16 : ltsk[32]
 *   off 48 : ltpk[32]
 *   off 80 : peers[npeers] —— 每条 36 字节 ID + 32 字节 LTPK
 *   之后   : setup_code[16] —— 配对码 "XXX-XX-XXX"，'\0' 补齐
 *   末尾   : crc32 u32     —— 覆盖 0 .. blob_len-5
 *
 * 版本 2 相对版本 1 的变化：在 peers 之后加了 16 字节的 setup_code。
 * 放在末尾而不是 header 里，是为了不打乱已有字段的偏移 —— 反正
 * 版本号会变，旧数据一律按"没配过"处理，不必做字段级兼容。
 *
 * 为什么 CRC 要盖住 magic：magic 本身就是数据的一部分。只盖"payload"
 * 的话，一个被写坏的 magic 会先通过 magic 检查、再被当成有效配置。
 */
#include "hk_store.h"

#include <string.h>

#include "hk_ed25519.h"

/* ------------------------------------------------------------------ */
static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */

uint32_t hk_store_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	size_t i;
	int b;

	if (data == NULL)
		return crc;

	for (i = 0; i < len; i++)
	{
		crc ^= data[i];
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
	}
	return crc ^ 0xFFFFFFFFu;
}

void hk_store_clear(HK_STORE *s)
{
	if (s == NULL)
		return;
	memset(s, 0, sizeof(*s));
	s->valid = 0;
	s->config_number = 1; /* HAP 的 c# 从 1 起 */
}

int hk_store_ensure_identity(HK_STORE *s,
							 void (*rand)(uint8_t *buf, size_t n, void *ctx),
							 void *ctx)
{
	if (s == NULL || rand == NULL)
		return -1;

	if (s->valid)
		return 0;

	/* 只生成一次：LTSK 丢了 iOS 那边就彻底连不上，必须落盘保存 */
	rand(s->ltsk, sizeof(s->ltsk), ctx);
	if (hk_ed25519_pubkey(s->ltpk, s->ltsk) != 0)
		return -1;
	return 0;
}

const uint8_t *hk_store_peer_find(const HK_STORE *s, const char *id)
{
	uint8_t i;

	if (s == NULL || id == NULL)
		return NULL;

	for (i = 0; i < s->npeers; i++)
	{
		if (strncmp(s->peers[i].id, id, HK_STORE_ID_LEN) == 0)
			return s->peers[i].ltpk;
	}
	return NULL;
}

int hk_store_peer_add(HK_STORE *s, const char *id, const uint8_t ltpk[32])
{
	uint8_t i;
	size_t n;

	if (s == NULL || id == NULL || ltpk == NULL)
		return -2;

	n = strlen(id);
	if (n == 0 || n > HK_STORE_ID_LEN)
		return -2;

	/* 同 ID 重配对：覆盖公钥，不算新增 */
	for (i = 0; i < s->npeers; i++)
	{
		if (strncmp(s->peers[i].id, id, HK_STORE_ID_LEN) == 0)
		{
			memcpy(s->peers[i].ltpk, ltpk, 32);
			return 0;
		}
	}

	if (s->npeers >= HK_STORE_MAX_PEERS)
		return -1;

	memset(&s->peers[s->npeers], 0, sizeof(s->peers[s->npeers]));
	memcpy(s->peers[s->npeers].id, id, n);
	s->peers[s->npeers].id[n] = '\0';
	memcpy(s->peers[s->npeers].ltpk, ltpk, 32);
	s->npeers++;
	s->config_number++;
	return 0;
}

int hk_store_peer_del(HK_STORE *s, const char *id)
{
	uint8_t i;

	if (s == NULL || id == NULL)
		return -2;

	for (i = 0; i < s->npeers; i++)
	{
		if (strncmp(s->peers[i].id, id, HK_STORE_ID_LEN) == 0)
		{
			/* 保持数组紧凑：后面整体前移一格 */
			if (i + 1 < s->npeers)
				memmove(&s->peers[i], &s->peers[i + 1],
						sizeof(s->peers[i]) * (size_t)(s->npeers - i - 1));
			s->npeers--;
			memset(&s->peers[s->npeers], 0, sizeof(s->peers[s->npeers]));
			s->config_number++;
			return 0;
		}
	}
	return -1;
}

int hk_store_serialize(const HK_STORE *s, uint8_t *out, size_t cap,
					   size_t *out_len)
{
	size_t p = 0;
	size_t total;
	uint8_t i;

	if (s == NULL || out == NULL)
		return -1;

	if (s->npeers > HK_STORE_MAX_PEERS)
		return -1;

	total = HK_STORE_HDR_LEN + HK_STORE_KEY_LEN +
			(size_t)s->npeers * HK_STORE_PEER_LEN +
			HK_STORE_CODE_LEN + HK_STORE_CRC_LEN;
	if (total > cap || total > 0xFFFFu)
		return -1;

	put_u32(out + p, HK_STORE_MAGIC);
	p += 4;
	put_u16(out + p, (uint16_t)HK_STORE_VERSION);
	p += 2;
	put_u16(out + p, (uint16_t)total);
	p += 2;
	put_u32(out + p, s->config_number);
	p += 4;
	out[p++] = s->npeers;
	out[p++] = 0;
	out[p++] = 0;
	out[p++] = 0;

	memcpy(out + p, s->ltsk, 32);
	p += 32;
	memcpy(out + p, s->ltpk, 32);
	p += 32;

	for (i = 0; i < s->npeers; i++)
	{
		memset(out + p, 0, HK_STORE_ID_LEN);
		memcpy(out + p, s->peers[i].id, strlen(s->peers[i].id));
		p += HK_STORE_ID_LEN;
		memcpy(out + p, s->peers[i].ltpk, 32);
		p += 32;
	}

	/* 配对码：整段先清零再拷，保证尾部一定是 '\0' 而不是 flash 里的垃圾 */
	memset(out + p, 0, HK_STORE_CODE_LEN);
	strncpy((char *)(out + p), s->setup_code, HK_STORE_CODE_LEN - 1);
	p += HK_STORE_CODE_LEN;

	put_u32(out + p, hk_store_crc32(out, p));
	p += 4;

	if (out_len != NULL)
		*out_len = p;
	return 0;
}

int hk_store_deserialize(const uint8_t *in, size_t len, HK_STORE *s)
{
	size_t p = 0;
	size_t total;
	uint16_t ver, blob_len;
	uint8_t npeers;
	uint32_t crc_got, crc_calc;
	uint8_t i;
	HK_STORE tmp;

	if (in == NULL || s == NULL)
		return -1;

	if (len < HK_STORE_HDR_LEN + HK_STORE_KEY_LEN + HK_STORE_CODE_LEN + HK_STORE_CRC_LEN)
		return -1;

	if (get_u32(in) != HK_STORE_MAGIC)
		return -1;
	p += 4;

	ver = get_u16(in + p);
	p += 2;
	if (ver != HK_STORE_VERSION)
		return -1;

	blob_len = get_u16(in + p);
	p += 2;
	if (blob_len < HK_STORE_HDR_LEN + HK_STORE_KEY_LEN + HK_STORE_CODE_LEN + HK_STORE_CRC_LEN ||
		blob_len > len)
		return -1;

	/* 长度必须和 npeers 自洽，否则后面的字段会读飞 */
	npeers = in[12];
	if (npeers > HK_STORE_MAX_PEERS)
		return -1;
	total = HK_STORE_HDR_LEN + HK_STORE_KEY_LEN +
			(size_t)npeers * HK_STORE_PEER_LEN +
			HK_STORE_CODE_LEN + HK_STORE_CRC_LEN;
	if (total != blob_len)
		return -1;

	crc_got = get_u32(in + blob_len - HK_STORE_CRC_LEN);
	crc_calc = hk_store_crc32(in, blob_len - HK_STORE_CRC_LEN);
	if (crc_got != crc_calc)
		return -1;

	/* 全部通过才落进 s —— 中途失败时 s 保持原样 */
	memset(&tmp, 0, sizeof(tmp));
	p = 8; /* config_number */
	tmp.config_number = get_u32(in + p);
	p += 4;
	tmp.npeers = in[p]; /* 12 */
	p += 4;				/* npeers(1) + reserved(3) -> 16 */

	memcpy(tmp.ltsk, in + p, 32);
	p += 32;
	memcpy(tmp.ltpk, in + p, 32);
	p += 32;

	for (i = 0; i < npeers; i++)
	{
		memcpy(tmp.peers[i].id, in + p, HK_STORE_ID_LEN);
		tmp.peers[i].id[HK_STORE_ID_LEN] = '\0';
		p += HK_STORE_ID_LEN;
		memcpy(tmp.peers[i].ltpk, in + p, 32);
		p += 32;
	}

	/* 读回配对码。memset 已经把 tmp 清零了，这里只拷 15 字节，
	 * 第 16 字节恒为 '\0' —— 即便 flash 里存的是没结尾的垃圾也不会溢出。 */
	memcpy(tmp.setup_code, in + p, HK_STORE_CODE_LEN - 1);
	tmp.setup_code[HK_STORE_CODE_LEN - 1] = '\0';
	p += HK_STORE_CODE_LEN;

	tmp.valid = 1;
	*s = tmp;
	return 0;
}

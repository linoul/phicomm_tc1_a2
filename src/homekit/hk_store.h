/*
 * hk_store.h —— HomeKit 配对信息的持久化（纯 C99，不碰 SDK，可在主机自测）
 *
 * 要存三样东西：
 *   1) 配件的长期身份 LTSK/LTPK（Ed25519）。**这个绝不能丢** ——
 *      丢了的话 iOS 那边存的还是旧公钥，下次连接会直接验签失败，
 *      表现为"重启后排插在家庭 App 里变成无响应"，只能删除配件重配。
 *   2) 已配对控制器的列表（iOS 的 pairing ID + 它的长期公钥）。
 *   3) 配置号 c#。HAP 要求配件结构变化时必须递增，否则 iOS 不重新拉
 *      /accessories。本工程结构固定，只在"新增/删除配对"时递增。
 *
 * 三个容易踩的点：
 *
 *   1) **落盘格式必须字节级自描述**：magic + 版本 + 自身长度 + CRC32。
 *      只写个裸结构体，将来结构体一改（或者读到半截掉电写坏的数据）
 *      就会把垃圾当配置用 —— 而且 HomeKit 这种"身份错了就彻底连不上"
 *      的场景，排查起来极其痛苦。
 *   2) **CRC 要覆盖包括 magic 在内的整段**，且校验失败要当成"没配过"
 *      而不是"报错退出" —— 掉电写坏是常态，设备必须能自愈。
 *   3) **本层不碰 flash**。序列化和反序列化是纯函数，读写由第三层的
 *      hk_net.c 调 hffile_userbin_read/write。这样自测能在 PC 上跑，
 *      也不用为了测存储去模拟整个 SDK。
 *
 * 字节序：全部按小端逐字节拼（ARM 是小端，但这样写不依赖平台）。
 */
#ifndef HK_STORE_H
#define HK_STORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HK_STORE_MAGIC 0x484B4954u /* "HKIT" */
#define HK_STORE_VERSION 2

/* iOS 的 pairing ID 是 36 字节 UUID 字符串，留 1 字节给 '\0' */
#define HK_STORE_ID_LEN 36
#define HK_STORE_ID_BUF 37

/* 配对码 "XXX-XX-XXX"。给 16 是为了让整段保持 4 字节对齐，
 * 也让将来万一改成不带连字符的 8 位码不用改布局。 */
#define HK_STORE_CODE_LEN 16

/* 最多记住几个控制器。家庭里一般 1~2 个，5 个足够 */
#define HK_STORE_MAX_PEERS 5

#define HK_STORE_HDR_LEN 16 /* magic4 + ver2 + len2 + c# 4 + npeers1 + rsv3 */
#define HK_STORE_KEY_LEN 64 /* ltsk 32 + ltpk 32 */
#define HK_STORE_PEER_LEN 68 /* id 36 + ltpk 32 */
#define HK_STORE_CRC_LEN 4

#define HK_STORE_BLOB_MAX \
	(HK_STORE_HDR_LEN + HK_STORE_KEY_LEN + \
	 HK_STORE_MAX_PEERS * HK_STORE_PEER_LEN + \
	 HK_STORE_CODE_LEN + HK_STORE_CRC_LEN)

typedef struct _HK_PEER
{
	char id[HK_STORE_ID_BUF]; /* '\0' 结尾的 UUID 字符串 */
	uint8_t ltpk[32];
} HK_PEER;

typedef struct _HK_STORE
{
	int valid;				/* 1 = 内容可用（自检通过） */
	uint32_t config_number; /* c#，结构变化时递增 */
	uint8_t ltsk[32];
	uint8_t ltpk[32];
	uint8_t npeers;
	HK_PEER peers[HK_STORE_MAX_PEERS];
	char setup_code[HK_STORE_CODE_LEN]; /* "XXX-XX-XXX"，'\0' 结尾；空串=未设定 */
} HK_STORE;

/* 清空成"未配过、无身份"的初始状态 */
void hk_store_clear(HK_STORE *s);

/*
 * 保证配件身份存在。若 s 里已有身份则原样返回；
 * 否则用 rand 生成 32 字节 LTSK 并导出 LTPK。
 *   rand: 随机源（固件侧填 TRNG，测试填固定值）
 * 返回 0；rand 为 NULL 或生成失败返回 -1。
 */
int hk_store_ensure_identity(HK_STORE *s,
							 void (*rand)(uint8_t *buf, size_t n, void *ctx),
							 void *ctx);

/* 按 ID 找控制器；找到返回其 LTPK，否则返回 NULL */
const uint8_t *hk_store_peer_find(const HK_STORE *s, const char *id);

/*
 * 新增一个控制器。已存在同 ID 则**覆盖**它的公钥（iOS 重配对时就是这样）。
 * 满了返回 -1，参数非法返回 -2。成功会递增 c# 并返回 0。
 */
int hk_store_peer_add(HK_STORE *s, const char *id, const uint8_t ltpk[32]);

/* 删除一个控制器。不存在返回 -1；成功递增 c# 并返回 0。 */
int hk_store_peer_del(HK_STORE *s, const char *id);

/* 序列化。out_len 返回写入的字节数（不含 cap 检查之外的内容）。
 * 返回 0 成功，-1 参数错/装不下。 */
int hk_store_serialize(const HK_STORE *s, uint8_t *out, size_t cap,
					   size_t *out_len);

/* 反序列化。任何一步不对（magic/版本/长度/CRC/npeers 越界）都返回 -1，
 * 并且 **s 保持不变** —— 调用方可以安全地接着用旧内容或走 hk_store_clear()。
 * 返回 0 成功。 */
int hk_store_deserialize(const uint8_t *in, size_t len, HK_STORE *s);

/* CRC32（与 Python zlib.crc32 同算法：poly 0xEDB88320，init/xorout 0xFFFFFFFF） */
uint32_t hk_store_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HK_STORE_H */

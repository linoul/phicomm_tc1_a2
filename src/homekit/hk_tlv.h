/*
 * hk_tlv.h —— HAP 的 TLV8 编解码
 *
 * TLV8 是 HomeKit 配对阶段的"信封"：每个条目是 type(1) + length(1) + value。
 * 看起来简单，但有两个坑会在真机上变成"iOS 默默配对失败"：
 *
 *   1) length 只有 1 字节，所以**超过 255 字节的值必须切成多片**，
 *      解码时再把"连续的同 type 条目"拼回去。SRP 的 A/B 公钥是 384 字节，
 *      必然要分片 —— 这一层写错，表现就是 M3 一发过去就没下文了。
 *   2) 收到的数据是网络来的，**每条长度都要做边界检查**。越界读在 MCU 上
 *      不会崩，只会读到上一帧的残留，然后算出个"看起来合法"的错误结果。
 *
 * 这一份同样只依赖 C99 标准库，不碰 SDK，所以能在 PC 上跑自测。
 */
#ifndef HK_TLV_H
#define HK_TLV_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* TLV8 条目类型（HAP 规范 Table 5-6 / 5-12） */
#define HK_TLV_METHOD 0x00		 /* 1 字节：见下面 HK_METHOD_* */
#define HK_TLV_IDENTIFIER 0x01	 /* 36 字节 ASCII：配对标识（设备 ID）。HAP 里也叫 USERNAME */
#define HK_TLV_SALT 0x02		 /* 16 字节：SRP salt */
#define HK_TLV_PUBLICKEY 0x03	 /* SRP: 384 字节；Verify: 32 字节 */
#define HK_TLV_PASSWORD_PROOF 0x04 /* 64 字节：SRP 的 M1/M2（仅 Pair Setup M3/M4） */
#define HK_TLV_ENCRYPTEDDATA 0x05 /* AEAD 密文 + 16 字节 tag */
#define HK_TLV_STATE 0x06		 /* 1 字节：M1..M6（HAP 里也叫 SEQUENCE_NUM） */
#define HK_TLV_ERROR 0x07		 /* 1 字节：见下面 HK_ERR_* */
#define HK_TLV_RETRYDELAY 0x08	 /* 4 字节：繁忙时让控制器等多少秒 */
#define HK_TLV_CERTIFICATE 0x09	 /* 证书（本实现用不到） */
#define HK_TLV_SIGNATURE 0x0A	 /* 64 字节：Ed25519 签名（HAP 里也叫 PROOF） */
#define HK_TLV_PERMISSIONS 0x0B	 /* 1 字节：0x00=普通用户 0x01=管理员 */
#define HK_TLV_FRAGMENTDATA 0x0C /* 分片（本实现用不到） */
#define HK_TLV_FRAGMENTLAST 0x0D /* 分片结束标记 */
#define HK_TLV_FLAGS 0x13		 /* 1 字节：0x01=临时配对（规范值，本实现不使用） */

/*
 * ⚠️ 最容易踩的一个坑：HAP 里有**两个**"证明"类型，含义完全不同：
 *
 *   0x04 PASSWORD_PROOF —— SRP 的 M1/M2，只出现在 Pair Setup 的 M3/M4
 *   0x0A SIGNATURE      —— Ed25519 签名，出现在 M5/M6 与 Pair Verify 的 M2/M3
 *
 * 混用的话，C 侧自测照样全绿（两边错得一样），但 iOS 会静默配对失败。
 * 这个 bug 就是靠 tools/hap_ref.py 的外部交叉验证抓出来的。
 */

/* type 0x00（method）的取值 */
#define HK_METHOD_PAIR_SETUP 0x00
#define HK_METHOD_PAIR_VERIFY 0x01
#define HK_METHOD_ADD_PAIRING 0x03
#define HK_METHOD_REMOVE_PAIRING 0x04
#define HK_METHOD_LIST_PAIRINGS 0x05

/* type 0x07（error）的取值 */
#define HK_ERR_UNKNOWN 0x01
#define HK_ERR_AUTH 0x02		 /* 配对码错 / 签名不对 */
#define HK_ERR_BACKOFF 0x03		 /* 太忙，配合 retryDelay */
#define HK_ERR_MAX_PEERS 0x04	 /* 已达最大配对数 */
#define HK_ERR_MAX_TRIES 0x05	 /* 尝试次数过多 */
#define HK_ERR_UNAVAILABLE 0x06	 /* 功能不可用 */

/* 单片最大长度（length 字段是 1 字节） */
#define HK_TLV_MAX_FRAGMENT 255

/* 一条 TLV8 消息的推荐缓冲：SRP 的 A/B 是 384 字节，分片后约 390 */
#define HK_TLV_BUF_MIN 512

/*
 * 解析器（只读游标）。
 * 用法：init 之后反复 next()，返回 1=取到一条，0=正常结束，-1=格式错误。
 * 返回 -1 后不要再调用（游标状态未定义）。
 */
typedef struct _HK_TLV_ITER
{
	const uint8_t *buf;
	size_t len;
	size_t pos;
} HK_TLV_ITER;

void hk_tlv_iter_init(HK_TLV_ITER *it, const uint8_t *buf, size_t len);

/*
 * 取下一条。**注意：分片不会在这里合并** —— 每个分片是独立的一条，
 * 上层要么自己拼，要么直接用 hk_tlv_find()（它会合并）。
 */
int hk_tlv_iter_next(HK_TLV_ITER *it, uint8_t *type, const uint8_t **val, size_t *vlen);

/*
 * 按 type 查找并**合并连续分片**，结果拷进 out。
 * 返回写进 out 的字节数；没找到返回 0。
 * 返回 -1 表示：消息格式错，或者 out 装不下（out_cap 太小）。
 *
 * out 传 NULL、out_cap 传 0 可以只用来"探测长度"吗？不行 —— 那样没法区分
 * "没找到"和"长度 0"。要测长度请用 hk_tlv_find_len()。
 */
int hk_tlv_find(const uint8_t *buf, size_t len, uint8_t type, uint8_t *out, size_t out_cap);

/* 同上，但只返回该 type 的完整值长度（找不到返回 0，格式错返回 -1） */
int hk_tlv_find_len(const uint8_t *buf, size_t len, uint8_t type);

/* 整条消息扫描一遍，确认没有越界/残尾。合法返回 0 */
int hk_tlv_validate(const uint8_t *buf, size_t len);

/*
 * 写入端：往 buf 追加一条（超长自动分片）。pos 是**输入输出**的写游标，
 * 调用前 *pos 是当前长度，返回后是新的长度。
 * 返回 0 成功；-1 表示 cap 不够（此时 buf 与 *pos **不会被改动**，
 * 这样上层可以安全地"试写失败再退回错误响应"）。
 */
int hk_tlv_put(uint8_t *buf, size_t *pos, size_t cap,
			   uint8_t type, const uint8_t *val, size_t vlen);

/* 写入 1 字节值（state / error / method 这些用） */
int hk_tlv_put_u8(uint8_t *buf, size_t *pos, size_t cap, uint8_t type, uint8_t val);

/*
 * 构造一条错误响应：[state=state][error=err]。
 * 这是"要么全写、要么不写"的：空间不够就返回 -1 且不动 buf。
 */
int hk_tlv_put_error(uint8_t *buf, size_t *pos, size_t cap, uint8_t state, uint8_t err);

#ifdef __cplusplus
}
#endif

#endif /* HK_TLV_H */

/*
 * hk_hap.h —— HAP 会话层：帧加密、HTTP、端点分发
 *
 * 这一层是"配对完成后"的通信。三个最容易写错、错了就表现为
 * "iOS 连上了但永远转圈"的点，先写在最前面：
 *
 *   1) **帧的 AAD 不是空**，是那 2 字节的长度头（小端）。
 *      配对阶段的 AEAD aad 才是空 —— 两者别混。
 *   2) **nonce 是每方向独立的计数器**：4 个零字节 + 8 字节小端计数，
 *      配件→控制器与控制器→配件各数各的，从 0 开始每帧 +1。
 *      计数器错位 = 第一帧之后全部认证失败。
 *   3) **单帧明文上限 1024 字节**（规范 0x400）。超了必须切成多帧，
 *      每帧都带自己的长度头 + 密文 + 16 字节 tag。/accessories 的响应
 *      很容易超过 1024，所以分片是必需的，不是可选的。
 *
 * 以上三条都对 HAP-NodeJS 的 hapCrypto.ts（layerEncrypt/layerDecrypt）核对过。
 *
 * 内存：本层不做缓冲分配，所有缓冲由调用方给。
 * 解密侧能处理"半包"—— 返回消费了多少字节，剩下的由调用方留着下次接着喂。
 */
#ifndef HK_HAP_H
#define HK_HAP_H

#include <stdint.h>
#include <stddef.h>

#include "hk_pair.h"
#include "hk_acc.h"
#include "hk_tlv.h"
#include "hk_chachapoly.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- 帧 ---- */
#define HK_HAP_FRAG_MAX 1024	/* 单帧明文上限（规范 0x400） */
#define HK_HAP_TAG_LEN 16
#define HK_HAP_HDR_LEN 2
#define HK_HAP_OVERHEAD (HK_HAP_HDR_LEN + HK_HAP_TAG_LEN) /* 每帧固定开销 18 */

/* ---- HTTP ---- */
#define HK_HTTP_PATH_MAX 24
#define HK_HTTP_QUERY_MAX 64

#define HK_HTTP_GET 0
#define HK_HTTP_PUT 1
#define HK_HTTP_POST 2

/* ------------------------------------------------------------------ */

/*
 * 一个已建立的 HAP 会话（Pair Verify 成功之后才有）。
 * a2c = 配件发给控制器用的密钥；c2a = 解密控制器数据用的密钥。
 * 命名反直觉的原因见 hk_pair.h：HAP 的 info 串站在控制器视角写。
 */
typedef struct _HK_SESSION
{
	uint8_t a2c[32];
	uint8_t c2a[32];
	uint64_t a2c_count;
	uint64_t c2a_count;
	int up; /* 1 = 会话已建立 */
} HK_SESSION;

void hk_session_begin(HK_SESSION *s, const uint8_t a2c[32], const uint8_t c2a[32]);

/* 给定明文长度，算加密后需要多少字节（用于分配缓冲） */
size_t hk_frame_encrypted_size(size_t pt_len);

/*
 * 把明文编成一串 HAP 帧写入 out。会自动按 1024 切片。
 * 返回写出的字节数；-1 表示 out_cap 不够。
 */
int hk_frame_encrypt(HK_SESSION *s, uint8_t *out, size_t out_cap,
					 const uint8_t *pt, size_t pt_len);

/*
 * 解密。in 是任意长度的 TCP 字节流（可能含半包）。
 * 解出的明文写入 out（多帧会拼接）。
 *   *consumed = 实际消费掉的密文字节数（剩下的调用方应留到下次）
 * 返回解出的明文字节数（可能是 0，表示还不够一整帧）；
 * -1 = 认证失败或格式错（一旦失败，连接必须断开）。
 */
int hk_frame_decrypt(HK_SESSION *s, const uint8_t *in, size_t in_len,
					 size_t *consumed, uint8_t *out, size_t out_cap);

/* ------------------------------------------------------------------ */

typedef struct _HK_HTTP_REQ
{
	int method;
	char path[HK_HTTP_PATH_MAX];
	char query[HK_HTTP_QUERY_MAX];
	size_t content_length;
	const uint8_t *body; /* 指向请求缓冲内部，不拷贝 */
	size_t body_len;	 /* = min(content_length, 实际可用) */
} HK_HTTP_REQ;

/* 解析一个 HTTP/1.1 请求。返回 0 成功，-1 格式错。 */
int hk_http_parse(const uint8_t *buf, size_t len, HK_HTTP_REQ *out);

/*
 * 构造响应。body 为 NULL 或 body_len=0 时不带 Content-Length 之外的 body。
 * HAP 要求 204 不带 body 和 Content-Type。
 * 返回写出的字节数；0 = 缓冲不够。
 */
size_t hk_http_response(uint8_t *out, size_t cap, int status,
						const char *ctype, const uint8_t *body, size_t body_len);

/*
 * 构造**事件通知**（第四阶段主动推送用）。
 *
 * 状态行是 "EVENT/1.0 200 OK"，**不是** "HTTP/1.1 200 OK" ——
 * 这是 HAP 与 HTTP 少数几个实实在在不一样的地方之一。写错了的表现是
 * iOS 收到一条畸形响应后直接断开，且不会重试，现场只能看到"设备无响应"。
 *
 * ctype 恒为 application/hap+json（HAP 事件只能是这个类型）。
 * 返回写出的字节数；0 = 缓冲不够。
 */
size_t hk_event_response(uint8_t *out, size_t cap,
						 const uint8_t *body, size_t body_len);

/* ------------------------------------------------------------------ */

typedef struct _HK_HAP_CTX
{
	HK_SESSION *sess;		/* 可为 NULL（还没验证） */
	HK_SETUP *setup;		/* Pair Setup 会话，固件侧一个就够 */
	HK_VERIFY *verify;		/* Pair Verify 会话 */
	HK_ACCESSORY *acc;		/* 配件长期身份 */
	const HK_ACCESSORY_DB *db;
	const HK_ACC_IO *io;

	uint8_t ios_ltpk[32]; /* 已存储的控制器长期公钥 */
	int paired;			  /* 1 = 已配对过 */

	const char *setup_code; /* "XXX-XX-XXX" */
	int max_peers;			/* 已达上限时回 HK_ERR_MAX_PEERS */

	/* 随机源：固件侧填 TRNG；测试时填固定值以便复现 */
	void (*rand)(uint8_t *buf, size_t n, void *ctx);
	void *rand_ctx;
} HK_HAP_CTX;

/*
 * 处理一个**已解密**的 HTTP 请求，产出**明文** HTTP 响应（调用方再加密）。
 * 返回响应长度；0 表示缓冲不够。
 *
 * 端点：
 *   POST /pair-setup       未加密，body = TLV8
 *   POST /pair-verify      未加密，body = TLV8
 *   POST /pairings         加密（本实现只回"不支持"）
 *   POST /identify         未加密
 *   GET  /accessories      加密
 *   GET  /characteristics?id=1.12,...   加密
 *   PUT  /characteristics  加密
 */
size_t hk_hap_handle(HK_HAP_CTX *c, const HK_HTTP_REQ *req,
					 uint8_t *out, size_t cap);

/* 把 "id=1.12,1.22" 解析成条目（aid/iid），返回条数 */
int hk_hap_parse_ids(const char *query, HK_JITEM *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* HK_HAP_H */

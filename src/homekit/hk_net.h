/*
 * hk_net.h —— HomeKit 第三阶段：平台层（TCP 51826 / mDNS / 持久化 / TRNG）
 *
 * 这一层把第二阶段的"纯协议"接到真实的芯片上。它分两半，靠
 * HK_NET_PLATFORM 宏分开：
 *
 *   A. 纯逻辑（不 include 任何 SDK 头文件，主机 gcc 能编、能跑自测）
 *      - HK_CONN：一条 TCP 连接上的 HAP 状态机（含明文/密文切换、半包）
 *      - hk_sub_*：事件订阅表（第四阶段主动推送要用，这里先落底子）
 *      - hk_mdns_txt()：mDNS 的 TXT 记录组包
 *   B. 平台胶水（只有编进固件时才有）
 *      - hk_net_start()：起监听线程
 *      - hk_persist_load/save：走 hffile_userbin
 *      - hk_rand：硬件 TRNG
 *      - hk_mdns_start：mdns_announce_service
 *
 * ★★★ 这一层最容易写错的五件事 ★★★
 *
 *   1) **加密的切换点是 Pair Verify 的 M4 之后，不是 M2 之后。**
 *      M1/M2 还是明文 TLV8。判据是 `HK_SESSION.up`（hk_session_begin 里置 1，
 *      而 hk_session_begin 只在 hk_hap.c 处理完 /pair-verify 的 state=3 时调用）。
 *      早切一帧的表现是：iOS 发来 M3 时配件当密文解，直接认证失败断连。
 *
 *   2) **TCP 是字节流，不是报文。** 一次 recv 可能只有半个请求，也可能
 *      一次带来两个请求。必须"累积 + 循环消费"，绝不能假设一收一发。
 *      同理，解密后的残帧要留在连接里等下次。
 *
 *   3) **半包绝不能推进计数器。** hk_frame_decrypt 用 *consumed 告诉调用方
 *      消费了多少，没消费的字节必须原样留着 —— 一旦拿去解第二次，
 *      nonce 计数器就错位，之后每一帧都认证失败。
 *
 *   4) **MEMP_NUM_TCP_PCB 只有 4**（lwipopts.h:127，不是文档里早期写的 12）。
 *      一个 listening PCB + 一个连接就占掉 2 个，httpd 还要用。
 *      所以本层刻意只支持 HK_NET_MAX_CONN 条并发，多了直接拒。
 *
 *   5) **LTSK 丢了就彻底连不上。** 持久化失败不能"带病运行"——
 *      身份不对时 iOS 会静默失败，现场极难排查。所以 hk_persist_load
 *      失败要当成"没配过"重新生成身份并**立刻落盘**，而不是继续用内存里的。
 *
 * 关于可测性：把 TCP 的字节流喂给 hk_conn_feed()，它吐出该发回的字节。
 * 这台"状态机"不认识 socket，所以能在 PC 上把上面 1/2/3 全测掉 ——
 * 这三条恰恰是真机上最难查的。
 */
#ifndef HK_NET_H
#define HK_NET_H

#include <stdint.h>
#include <stddef.h>

#include "hk_hap.h"
#include "hk_store.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define HK_NET_PORT 51826

/* ---- 缓冲预算（都很紧，改之前先看 doc/HOMEKIT.md 第 7 节） ---- */

/* 单条请求的明文上限。/accessories 的**请求**很短，但 PUT /characteristics
 * 可能带 6 条；给 1600 是留了余量。 */
#define HK_NET_RX_CAP 1600

/* 解密后的明文暂存的容量，与 RX 同量级 */
#define HK_NET_PLAIN_CAP 1600

/* hk_hap_handle 产出的明文响应上限。/accessories 实测 1766 字节，
 * 给 2300 是防止以后加特性溢出。 */
#define HK_NET_RESP_CAP 2300

/* 加密并分片后的输出上限。每帧固定开销 18 字节（2 长度头 + 16 tag），
 * 2300 的明文最多切成 3 帧 => 2300 + 54 = 2354，给 2400。 */
#define HK_NET_TX_CAP 2400

/* 一次 feed 允许产出的总字节（可能含多个响应），给两帧的量 */
#define HK_NET_OUT_CAP (HK_NET_TX_CAP * 2)

/* 未消费的残帧（密文）暂存。一帧明文最多 1024 + 18，给 1100 够；
 * 但 iOS 可能连发多帧，给 1200。 */
#define HK_NET_FRAG_CAP 1200

/* ---- 返回码 ---- */
#define HK_NET_OK 0
#define HK_NET_ERR_FATAL -1	 /* 认证失败/格式错 —— 调用方必须断开 */
#define HK_NET_ERR_NOMEM -2	 /* 输出缓冲装不下 */
#define HK_NET_ERR_BIGREQ -3 /* 单条请求超过 HK_NET_RX_CAP */

/* ---- 事件订阅表 ---- */

/* iOS 用 PUT /characteristics 带 "ev":true 订阅，一条连接最多记这么多个。
 * 6 路 On 才 6 个，给 8 留余量。 */
#define HK_SUB_MAX 8

typedef struct _HK_SUB
{
	uint16_t iid;
	uint8_t used;
} HK_SUB;

void hk_sub_clear(HK_SUB *tab, int max);
int hk_sub_add(HK_SUB *tab, int max, uint16_t iid);
int hk_sub_del(HK_SUB *tab, int max, uint16_t iid);
int hk_sub_has(const HK_SUB *tab, int max, uint16_t iid);

/* ------------------------------------------------------------------ */

/*
 * 一条 HAP 连接。所有缓冲都内嵌 —— 这样主机自测和固件用的是同一份布局，
 * 不会出现"PC 上好好的、上了板栈溢出"。
 *
 * 注意 ctx 里那些指针（sess/setup/verify/acc/db/io）必须在 hk_conn_init
 * 之后一直有效。本结构通常做成静态全局。
 */
typedef struct _HK_CONN
{
	HK_HAP_CTX ctx;
	HK_SESSION sess;
	HK_SETUP setup;
	HK_VERIFY verify;

	HK_SUB subs[HK_SUB_MAX];

	/* 收到的、**尚未处理完**的明文请求字节 */
	uint8_t rx[HK_NET_RX_CAP];
	size_t rx_len;

	/* 未消费的密文残帧（只对解密阶段有意义） */
	uint8_t frag[HK_NET_FRAG_CAP];
	size_t frag_len;

	/* hk_frame_decrypt 的明文输出暂存 */
	uint8_t plain[HK_NET_PLAIN_CAP];

	/* hk_hap_handle 的明文响应 */
	uint8_t resp[HK_NET_RESP_CAP];

	/* 加密后的输出 */
	uint8_t tx[HK_NET_TX_CAP];

	int encrypted; /* 1 = 已 Verify 成功，后续走帧加密 */
	int dead;	   /* 1 = 出过致命错，调用方应断开 */
} HK_CONN;

/*
 * 初始化一条连接。
 *   acc        : 配件长期身份（LTSK/LTPK/id），必须已持久化过
 *   io         : 6 路继电器的读写回调
 *   setup_code : "XXX-XX-XXX"
 *   store      : 已配对控制器表（可为 NULL，表示不支持多控制器）
 * 返回值 0。
 *
 * 注意：**不会**重置 sess —— 每次连接都是新会话，必须重新 Verify。
 */
int hk_conn_init(HK_CONN *c, const HK_ACCESSORY *acc, const HK_ACC_IO *io,
				 const char *setup_code, const HK_STORE *store,
				 void (*rand)(uint8_t *buf, size_t n, void *ctx), void *rand_ctx);

/*
 * 喂入本次 recv 到的字节，产出要发回的字节。
 *
 *   in/in_len : 本次收到的 TCP 字节（可以是任意长度，包括半个请求）
 *   out/out_cap: 调用方给的回写缓冲，至少 HK_NET_OUT_CAP
 *   *out_len  : 产出的字节数（可能是 0，表示还没凑齐一个请求）
 *
 * 返回 HK_NET_OK / HK_NET_ERR_*。
 * 一旦返回 FATAL，这条连接的数据已经不可信，必须 close。
 *
 * 里面会做三件事：解帧（若已加密）、累积、循环处理所有完整请求。
 */
int hk_conn_feed(HK_CONN *c, const uint8_t *in, size_t in_len,
				 uint8_t *out, size_t *out_len, size_t out_cap);

/* ---- 第四阶段：事件推送 ---- */

/* 一条事件通知的明文上限。body 形如
 *   {"characteristics":[{"aid":1,"iid":12,"value":true}]}   ≈ 60 字节
 * 加上 EVENT/1.0 的头（约 76 字节），给 160 足够。 */
#define HK_NET_EVT_CAP 160

/*
 * 构造一条事件通知的**明文**（调用方再用会话密钥加密发出去）。
 *   iid   : 发生变化的 characteristic（6 路的 On 是 12/22/.../62）
 *   value : 新值
 * 返回写出的字节数；0 = 缓冲不够或参数非法。
 *
 * 只推一个 characteristic：本工程的事件源是"某一路继电器被扳动"，
 * 一次只可能一路变。HAP 虽然允许一次推多条，但只要其中一条 iid 写错
 * 整批就会被 iOS 丢弃，分批推要安全得多。
 *
 * ★ 注意不要带 "status" 字段 —— 那是 /characteristics **响应**用的。
 *   事件里带上它，iOS 会当成响应去匹配请求号，表现为"开关状态不刷新"。
 */
size_t hk_event_build(uint8_t *out, size_t cap, uint16_t iid, int value);

/* ------------------------------------------------------------------ */

/*
 * 组 HAP 的 mDNS TXT 记录（纯字符串，可在主机测）。
 * 形如： pv=1.1:id=F0:FE:6B:E6:78:82:c#=1:s#=1:ff=0:ci=7:md=TC1:sf=1
 *
 *   id     : 配件 pairing ID，就是 MAC（"XX:XX:XX:XX:XX:XX"）
 *   cnum   : 配置号 c#，结构或配对列表变了要 +1，否则 iOS 不重新拉 /accessories
 *   paired : 1 = 已配对（此时 sf=0）
 *   model  : 型号串
 *
 * 返回写出的字符数（不含 '\0'）；0 表示 cap 不够。
 *
 * 两个坑：
 *   - 整个串**不能超过 255 字节**（MDNS_MAX_KEYVAL_LEN），所以 model 要短；
 *   - mdns.h 明确说了：**keyvals 缓冲会被 mDNS 模块改写**，
 *     所以每次要重新组，不能组一次一直用。
 */
size_t hk_mdns_txt(char *out, size_t cap, const char *id, uint32_t cnum,
				   int paired, const char *model);

/* ------------------------------------------------------------------ */
/* 以下只有编进固件时才有（HK_NET_PLATFORM）                            */
/* ------------------------------------------------------------------ */

#ifdef HK_NET_PLATFORM

/* userbin 里放 HomeKit 数据的位置：紧跟 PLUG_CONFIG 之后。
 * 用 sizeof(PLUG_CONFIG) 而不是写死 188/192 —— 结构体以后再改也不用改这里。
 * +8 是留一点隔离，免得两段紧挨着互相影响。 */
#define HK_STORE_USERBIN_ADDR (PLUG_CONFIG_USERBIN_ADDR + sizeof(PLUG_CONFIG) + 8)

/* SDK 在 userbin 2864 之后放自己的 MQTT 备份
 * （hfflashlayout.h: USER_BIN_FILE_MQTT_OFFSET - USER_BIN_FILE_OFFSET = 0xB30 = 2864）。
 * 早期把计量数据写到 3072 就压在这儿，表现为开机跑到计量初始化就重启。 */
#define HK_USERBIN_SDK_LIMIT 2864

/*
 * 从 userbin 读回配对信息。任何一步不对（读到垃圾/CRC 错/版本不对）
 * 都返回 -1，调用方应按"没配过"处理。
 * 返回 0 表示 s 里是有效内容。
 */
int hk_persist_load(HK_STORE *s);

/* 落盘。返回 0 成功，-1 失败。 */
int hk_persist_save(const HK_STORE *s);

/* 清掉已存的配对信息（Web 后台"恢复出厂"用） */
int hk_persist_erase(void);

/* 硬件 TRNG。签名与 hk_hap 的 rand 回调一致，可直接传给 ctx。 */
void hk_rand(uint8_t *buf, size_t n, void *ctx);

/*
 * 起 HAP 服务：读回持久化 -> 广播 mDNS -> 起 TCP 51826 监听线程。
 *   display_name : 配件在「家庭」App 里显示的名字（mDNS 的 servname）
 *
 * 配件 ID 不在这里传 —— HAP 规定它就是 MAC（"XX:XX:XX:XX:XX:XX"），
 * 本函数内部从 hfnet_get_mac_address() 取，免得调用方传错格式。
 *
 * 返回 0 成功；-1 表示持久化存不下（此时**不会**起服务 ——
 * 身份不能落盘的话每次重启都要重配对，比不启动更糟）。
 */
int hk_net_start(const char *display_name);

/* 供 Web 后台显示/重置用 */
const HK_STORE *hk_net_store(void);
int hk_net_is_paired(void);

/*
 * 取当前配对码（"XXX-XX-XXX"）。返回的是内部静态缓冲的指针。
 * 服务没起来时返回 ""。
 */
const char *hk_net_get_code(void);

/*
 * 改配对码。
 *   code : "XXX-XX-XXX" 或 "XXXXXXXX"（连字符可省）
 * 返回 0 成功；-1 参数非法；-2 持久化失败（此时内存里已改，但重启会丢）。
 *
 * ★★ 为什么改配对码一定要顺带解绑所有已配对的控制器 ★★
 * HAP 的 SRP 里，配对码参与派生 SRP 的 salt/verifier。iOS 一旦配对成功，
 * 之后的连接走 Pair Verify（用长期公钥），**不再校验配对码**。
 * 所以"改了码但保留配对"是无效的：旧 iPhone 照样能用，
 * 而码本身却变了 —— 用户会以为"改了码别人就进不来"，实际没有。
 * 正确语义是：改码 = 恢复出厂，所有人都要重新配对。
 * 这里直接把整个 store 重置（含身份），最干净也最不容易出错。
 */
int hk_net_set_code(const char *code);

/*
 * 生成一个新的随机配对码并落盘（等同"改配对码"的随机版）。
 * 用硬件 TRNG。返回 0 成功并把新码写进 out。
 */
int hk_net_gen_code(char *out, size_t cap);

/*
 * 解除所有配对（Web 后台"恢复出厂"）。保留配件自身身份 LTSK/LTPK ——
 * 身份可以复用，只要控制器列表清空即可；这样 iOS 那边删掉配件后能重配。
 * 成功会递增 c# 并重新广播 mDNS。
 */
int hk_net_unpair_all(void);

/*
 * 通知某一路继电器状态变了（第四阶段的推送）。
 *   idx : 0..5
 *   v   : 0/1
 *
 * ★ 可以在**任何线程**里调（包括按键中断的回调、MQTT 回调、httpd 回调）。
 *   函数只置位掩码就返回，不碰 socket —— 因为 httpd 回调里阻塞等网络
 *   会把整个 Web 后台拖死（这个工程里 httpd 只有一个 1KB 的缓冲和单线程）。
 */
void hk_net_notify(uint8_t idx, int v);

#endif /* HK_NET_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* HK_NET_H */

/*
 * hk_net.c —— HomeKit 第三阶段：平台层
 *
 * 文件按 HK_NET_PLATFORM 分成两半，前半（纯逻辑）不 include 任何 SDK 头，
 * 所以 tools/ 下的自测能在 PC 上把它完整跑一遍。这么做的原因写在
 * hk_net.h 的开头：真机上最难查的三件事（加密切换点、TCP 粘包、半包
 * 推进计数器）全部落在前半，而它们在 PC 上都能被确定地测出来。
 */
#include "hk_net.h"

#include <stdio.h>
#include <string.h>

#include "hk_json.h"

/* ================================================================== */
/* A. 纯逻辑：事件订阅表                                               */
/* ================================================================== */

void hk_sub_clear(HK_SUB *tab, int max)
{
	if (tab == NULL)
		return;
	memset(tab, 0, sizeof(HK_SUB) * (size_t)max);
}

int hk_sub_add(HK_SUB *tab, int max, uint16_t iid)
{
	int i, free_at = -1;

	if (tab == NULL)
		return -1;

	for (i = 0; i < max; i++)
	{
		if (tab[i].used && tab[i].iid == iid)
			return 0; /* 已订阅，幂等 */
		if (!tab[i].used && free_at < 0)
			free_at = i;
	}
	if (free_at < 0)
		return -1; /* 满了 */

	tab[free_at].used = 1;
	tab[free_at].iid = iid;
	return 0;
}

int hk_sub_del(HK_SUB *tab, int max, uint16_t iid)
{
	int i;

	if (tab == NULL)
		return -1;
	for (i = 0; i < max; i++)
	{
		if (tab[i].used && tab[i].iid == iid)
		{
			tab[i].used = 0;
			tab[i].iid = 0;
			return 0;
		}
	}
	return -1;
}

int hk_sub_has(const HK_SUB *tab, int max, uint16_t iid)
{
	int i;

	if (tab == NULL)
		return 0;
	for (i = 0; i < max; i++)
	{
		if (tab[i].used && tab[i].iid == iid)
			return 1;
	}
	return 0;
}

/* ================================================================== */
/* A. 纯逻辑：请求边界判定                                             */
/* ================================================================== */

/* 大小写不敏感的前缀比较，s 传小写 */
static int ieq(const uint8_t *b, size_t len, const char *s)
{
	size_t i;

	for (i = 0; s[i]; i++)
	{
		char c;
		if (i >= len)
			return 0;
		c = (char)b[i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (c != s[i])
			return 0;
	}
	return 1;
}

/*
 * 判断 buf 里是否已经有一个完整的 HTTP 请求。
 * 返回它占用的总字节数；-1 表示头还没收全；-2 表示 Content-Length 大得
 * 不可能装下（这种请求必须判死，否则会一直等下去直到缓冲爆掉）。
 */
static int req_total(const uint8_t *b, size_t len)
{
	size_t j, i, body_at = 0, cl = 0;
	int found = 0;

	if (b == NULL || len < 16)
		return -1;

	for (j = 0; j + 3 < len; j++)
	{
		if (b[j] == '\r' && b[j + 1] == '\n' && b[j + 2] == '\r' && b[j + 3] == '\n')
		{
			body_at = j + 4;
			found = 1;
			break;
		}
		if (b[j] == '\n' && b[j + 1] == '\n')
		{
			body_at = j + 2;
			found = 1;
			break;
		}
	}
	if (!found)
		return -1;

	/* "content-length" 14 字符 + ':' */
	for (i = 0; i + 15 <= body_at; i++)
	{
		if (b[i] != 'C' && b[i] != 'c')
			continue;
		if (!ieq(b + i, body_at - i, "content-length"))
			continue;
		i += 14;
		while (i < body_at && (b[i] == ' ' || b[i] == ':'))
			i++;
		while (i < body_at && b[i] >= '0' && b[i] <= '9')
		{
			cl = cl * 10u + (size_t)(b[i] - '0');
			if (cl > HK_NET_RX_CAP)
				return -2;
			i++;
		}
		break;
	}

	if (body_at + cl > HK_NET_RX_CAP)
		return -2;
	return (int)(body_at + cl);
}

/* ================================================================== */
/* A. 纯逻辑：连接状态机                                               */
/* ================================================================== */

int hk_conn_init(HK_CONN *c, const HK_ACCESSORY *acc, const HK_ACC_IO *io,
				 const char *setup_code, const HK_STORE *store,
				 void (*rand)(uint8_t *buf, size_t n, void *ctx), void *rand_ctx)
{
	if (c == NULL || acc == NULL)
		return -1;

	memset(c, 0, sizeof(*c));

	c->ctx.sess = &c->sess;
	c->ctx.setup = &c->setup;
	c->ctx.verify = &c->verify;
	c->ctx.acc = (HK_ACCESSORY *)acc;
	c->ctx.db = &hk_acc_db;
	c->ctx.io = io;
	c->ctx.setup_code = setup_code;
	c->ctx.rand = rand;
	c->ctx.rand_ctx = rand_ctx;

	/* 已存储的控制器公钥：取第一个（本工程只支持一个，够用） */
	if (store != NULL && store->valid && store->npeers > 0)
	{
		memcpy(c->ctx.ios_ltpk, store->peers[0].ltpk, 32);
		c->ctx.paired = 1;
	}
	else
	{
		c->ctx.paired = 0;
	}
	c->ctx.max_peers = HK_STORE_MAX_PEERS;

	hk_setup_init(&c->setup);
	hk_verify_init(&c->verify);
	hk_sub_clear(c->subs, HK_SUB_MAX);

	c->rx_len = 0;
	c->frag_len = 0;
	c->encrypted = 0;
	c->dead = 0;
	return 0;
}

/*
 * 维护事件订阅表。iOS 用 PUT /characteristics 带 "ev":true/false 增删订阅。
 * 注意：一条 PUT 里可能既有 value 又有 ev（"写并订阅"），所以要分开看。
 */
static void apply_subs(HK_CONN *c, const HK_HTTP_REQ *req)
{
	HK_JITEM items[8];
	int n, i;

	if (req->method != HK_HTTP_PUT)
		return;
	if (strcmp(req->path, "/characteristics") != 0)
		return;

	n = hk_json_scan_writes(req->body, req->body_len, items, 8);
	if (n <= 0)
		return;

	for (i = 0; i < n; i++)
	{
		if (!items[i].has_ev)
			continue;
		if (items[i].iid <= 0 || items[i].iid > 0xffff)
			continue;
		if (items[i].ev)
			hk_sub_add(c->subs, HK_SUB_MAX, (uint16_t)items[i].iid);
		else
			hk_sub_del(c->subs, HK_SUB_MAX, (uint16_t)items[i].iid);
	}
}

int hk_conn_feed(HK_CONN *c, const uint8_t *in, size_t in_len,
				 uint8_t *out, size_t *out_len, size_t out_cap)
{
	size_t produced = 0;

	if (out_len)
		*out_len = 0;
	if (c == NULL || out == NULL || out_cap == 0)
		return HK_NET_ERR_FATAL;
	if (c->dead)
		return HK_NET_ERR_FATAL;

	/* ---- 1. 把新到的字节变成 rx 里的明文 ---- */
	if (in != NULL && in_len > 0)
	{
		if (c->encrypted)
		{
			if (c->frag_len + in_len > HK_NET_FRAG_CAP)
			{
				c->dead = 1;
				return HK_NET_ERR_FATAL;
			}
			memcpy(c->frag + c->frag_len, in, in_len);
			c->frag_len += in_len;

			/* 尽量多解：一次 recv 可能带来好几帧 */
			while (c->frag_len > 0)
			{
				size_t consumed = 0;
				int n;

				n = hk_frame_decrypt(&c->sess, c->frag, c->frag_len,
									 &consumed, c->plain, HK_NET_PLAIN_CAP);
				if (n < 0)
				{
					c->dead = 1;
					return HK_NET_ERR_FATAL;
				}
				if (consumed == 0)
					break; /* 还不够一整帧，留着等下次 */

				if (consumed < c->frag_len)
					memmove(c->frag, c->frag + consumed, c->frag_len - consumed);
				c->frag_len -= consumed;

				if (n == 0)
					continue;

				if (c->rx_len + (size_t)n > HK_NET_RX_CAP)
				{
					c->dead = 1;
					return HK_NET_ERR_BIGREQ;
				}
				memcpy(c->rx + c->rx_len, c->plain, (size_t)n);
				c->rx_len += (size_t)n;
			}
		}
		else
		{
			if (c->rx_len + in_len > HK_NET_RX_CAP)
			{
				c->dead = 1;
				return HK_NET_ERR_BIGREQ;
			}
			memcpy(c->rx + c->rx_len, in, in_len);
			c->rx_len += in_len;
		}
	}

	/* ---- 2. 循环处理 rx 里所有完整的请求 ---- */
	while (c->rx_len > 0)
	{
		HK_HTTP_REQ req;
		size_t rlen, olen;
		int need, rc;

		need = req_total(c->rx, c->rx_len);
		if (need == -1)
			break; /* 头还没收全 */
		if (need == -2)
		{
			c->dead = 1;
			return HK_NET_ERR_BIGREQ;
		}
		if ((size_t)need > c->rx_len)
			break; /* body 还没收全 */

		if (hk_http_parse(c->rx, (size_t)need, &req) != 0)
		{
			c->dead = 1;
			return HK_NET_ERR_FATAL;
		}

		rlen = hk_hap_handle(&c->ctx, &req, c->resp, HK_NET_RESP_CAP);
		if (rlen == 0)
		{
			c->dead = 1;
			return HK_NET_ERR_NOMEM;
		}

		apply_subs(c, &req);

		if (c->encrypted)
		{
			rc = hk_frame_encrypt(&c->sess, c->tx, HK_NET_TX_CAP, c->resp, rlen);
			if (rc < 0)
			{
				c->dead = 1;
				return HK_NET_ERR_NOMEM;
			}
			olen = (size_t)rc;
			if (produced + olen > out_cap)
			{
				c->dead = 1;
				return HK_NET_ERR_NOMEM;
			}
			memcpy(out + produced, c->tx, olen);
		}
		else
		{
			olen = rlen;
			if (produced + olen > out_cap)
			{
				c->dead = 1;
				return HK_NET_ERR_NOMEM;
			}
			memcpy(out + produced, c->resp, olen);
		}
		produced += olen;

		/* 消费掉这个请求（rx 里可能还有下一个） */
		if ((size_t)need < c->rx_len)
			memmove(c->rx, c->rx + need, c->rx_len - (size_t)need);
		c->rx_len -= (size_t)need;

		/*
		 * 切换点：Verify 成功（sess.up 被 hk_session_begin 置 1）之后，
		 * 控制器接下来发的就是密文了。注意必须等**响应发出之后**才切，
		 * 因为这条 M4 响应本身还是明文。
		 */
		if (!c->encrypted && c->sess.up)
		{
			c->encrypted = 1;
			c->frag_len = 0;
		}
	}

	if (out_len)
		*out_len = produced;
	return HK_NET_OK;
}

/* ================================================================== */
/* A. 纯逻辑：mDNS TXT 记录                                            */
/* ================================================================== */

/* ================================================================== */
/* A. 纯逻辑：事件通知（第四阶段）                                      */
/* ================================================================== */

size_t hk_event_build(uint8_t *out, size_t cap, uint16_t iid, int value)
{
	uint8_t body[96];
	HK_JSON_OUT j;
	size_t blen;

	if (out == NULL || cap == 0 || iid == 0)
		return 0;

	hk_json_init(&j, body, sizeof(body));
	hk_json_obj_begin(&j);
	hk_json_key(&j, "characteristics");
	hk_json_arr_begin(&j);
	hk_json_obj_begin(&j);
	hk_json_key(&j, "aid");
	hk_json_int(&j, 1);
	hk_json_key(&j, "iid");
	hk_json_int(&j, (int32_t)iid);
	hk_json_key(&j, "value");
	hk_json_bool(&j, value ? 1 : 0);
	hk_json_obj_end(&j);
	hk_json_arr_end(&j);
	hk_json_obj_end(&j);

	if (!hk_json_ok(&j))
		return 0;
	blen = hk_json_len(&j);

	return hk_event_response(out, cap, body, blen);
}

/* ================================================================== */
/* A. 纯逻辑：mDNS TXT                                                 */
/* ================================================================== */

size_t hk_mdns_txt(char *out, size_t cap, const char *id, uint32_t cnum,
				   int paired, const char *model)
{
	int n;

	if (out == NULL || cap == 0 || id == NULL)
		return 0;

	n = snprintf(out, cap, "pv=1.1:id=%s:c#=%u:s#=1:ff=0:ci=7:md=%s:sf=%u",
				 id, (unsigned)cnum, (model != NULL) ? model : "TC1",
				 paired ? 0u : 1u);

	if (n < 0 || (size_t)n >= cap)
	{
		out[0] = '\0';
		return 0;
	}
	/* mdns.h: 单个 key/value 对不能超过 MDNS_MAX_KEYVAL_LEN(255) */
	if ((size_t)n > 255)
	{
		out[0] = '\0';
		return 0;
	}
	return (size_t)n;
}

/* ================================================================== */
/* B. 平台胶水（只有编进固件时才编译）                                  */
/* ================================================================== */

#ifdef HK_NET_PLATFORM

#include "hsf.h"
#include "hffile.h"
#include "hfsys.h"
#include "hfthread.h"
#include "hfnet.h"
#include "user_config.h"
#include "user_gpio.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/def.h"
#include "mdns/mdns.h"
#include "hal/hal/trng_api.h"

#include "hk_code.h"

/* ---- 编译期护栏：HomeKit 的存储区不许越过 SDK 的 MQTT 备份区 ---- */
typedef char hk_net_layout_ok[(HK_STORE_USERBIN_ADDR + HK_STORE_BLOB_MAX <= HK_USERBIN_SDK_LIMIT) ? 1 : -1];

/* ---- 全局状态 ---- */
static HK_STORE g_store;
static HK_ACCESSORY g_acc;
static char g_acc_id[18];	 /* "XX:XX:XX:XX:XX:XX" */
static HK_CONN g_conn;		 /* 只支持一条并发连接（PCB 只有 4 个） */
static int g_net_up;
static uint32_t g_cfg_num = 1;

/*
 * 待发事件（第四阶段）。用位掩码而不是队列：6 路各占一位，
 * 同一路连续被扳动两次只推最后一次 —— HAP 的事件本来就是"通知变化"，
 * 中间态丢了没关系，最终一致就行，这样连缓冲都不用开。
 *
 * 并发隐患（已知、未加锁）：g_store 被 Web 线程（读）和本线程（配对写）
 * 同时访问。工程里 user_config 那套也是这个用法，暂时沿用；
 * 真机若在配对瞬间刷页面出问题，再给 hk_persist_* 加 RTX 互斥。
 */
static volatile uint8_t g_evt_pending;
static volatile uint8_t g_evt_val[HK_NR_OUTLET];

/* mDNS 的三块缓冲必须常驻全局：mdns.h 明确要求 hostname/servname 在
 * mdns_start 与 mdns_stop 之间不变，且 keyvals 会被模块改写。 */
static char g_mdns_host[32];
static char g_mdns_serv[32];
static char g_mdns_txt[256];
static struct mdns_service g_mdns_svc;

/* ---- TRNG ---- */
void hk_rand(uint8_t *buf, size_t n, void *ctx)
{
	size_t olen = 0;
	(void)ctx;

	if (buf == NULL || n == 0)
		return;

	/*
	 * rda_trng_get_bytes 的第一个参数在 HAL 里没用到（对比 trng_get_bytes
	 * 才需要 trng_t*），传 NULL。失败就退化到 hfsys_random()——
	 * 那不是密码学强度的源，但总好过用固定值（固定值会让 SRP 的 b 可预测）。
	 */
	if (rda_trng_get_bytes(NULL, buf, n, &olen) == 0 && olen == n)
		return;

	{
		size_t i = 0;
		while (i < n)
		{
			unsigned int r = hfsys_random();
			size_t k;
			for (k = 0; k < 4 && i < n; k++, i++)
				buf[i] = (uint8_t)(r >> (8 * k));
		}
	}
}

/* ---- 持久化 ---- */
int hk_persist_load(HK_STORE *s)
{
	uint8_t blob[HK_STORE_BLOB_MAX];

	if (s == NULL)
		return -1;

	/*
	 * 不检查 hffile_userbin_read 的返回值：工程里现有的 read 调用都没检查，
	 * 而且读到垃圾（未写过的 flash）也是常态。有效性一律交给
	 * hk_store_deserialize 的 magic + 长度 + CRC 去判 —— 这才是真正可靠的判据。
	 */
	memset(blob, 0, sizeof(blob));
	hffile_userbin_read(HK_STORE_USERBIN_ADDR, (char *)blob, (int)sizeof(blob));

	hk_store_clear(s);
	if (hk_store_deserialize(blob, sizeof(blob), s) != 0)
	{
		hk_store_clear(s);
		return -1;
	}
	return 0;
}

int hk_persist_save(const HK_STORE *s)
{
	uint8_t blob[HK_STORE_BLOB_MAX];
	size_t len = 0;

	if (s == NULL)
		return -1;
	if (hk_store_serialize(s, blob, sizeof(blob), &len) != 0)
		return -1;
	if (hffile_userbin_write(HK_STORE_USERBIN_ADDR, (char *)blob, (int)len) != HF_SUCCESS)
		return -1;
	return 0;
}

int hk_persist_erase(void)
{
	hk_store_clear(&g_store);
	return hk_persist_save(&g_store);
}

/* ---- 6 路继电器的 I/O 回调 ---- */
static int io_get_on(uint8_t idx, int *v, void *ctx)
{
	(void)ctx;
	if (idx >= HK_NR_OUTLET || v == NULL)
		return -1;
	*v = (plug_status.plug[idx] != 0) ? 1 : 0;
	return 0;
}

static int io_set_on(uint8_t idx, int v, void *ctx)
{
	(void)ctx;
	if (idx >= HK_NR_OUTLET)
		return -1;
	user_relay_set(idx, v ? 1 : 0);
	return 0;
}

static void io_identify(void *ctx)
{
	(void)ctx;
	/* 排插没有蜂鸣器，用 LED 闪三下代替 */
	user_led_set(1);
	hf_thread_delay(200);
	user_led_set(0);
	hf_thread_delay(200);
	user_led_set(1);
	hf_thread_delay(200);
	user_led_set(0);
}

static const HK_ACC_IO g_acc_io = {
	io_get_on, io_set_on, io_identify, NULL};

/*
 * 把待发事件推出去（只在已 Verify、且有订阅者时才真正发）。
 * 返回发出的字节数；0 = 没什么可发。
 *
 * 为什么必须自己加密再发，而不能复用 hk_conn_feed：
 * feed 是"收到请求 -> 产出响应"，而事件是**没有请求**的主动推送。
 * HAP 里它走的是 "EVENT/1.0" 而不是 "HTTP/1.1"，且紧跟着的 body 才是
 * 那段 JSON —— hk_event_build 已经把头带上了，所以这里只管加帧。
 */
static int hk_net_push_events(int cs)
{
	uint8_t ev[HK_NET_EVT_CAP];
	uint8_t tx[HK_NET_TX_CAP];
	size_t elen = 0;
	int sent = 0;
	uint8_t pend;
	int i;

	if (!g_evt_pending)
		return 0;
	if (!g_conn.encrypted || g_conn.dead)
	{
		/* 没 Verify 过就没法推 —— 直接丢弃，等下次连接时 iOS 拉全量 */
		g_evt_pending = 0;
		return 0;
	}

	pend = g_evt_pending;
	g_evt_pending = 0; /* 先清：推失败也不重试，下一次变化会再带出来 */

	for (i = 0; i < HK_NR_OUTLET; i++)
	{
		int rc;

		if (!(pend & (1u << i)))
			continue;
		/* 只推 iOS 订阅过的 —— 没订阅就推，iOS 会认为这是"未知特性" */
		if (!hk_sub_has(g_conn.subs, HK_SUB_MAX, HK_IID_OUTLET_ON(i)))
			continue;

		elen = hk_event_build(ev, sizeof(ev), HK_IID_OUTLET_ON(i),
							  g_evt_val[i] ? 1 : 0);
		if (elen == 0)
			continue;

		rc = hk_frame_encrypt(&g_conn.sess, tx, sizeof(tx), ev, elen);
		if (rc <= 0)
			continue;

		if (lwip_send(cs, tx, (size_t)rc, 0) < 0)
			break;
		sent += rc;
	}
	return sent;
}

/* ---- 监听线程 ---- */
static void hk_net_thread(void *arg)
{
	int ls;
	struct sockaddr_in sa;
	uint8_t rbuf[600];
	uint8_t obuf[HK_NET_OUT_CAP];

	(void)arg;

	ls = lwip_socket(AF_INET, SOCK_STREAM, 0);
	if (ls < 0)
		return;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = lwip_htons(HK_NET_PORT);
	sa.sin_addr.s_addr = lwip_htonl(INADDR_ANY);

	if (lwip_bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0)
	{
		lwip_close(ls);
		return;
	}
	if (lwip_listen(ls, 1) < 0)
	{
		lwip_close(ls);
		return;
	}

	g_net_up = 1;

	while (1)
	{
		int cs = lwip_accept(ls, NULL, NULL);
		if (cs < 0)
		{
			hf_thread_delay(200);
			continue;
		}

		/*
		 * 每次新连接都重新起一个 HAP 会话（必须重新 Verify）。
		 *
		 * 配对码取 g_store.setup_code —— 它是持久化的、可由 Web 后台改写。
		 * 注意这里传的是指针，而 hk_conn_init 内部会保存它，
		 * 所以 g_store 在整个连接期间不能被别的线程改掉
		 * （改码会走 hk_net_set_code -> 重置 store，见那里的说明）。
		 */
		hk_conn_init(&g_conn, &g_acc, &g_acc_io,
					 g_store.setup_code[0] ? g_store.setup_code : HK_CODE_DEFAULT,
					 &g_store, hk_rand, NULL);

		while (1)
		{
			int n = lwip_recv(cs, rbuf, sizeof(rbuf), MSG_DONTWAIT);
			size_t olen = 0;
			int rc;

			/*
			 * ★ 用 MSG_DONTWAIT 而不是设 SO_RCVTIMEO：
			 *   这个 SDK 的 lwIP 没有把 errno/EAGAIN 暴露出来
			 *   （sockets.h 里压根没有 EAGAIN 的宏），所以没法区分
			 *   "超时"和"真出错"。改成每次 recv 都不阻塞 + 主动 delay，
			 *   既不用 errno，又能周期性地回头看有没有事件要推。
			 *
			 *   n == 0 : 对端关了连接
			 *   n <  0 : 这次没数据（或出错）—— 都交给下面的事件轮询处理
			 */
			if (n == 0)
				break;

			if (n > 0)
			{
				rc = hk_conn_feed(&g_conn, rbuf, (size_t)n, obuf, &olen,
								  sizeof(obuf));
				if (olen > 0)
				{
					if (lwip_send(cs, obuf, olen, 0) < 0)
						break;
				}
				if (rc != HK_NET_OK)
					break;
			}

			/* 主动推送：按键/MQTT/Web 那边改了继电器状态 */
			(void)hk_net_push_events(cs);

			if (g_conn.dead)
				break;

			if (n > 0)
				continue; /* 还有数据就别睡，赶紧收 */

			hf_thread_delay(50); /* 20Hz 轮询，够快又不占 CPU */
		}

		lwip_close(cs);
		hf_thread_delay(50);
	}
}

/* ---- mDNS ---- */
static void hk_mdns_announce(void)
{
	memset(&g_mdns_svc, 0, sizeof(g_mdns_svc));

	g_mdns_svc.servname = g_mdns_serv;
	g_mdns_svc.servtype = "_hap";
	g_mdns_svc.domain = "local";
	g_mdns_svc.port = lwip_htons(HK_NET_PORT);
	g_mdns_svc.proto = MDNS_PROTO_TCP;
	g_mdns_svc.keyvals = g_mdns_txt; /* 会被 mDNS 改写，所以每次重建 */

	/* 先重建 TXT：c# 与 sf 会随配对状态变 */
	if (hk_mdns_txt(g_mdns_txt, sizeof(g_mdns_txt), g_acc_id,
					g_cfg_num, g_store.valid && g_store.npeers > 0, "TC1") == 0)
		return;

	mdns_announce_service(&g_mdns_svc, NULL);
}

/* ---- 对外入口 ---- */
int hk_net_start(const char *display_name)
{
	char mac[18];
	int i;

	/* 1) 拿 MAC 当配件 ID（HAP 要求 id 就是 MAC，形如 XX:XX:XX:XX:XX:XX） */
	memset(mac, 0, sizeof(mac));
	hfnet_get_mac_address(mac);
	/* /status 里的是不带冒号的 "F0FE6BE67882"，转成带冒号 */
	{
		const char *src = mac;
		char *dst = g_acc_id;
		int n = 0;
		for (i = 0; src[i] && n < 12; i++)
		{
			if ((src[i] >= '0' && src[i] <= '9') || (src[i] >= 'A' && src[i] <= 'F') ||
				(src[i] >= 'a' && src[i] <= 'f'))
			{
				if (n > 0 && (n % 2 == 0))
					*dst++ = ':';
				*dst++ = src[i];
				n++;
			}
		}
		*dst = '\0';
	}

	/* 2) 读回持久化。失败 = 没配过，重新生成身份与配对码并立刻落盘 */
	hk_store_clear(&g_store);
	if (hk_persist_load(&g_store) != 0)
	{
		hk_store_clear(&g_store);
		hk_store_ensure_identity(&g_store, hk_rand, NULL);
		g_store.config_number = 1;
		g_store.valid = 1;

		/*
		 * 首次启动用 TRNG 现生成一个码。
		 * 排插没有屏幕，这个码只能在 Web 后台看到 —— 所以必须是随机的：
		 * 写死成常量等于任何人拿到同型号设备都能配对。
		 * 生成失败就退到 HK_CODE_DEFAULT（它是个合法码，只是公知）。
		 */
		if (hk_code_random(g_store.setup_code, sizeof(g_store.setup_code),
						   hk_rand, NULL) != 0)
			snprintf(g_store.setup_code, sizeof(g_store.setup_code),
					 "%s", HK_CODE_DEFAULT);

		if (hk_persist_save(&g_store) != 0)
			return -1; /* 存不下就别起来：身份不持久 = 每次重启都要重配对 */
	}
	else if (!g_store.setup_code[0] || !hk_code_valid(g_store.setup_code))
	{
		/*
		 * 老版本（v1）存下来的没有配对码字段，或存了个非法值。
		 * 补一个随机码再落盘 —— 不动 LTSK/LTPK，已配对的 iPhone 不受影响。
		 */
		if (hk_code_random(g_store.setup_code, sizeof(g_store.setup_code),
						   hk_rand, NULL) != 0)
			snprintf(g_store.setup_code, sizeof(g_store.setup_code),
					 "%s", HK_CODE_DEFAULT);
		(void)hk_persist_save(&g_store);
	}
	g_cfg_num = g_store.config_number;

	/* 3) 配件身份 */
	g_acc.id = g_acc_id;
	memcpy(g_acc.ltsk, g_store.ltsk, 32);
	memcpy(g_acc.ltpk, g_store.ltpk, 32);

	/* 4) mDNS */
	snprintf(g_mdns_host, sizeof(g_mdns_host), "TC1-%s", g_acc_id + 9);
	snprintf(g_mdns_serv, sizeof(g_mdns_serv), "%s",
			 (display_name != NULL) ? display_name : "TC1");
	for (i = 0; g_mdns_host[i]; i++)
		if (g_mdns_host[i] == ':')
			g_mdns_host[i] = '-';

	if (mdns_start(NULL, g_mdns_host) == MDNS_SUCCESS)
		hk_mdns_announce();

	/* 5) 监听线程。栈给足：SRP 的 3072 位模幂 + 4KB 连接缓冲都在这条栈上 */
	{
		hfthread_hande_t th;
		if (hfthread_create(hk_net_thread, "hap", 4096, NULL,
							HFTHREAD_PRIORITIES_MID, &th, NULL) != HF_SUCCESS)
			return -1;
	}
	return 0;
}

const HK_STORE *hk_net_store(void)
{
	return &g_store;
}

int hk_net_is_paired(void)
{
	return (g_store.valid && g_store.npeers > 0) ? 1 : 0;
}

const char *hk_net_get_code(void)
{
	if (!g_net_up)
		return "";
	return g_store.setup_code[0] ? g_store.setup_code : "";
}

int hk_net_set_code(const char *code)
{
	char norm[HK_CODE_BUF];

	if (code == NULL)
		return -1;
	if (hk_code_normalize(code, norm, sizeof(norm)) != 0)
		return -1;
	if (!hk_code_valid(norm))
		return -1;

	/*
	 * 改码 = 恢复出厂：把身份和控制器列表一起重置。
	 * 理由写在 hk_net.h 的声明处 —— HAP 配对成功后就不再校验配对码，
	 * 只清控制器列表不换身份的话，语义会变成"码变了但老设备还能用"。
	 */
	hk_store_clear(&g_store);
	hk_store_ensure_identity(&g_store, hk_rand, NULL);
	g_store.config_number = (g_cfg_num > 0) ? g_cfg_num + 1 : 2;
	g_store.valid = 1;
	snprintf(g_store.setup_code, sizeof(g_store.setup_code), "%s", norm);

	if (hk_persist_save(&g_store) != 0)
		return -2;

	g_cfg_num = g_store.config_number;

	/* 身份换了，正在跑的连接必须断掉：它的 LTSK 已经和新身份对不上 */
	g_conn.dead = 1;
	memcpy(g_acc.ltsk, g_store.ltsk, 32);
	memcpy(g_acc.ltpk, g_store.ltpk, 32);

	/* c# 与 sf 都变了，重新广播，否则 iOS 还按旧记录连 */
	if (g_net_up)
		hk_mdns_announce();
	return 0;
}

int hk_net_gen_code(char *out, size_t cap)
{
	char code[HK_CODE_BUF];

	if (hk_code_random(code, sizeof(code), hk_rand, NULL) != 0)
		return -1;
	if (hk_net_set_code(code) != 0)
		return -2;
	if (out != NULL && cap > 0)
		snprintf(out, cap, "%s", code);
	return 0;
}

int hk_net_unpair_all(void)
{
	int i;
	int n = g_store.npeers;

	/* 逐个删，让 hk_store 自己维护 c#（它会在每次删时递增） */
	for (i = n - 1; i >= 0; i--)
		hk_store_peer_del(&g_store, g_store.peers[i].id);

	g_cfg_num = g_store.config_number;
	if (hk_persist_save(&g_store) != 0)
		return -1;
	if (g_net_up)
		hk_mdns_announce();
	return 0;
}

void hk_net_notify(uint8_t idx, int v)
{
	if (idx >= HK_NR_OUTLET)
		return;
	g_evt_val[idx] = v ? 1 : 0;
	g_evt_pending |= (uint8_t)(1u << idx);
}

#endif /* HK_NET_PLATFORM */

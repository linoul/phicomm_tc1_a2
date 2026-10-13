/*
 * hk_hap.c —— HAP 会话层实现（帧 / HTTP / 端点分发）
 *
 * 规范依据见 hk_hap.h 顶部三条（aad、nonce、分片），
 * 全部对着 HAP-NodeJS 的 hapCrypto.ts 核对过。
 */
#include "hk_hap.h"

#include <string.h>

/* ================================================================== */
/* 帧                                                                  */
/* ================================================================== */

static void frame_nonce(uint64_t cnt, uint8_t nonce[12])
{
	int i;
	memset(nonce, 0, 4);
	for (i = 0; i < 8; i++)
		nonce[4 + i] = (uint8_t)(cnt >> (8 * i)); /* 小端 */
}

void hk_session_begin(HK_SESSION *s, const uint8_t a2c[32], const uint8_t c2a[32])
{
	if (s == NULL)
		return;
	if (a2c)
		memcpy(s->a2c, a2c, 32);
	if (c2a)
		memcpy(s->c2a, c2a, 32);
	s->a2c_count = 0;
	s->c2a_count = 0;
	s->up = 1;
}

size_t hk_frame_encrypted_size(size_t pt_len)
{
	size_t frames = (pt_len + HK_HAP_FRAG_MAX - 1) / HK_HAP_FRAG_MAX;
	if (pt_len == 0)
		frames = 1;
	return pt_len + frames * HK_HAP_OVERHEAD;
}

int hk_frame_encrypt(HK_SESSION *s, uint8_t *out, size_t out_cap,
					 const uint8_t *pt, size_t pt_len)
{
	size_t off = 0, p = 0;

	if (s == NULL || !s->up || out == NULL)
		return -1;
	if (hk_frame_encrypted_size(pt_len) > out_cap)
		return -1;

	while (off < pt_len || (pt_len == 0 && p == 0))
	{
		size_t chunk = pt_len - off;
		uint8_t aad[HK_HAP_HDR_LEN];
		uint8_t nonce[12];

		if (chunk > HK_HAP_FRAG_MAX)
			chunk = HK_HAP_FRAG_MAX;

		aad[0] = (uint8_t)(chunk & 0xff);
		aad[1] = (uint8_t)((chunk >> 8) & 0xff);
		out[p] = aad[0];
		out[p + 1] = aad[1];

		frame_nonce(s->a2c_count, nonce);
		hk_aead_encrypt(out + p + HK_HAP_HDR_LEN,
						out + p + HK_HAP_HDR_LEN + chunk,
						s->a2c, nonce, aad, HK_HAP_HDR_LEN,
						pt + off, chunk);
		s->a2c_count++;

		p += HK_HAP_HDR_LEN + chunk + HK_HAP_TAG_LEN;
		off += chunk;
		if (pt_len == 0)
			break;
	}
	return (int)p;
}

int hk_frame_decrypt(HK_SESSION *s, const uint8_t *in, size_t in_len,
					 size_t *consumed, uint8_t *out, size_t out_cap)
{
	size_t p = 0, w = 0;

	if (consumed)
		*consumed = 0;
	if (s == NULL || !s->up || in == NULL || out == NULL)
		return -1;

	while (in_len - p >= HK_HAP_OVERHEAD)
	{
		size_t len = (size_t)in[p] | ((size_t)in[p + 1] << 8);
		uint8_t nonce[12];

		if (len > HK_HAP_FRAG_MAX)
			return -1; /* 格式错：长度头声称超过一帧上限 */
		if (in_len - p - HK_HAP_HDR_LEN - HK_HAP_TAG_LEN < len)
			break; /* 半包，等下一次数据 */
		if (w + len > out_cap)
			return -1; /* 输出缓冲不够 */

		frame_nonce(s->c2a_count, nonce);
		if (hk_aead_decrypt(out + w, in + p + HK_HAP_HDR_LEN + len,
							s->c2a, nonce,
							in + p, HK_HAP_HDR_LEN,
							in + p + HK_HAP_HDR_LEN, len) != 0)
			return -1; /* 认证失败：连接必须断开 */

		s->c2a_count++;
		w += len;
		p += HK_HAP_HDR_LEN + len + HK_HAP_TAG_LEN;
	}

	if (consumed)
		*consumed = p;
	return (int)w;
}

/* ================================================================== */
/* HTTP                                                                */
/* ================================================================== */

static int ieq(const uint8_t *p, size_t len, size_t at, const char *s)
{
	size_t i;
	for (i = 0; s[i] != '\0'; i++)
	{
		if (at + i >= len)
			return 0;
		if ((p[at + i] | 0x20) != (uint8_t)(s[i] | 0x20))
			return 0;
	}
	return 1;
}

/* 大小写不敏感地在 header 区找 "name: "，返回其后第一个非空格位置；找不到返回 -1 */
static long find_header(const uint8_t *p, size_t len, const char *name)
{
	size_t i = 0;
	int line_start = 1;
	while (i < len)
	{
		if (line_start)
		{
			if (ieq(p, len, i, name))
			{
				size_t j = i + strlen(name);
				if (j < len && p[j] == ':')
				{
					j++;
					while (j < len && (p[j] == ' ' || p[j] == '\t'))
						j++;
					return (long)j;
				}
			}
			line_start = 0;
		}
		if (p[i] == '\n')
			line_start = 1;
		i++;
	}
	return -1;
}

int hk_http_parse(const uint8_t *buf, size_t len, HK_HTTP_REQ *out)
{
	size_t i = 0, hs, body_at = 0;
	size_t mlen;

	if (buf == NULL || out == NULL || len < 16)
		return -1;

	memset(out, 0, sizeof(*out));

	/* method */
	if (ieq(buf, len, 0, "GET"))
		out->method = HK_HTTP_GET, mlen = 3;
	else if (ieq(buf, len, 0, "PUT"))
		out->method = HK_HTTP_PUT, mlen = 3;
	else if (ieq(buf, len, 0, "POST"))
		out->method = HK_HTTP_POST, mlen = 4;
	else
		return -1;

	i = mlen;
	while (i < len && buf[i] == ' ')
		i++;

	/* path（到空格或 '?' 为止） */
	hs = 0;
	while (i < len && buf[i] != ' ' && buf[i] != '?' && buf[i] != '\r' && buf[i] != '\n')
	{
		if (hs + 1 < HK_HTTP_PATH_MAX)
			out->path[hs++] = (char)buf[i];
		i++;
	}
	out->path[hs] = '\0';

	/* query（到空格为止） */
	if (i < len && buf[i] == '?')
	{
		i++;
		hs = 0;
		while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n')
		{
			if (hs + 1 < HK_HTTP_QUERY_MAX)
				out->query[hs++] = (char)buf[i];
			i++;
		}
		out->query[hs] = '\0';
	}

	/* 找 header 结束 */
	{
		size_t j = 0;
		int found = 0;
		while (j + 3 < len)
		{
			if (buf[j] == '\r' && buf[j + 1] == '\n' && buf[j + 2] == '\r' && buf[j + 3] == '\n')
			{
				body_at = j + 4;
				found = 1;
				break;
			}
			if (buf[j] == '\n' && buf[j + 1] == '\n')
			{
				body_at = j + 2;
				found = 1;
				break;
			}
			j++;
		}
		if (!found)
			return -1; /* 头还没收全 */
	}

	/* Content-Length */
	{
		long at = find_header(buf, len, "Content-Length");
		if (at >= 0)
		{
			size_t j = (size_t)at;
			long v = 0;
			int digits = 0;
			while (j < len && buf[j] >= '0' && buf[j] <= '9')
			{
				v = v * 10 + (buf[j] - '0');
				if (v > 1000000)
					return -1;
				digits++;
				j++;
			}
			if (digits == 0)
				return -1;
			out->content_length = (size_t)v;
		}
	}

	if (body_at <= len)
	{
		size_t avail = len - body_at;
		out->body = buf + body_at;
		out->body_len = avail < out->content_length ? avail : out->content_length;
	}
	return 0;
}

/* 手写状态行 + 头，避免依赖 snprintf（newlib-nano 的浮点/格式支持不定） */
static size_t put_int(char *p, int v)
{
	char tmp[12];
	size_t n = 0, k;
	int neg = 0;
	int32_t x;
	if (v < 0)
	{
		neg = 1;
		x = -(int32_t)(v + 1);
		x += 1;
	}
	else
		x = v;
	if (x == 0)
		tmp[n++] = '0';
	while (x > 0)
	{
		tmp[n++] = (char)('0' + (int)(x % 10));
		x /= 10;
	}
	k = 0;
	if (neg)
		p[k++] = '-';
	while (n > 0)
		p[k++] = tmp[--n];
	return k;
}

static const char *status_text(int st)
{
	switch (st)
	{
	case 200: return "OK";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 429: return "Too Many Requests";
	case 500: return "Internal Server Error";
	case 470: return "Connection Authorization Required";
	default: return "Unknown";
	}
}

/* 响应头的最大长度。状态行 + Content-Type + Content-Length 撑死也就 80 多字节，
 * 96 足够；这个值同时被 hk_hap_handle 用作"预留给头的空间"（见 finish_with_body）。 */
#define HK_HDR_MAX 96

/*
 * 构造完整的响应头到 hdr（不碰 body）。
 * body_len 为 0 时只写状态行 + 空行（204 就是这种），不带 Content-* 头。
 * 返回头长度；0 = hcap 不够。
 */
static size_t build_header(char *hdr, size_t hcap, int status,
						   const char *ctype, size_t body_len)
{
	size_t h = 0, i;

	if (hdr == NULL || hcap < 16)
		return 0;

	if (hcap < 9 + 5 + 2)
		return 0;
	memcpy(hdr + h, "HTTP/1.1 ", 9);
	h += 9;
	h += put_int(hdr + h, status);
	if (h + 1 >= hcap)
		return 0;
	hdr[h++] = ' ';
	{
		const char *t = status_text(status);
		for (i = 0; t[i] != '\0' && h + 2 < hcap; i++)
			hdr[h++] = t[i];
	}
	if (h + 2 >= hcap)
		return 0;
	hdr[h++] = '\r';
	hdr[h++] = '\n';

	if (body_len == 0 || ctype == NULL)
	{
		if (h + 2 >= hcap)
			return 0;
		hdr[h++] = '\r';
		hdr[h++] = '\n';
		return h;
	}

	if (h + 14 >= hcap)
		return 0;
	memcpy(hdr + h, "Content-Type: ", 14);
	h += 14;
	for (i = 0; ctype[i] != '\0' && h + 18 < hcap; i++)
		hdr[h++] = ctype[i];
	hdr[h++] = '\r';
	hdr[h++] = '\n';

	if (h + 16 >= hcap)
		return 0;
	memcpy(hdr + h, "Content-Length: ", 16);
	h += 16;
	h += put_int(hdr + h, (int)body_len);
	if (h + 4 >= hcap)
		return 0;
	hdr[h++] = '\r';
	hdr[h++] = '\n';
	hdr[h++] = '\r';
	hdr[h++] = '\n';
	return h;
}

size_t hk_http_response(uint8_t *out, size_t cap, int status,
						const char *ctype, const uint8_t *body, size_t body_len)
{
	char hdr[HK_HDR_MAX];
	size_t h;

	if (out == NULL)
		return 0;
	if (body == NULL)
		body_len = 0;

	h = build_header(hdr, sizeof(hdr), status, ctype, body_len);
	if (h == 0)
		return 0;
	if (h + body_len > cap)
		return 0;
	memcpy(out, hdr, h);
	if (body_len)
		memcpy(out + h, body, body_len);
	return h + body_len;
}

size_t hk_event_response(uint8_t *out, size_t cap,
						 const uint8_t *body, size_t body_len)
{
	char hdr[HK_HDR_MAX];
	size_t h = 0, i;
	const char *ctype = "application/hap+json";

	if (out == NULL || cap < 32)
		return 0;
	if (body == NULL)
		body_len = 0;

	/* "EVENT/1.0 200 OK\r\n" = 18 字节。别写成 HTTP/1.1，理由见头文件。 */
	if (sizeof(hdr) < 18 + 14 + 20 + 16 + 8)
		return 0;
	memcpy(hdr + h, "EVENT/1.0 200 OK\r\n", 18);
	h += 18;

	memcpy(hdr + h, "Content-Type: ", 14);
	h += 14;
	for (i = 0; ctype[i] != '\0' && h + 4 < sizeof(hdr); i++)
		hdr[h++] = ctype[i];
	hdr[h++] = '\r';
	hdr[h++] = '\n';

	memcpy(hdr + h, "Content-Length: ", 16);
	h += 16;
	h += put_int(hdr + h, (int)body_len);
	hdr[h++] = '\r';
	hdr[h++] = '\n';
	hdr[h++] = '\r';
	hdr[h++] = '\n';

	if (h + body_len > cap)
		return 0;
	memcpy(out, hdr, h);
	if (body_len)
		memcpy(out + h, body, body_len);
	return h + body_len;
}

/* ================================================================== */
/* 端点分发                                                            */
/* ================================================================== */

static void ctx_rand(HK_HAP_CTX *c, uint8_t *buf, size_t n)
{
	if (c == NULL || buf == NULL)
		return;
	if (c->rand != NULL)
		c->rand(buf, n, c->rand_ctx);
	else
		memset(buf, 0, n);
}

/*
 * 把已经写在 out+reserve 处的 body 前面"塞进"HTTP 头。
 * 这样整个过程不需要第二块缓冲 —— out 通常是 1KB 以上，
 * 拿前 96 字节当头的落脚点最省内存。
 */
#define HK_HAP_HDR_RESERVE HK_HDR_MAX

/*
 * 把已经生成在 out + body_at 处的响应体前面"塞进"HTTP 头。
 *
 * 为什么要这么绕：out 是同一块缓冲，头和体在同一块内存里会重叠，
 * 所以必须**先把体往前挪，再写头**（顺序反了就会覆盖掉体的开头）。
 * 换来的是整个 handle 过程不需要第二块缓冲 —— 在 MCU 上这很值钱。
 */
static size_t finish_with_body(uint8_t *out, size_t cap, int status,
							   const char *ctype, size_t body_at, size_t body_len)
{
	char hdr[HK_HDR_MAX];
	size_t h = build_header(hdr, sizeof(hdr), status, ctype, body_len);

	if (h == 0)
		return 0;
	if (h + body_len > cap)
		return 0;
	if (body_len > 0 && body_at != h)
		memmove(out + h, out + body_at, body_len);
	memcpy(out, hdr, h);
	return h + body_len;
}

int hk_hap_parse_ids(const char *query, HK_JITEM *out, int max)
{
	int n = 0;
	const char *p;

	if (query == NULL || out == NULL || max <= 0)
		return 0;

	p = strstr(query, "id=");
	if (p == NULL)
		return 0;
	p += 3;

	while (*p != '\0' && n < max)
	{
		int32_t aid = 0, iid = 0;
		int dig;
		for (dig = 0; *p >= '0' && *p <= '9'; p++, dig++)
			aid = aid * 10 + (*p - '0');
		if (dig == 0)
			break;
		if (*p != '.')
			break;
		p++;
		for (dig = 0; *p >= '0' && *p <= '9'; p++, dig++)
			iid = iid * 10 + (*p - '0');
		if (dig == 0)
			break;
		memset(&out[n], 0, sizeof(out[n]));
		out[n].aid = aid;
		out[n].iid = iid;
		out[n].vtype = HK_JVAL_NONE; /* 纯读 */
		n++;
		if (*p == ',')
			p++;
	}
	return n;
}

size_t hk_hap_handle(HK_HAP_CTX *c, const HK_HTTP_REQ *req,
					 uint8_t *out, size_t cap)
{
	if (c == NULL || req == NULL || out == NULL)
		return 0;

	/* ---------------- 未加密端点 ---------------- */
	if (strcmp(req->path, "/pair-setup") == 0)
	{
		uint8_t salt[16], b[32];
		uint8_t state = 0;
		size_t tlv_at = HK_HAP_HDR_RESERVE;
		size_t tlv_len = 0;
		size_t tlv_cap = (cap > tlv_at) ? (cap - tlv_at) : 0;
		int rc;

		if (req->method != HK_HTTP_POST || c->setup == NULL || c->acc == NULL)
			return hk_http_response(out, cap, 400, NULL, NULL, 0);

		if (hk_tlv_find(req->body, req->body_len, HK_TLV_STATE, &state, 1) != 1)
			return hk_http_response(out, cap, 400, NULL, NULL, 0);

		if (state == 1)
		{
			ctx_rand(c, salt, sizeof(salt));
			ctx_rand(c, b, sizeof(b));
			rc = hk_setup_m1_to_m2(c->setup, c->setup_code, salt, b,
								   out + tlv_at, &tlv_len, tlv_cap);
		}
		else if (state == 3)
		{
			uint8_t A[HK_SRP_N_LEN];
			uint8_t proof[HK_SRP_K_LEN];
			if (hk_tlv_find(req->body, req->body_len, HK_TLV_PUBLICKEY, A, sizeof(A)) != HK_SRP_N_LEN ||
				hk_tlv_find(req->body, req->body_len, HK_TLV_PASSWORD_PROOF, proof, sizeof(proof)) != HK_SRP_K_LEN)
				return hk_http_response(out, cap, 400, NULL, NULL, 0);
			rc = hk_setup_m3_to_m4(c->setup, A, proof, out + tlv_at, &tlv_len, tlv_cap);
		}
		else if (state == 5)
		{
			uint8_t enc[256];
			int n = hk_tlv_find(req->body, req->body_len, HK_TLV_ENCRYPTEDDATA, enc, sizeof(enc));
			if (n <= 0)
				return hk_http_response(out, cap, 400, NULL, NULL, 0);
			rc = hk_setup_m5_to_m6(c->setup, c->acc, enc, (size_t)n,
								   out + tlv_at, &tlv_len, tlv_cap);
			if (rc == HK_PAIR_OK)
			{
				/* 调用方负责持久化：这里只把结果留在 ctx 里 */
				memcpy(c->ios_ltpk, c->setup->ios_ltpk, 32);
				c->paired = 1;
			}
		}
		else
			return hk_http_response(out, cap, 400, NULL, NULL, 0);

		if (rc != HK_PAIR_OK)
		{
			/* hk_pair 已经把错误 TLV 写进缓冲了 */
			if (tlv_len == 0)
				return hk_http_response(out, cap, 500, NULL, NULL, 0);
			return finish_with_body(out, cap, 200, "application/pairing+tlv8", tlv_at, tlv_len);
		}
		return finish_with_body(out, cap, 200, "application/pairing+tlv8", tlv_at, tlv_len);
	}

	if (strcmp(req->path, "/pair-verify") == 0)
	{
		uint8_t eph[32];
		uint8_t state = 0;
		size_t tlv_at = HK_HAP_HDR_RESERVE;
		size_t tlv_len = 0;
		size_t tlv_cap = (cap > tlv_at) ? (cap - tlv_at) : 0;
		int rc;

		if (req->method != HK_HTTP_POST || c->verify == NULL || c->acc == NULL)
			return hk_http_response(out, cap, 400, NULL, NULL, 0);
		if (hk_tlv_find(req->body, req->body_len, HK_TLV_STATE, &state, 1) != 1)
			return hk_http_response(out, cap, 400, NULL, NULL, 0);

		if (state == 1)
		{
			uint8_t cpub[32];
			if (hk_tlv_find(req->body, req->body_len, HK_TLV_PUBLICKEY, cpub, sizeof(cpub)) != 32)
				return hk_http_response(out, cap, 400, NULL, NULL, 0);
			ctx_rand(c, eph, sizeof(eph));
			rc = hk_verify_m1_to_m2(c->verify, c->acc, cpub, eph,
									out + tlv_at, &tlv_len, tlv_cap);
		}
		else if (state == 3)
		{
			uint8_t enc[256];
			int n = hk_tlv_find(req->body, req->body_len, HK_TLV_ENCRYPTEDDATA, enc, sizeof(enc));
			if (n <= 0)
				return hk_http_response(out, cap, 400, NULL, NULL, 0);
			rc = hk_verify_m3_to_m4(c->verify, c->paired ? c->ios_ltpk : NULL, enc, (size_t)n,
									out + tlv_at, &tlv_len, tlv_cap);
			if (rc == HK_PAIR_OK && c->sess != NULL)
				hk_session_begin(c->sess, c->verify->a2c, c->verify->c2a);
		}
		else
			return hk_http_response(out, cap, 400, NULL, NULL, 0);

		if (rc != HK_PAIR_OK && tlv_len == 0)
			return hk_http_response(out, cap, 500, NULL, NULL, 0);
		return finish_with_body(out, cap, 200, "application/pairing+tlv8", tlv_at, tlv_len);
	}

	if (strcmp(req->path, "/pairings") == 0)
	{
		/* Add/Remove/List Pairings 是可选项，本实现不支持 */
		size_t tlv_at = HK_HAP_HDR_RESERVE;
		size_t tlv_len = 0;
		if (cap <= tlv_at)
			return 0;
		hk_tlv_put_error(out + tlv_at, &tlv_len, cap - tlv_at, 2, HK_ERR_UNAVAILABLE);
		return finish_with_body(out, cap, 200, "application/pairing+tlv8", tlv_at, tlv_len);
	}

	if (strcmp(req->path, "/identify") == 0)
	{
		if (req->method != HK_HTTP_POST)
			return hk_http_response(out, cap, 405, NULL, NULL, 0);
		if (c->io != NULL && c->io->identify != NULL)
			c->io->identify(c->io->ctx);
		return hk_http_response(out, cap, 204, NULL, NULL, 0);
	}

	/* ---------------- 加密端点：必须有会话 ---------------- */
	if (c->sess == NULL || !c->sess->up)
	{
		static const char unauth[] = "{\"status\":-70401}";
		return hk_http_response(out, cap, 401, "application/hap+json",
								(const uint8_t *)unauth, sizeof(unauth) - 1);
	}

	if (strcmp(req->path, "/accessories") == 0)
	{
		size_t body_at = HK_HAP_HDR_RESERVE;
		size_t body_len;
		if (req->method != HK_HTTP_GET || c->db == NULL || cap <= body_at)
			return hk_http_response(out, cap, 400, NULL, NULL, 0);
		body_len = hk_acc_render_accessories(c->db, c->io, out + body_at, cap - body_at);
		if (body_len == 0)
			return hk_http_response(out, cap, 500, NULL, NULL, 0);
		return finish_with_body(out, cap, 200, "application/hap+json", body_at, body_len);
	}

	if (strcmp(req->path, "/characteristics") == 0)
	{
		HK_JITEM items[12];
		int n = 0;
		size_t body_at = HK_HAP_HDR_RESERVE;
		size_t body_len;

		if (c->db == NULL || cap <= body_at)
			return hk_http_response(out, cap, 400, NULL, NULL, 0);

		if (req->method == HK_HTTP_GET)
			n = hk_hap_parse_ids(req->query, items, 12);
		else if (req->method == HK_HTTP_PUT)
			n = hk_json_scan_writes(req->body, req->body_len, items, 12);
		else
			return hk_http_response(out, cap, 405, NULL, NULL, 0);

		if (n <= 0)
			return hk_http_response(out, cap, 400, NULL, NULL, 0);

		body_len = hk_acc_render_characteristics(c->db, c->io, items, n,
												 out + body_at, cap - body_at);
		if (body_len == 0)
			return hk_http_response(out, cap, 500, NULL, NULL, 0);
		return finish_with_body(out, cap, 200, "application/hap+json", body_at, body_len);
	}

	return hk_http_response(out, cap, 404, NULL, NULL, 0);
}

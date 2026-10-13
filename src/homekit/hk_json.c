/*
 * hk_json.c —— 极简 JSON 实现（见 hk_json.h 的设计取舍说明）
 */
#include "hk_json.h"

#include <string.h>

/* ---- 内部：写入若干字节，容量不足则置 overflow ---- */
static void jput(HK_JSON_OUT *j, const char *s, size_t n)
{
	size_t i;
	if (j == NULL || j->overflow)
		return;
	if (j->pos + n > j->cap)
	{
		j->overflow = 1;
		return;
	}
	for (i = 0; i < n; i++)
		j->buf[j->pos + i] = (uint8_t)s[i];
	j->pos += n;
}

static void jputc(HK_JSON_OUT *j, char c)
{
	jput(j, &c, 1);
}

/* 进入容器前先补逗号（同一层的第二个及以后元素） */
static void jpre(HK_JSON_OUT *j)
{
	if (j == NULL || j->overflow)
		return;
	if (j->level >= 0 && j->need_comma[j->level])
		jputc(j, ',');
}

static void jpost(HK_JSON_OUT *j)
{
	if (j == NULL || j->overflow)
		return;
	if (j->level >= 0)
		j->need_comma[j->level] = 1;
}

void hk_json_init(HK_JSON_OUT *j, uint8_t *buf, size_t cap)
{
	if (j == NULL)
		return;
	j->buf = buf;
	j->pos = 0;
	j->cap = cap;
	j->overflow = 0;
	j->level = -1;
	memset(j->need_comma, 0, sizeof(j->need_comma));
}

void hk_json_obj_begin(HK_JSON_OUT *j)
{
	if (j == NULL || j->overflow)
		return;
	jpre(j);
	jputc(j, '{');
	jpost(j);
	if (j->level + 1 < HK_JSON_MAX_DEPTH)
	{
		j->level++;
		j->need_comma[j->level] = 0;
	}
	else
		j->overflow = 1; /* 层数超上限：宣布失败，避免写坏 */
}

void hk_json_obj_end(HK_JSON_OUT *j)
{
	if (j == NULL || j->overflow)
		return;
	if (j->level >= 0)
		j->level--;
	jputc(j, '}');
}

void hk_json_arr_begin(HK_JSON_OUT *j)
{
	if (j == NULL || j->overflow)
		return;
	jpre(j);
	jputc(j, '[');
	jpost(j);
	if (j->level + 1 < HK_JSON_MAX_DEPTH)
	{
		j->level++;
		j->need_comma[j->level] = 0;
	}
	else
		j->overflow = 1;
}

void hk_json_arr_end(HK_JSON_OUT *j)
{
	if (j == NULL || j->overflow)
		return;
	if (j->level >= 0)
		j->level--;
	jputc(j, ']');
}

void hk_json_key(HK_JSON_OUT *j, const char *k)
{
	if (j == NULL || j->overflow)
		return;
	jpre(j);
	jputc(j, '"');
	jput(j, k, strlen(k));
	jput(j, "\":", 2);
	/*
	 * 关键：key 后面紧跟 value，中间不能有逗号。
	 * 所以这里把 need_comma 清 0 —— value 写完后由 jpost() 再置 1，
	 * 下一个 key 才会补逗号。（漏掉这步就会输出 {"a":,"b":} 这种非法 JSON。）
	 */
	if (j->level >= 0)
		j->need_comma[j->level] = 0;
}

void hk_json_raw(HK_JSON_OUT *j, const char *s)
{
	jpre(j);
	jput(j, s, strlen(s));
	jpost(j);
}

/* 字符串转义：只处理 " \ 和控制字符。UTF-8（中文名）原样输出，
 * HAP 要求 JSON 是 UTF-8，iOS 也接受未转义的中文。 */
void hk_json_str(HK_JSON_OUT *j, const char *s)
{
	size_t i, n;
	if (j == NULL || j->overflow)
		return;
	if (s == NULL)
	{
		jpre(j);
		jput(j, "null", 4);
		jpost(j);
		return;
	}
	jpre(j);
	jputc(j, '"');
	n = strlen(s);
	for (i = 0; i < n && !j->overflow; i++)
	{
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\')
		{
			char esc[2];
			esc[0] = '\\';
			esc[1] = (char)c;
			jput(j, esc, 2);
		}
		else if (c == '\n')
			jput(j, "\\n", 2);
		else if (c == '\r')
			jput(j, "\\r", 2);
		else if (c == '\t')
			jput(j, "\\t", 2);
		else if (c < 0x20)
		{
			static const char *hexd = "0123456789abcdef";
			char esc[6];
			esc[0] = '\\';
			esc[1] = 'u';
			esc[2] = '0';
			esc[3] = '0';
			esc[4] = hexd[(c >> 4) & 0xf];
			esc[5] = hexd[c & 0xf];
			jput(j, esc, 6);
		}
		else
			jputc(j, (char)c);
	}
	jputc(j, '"');
	jpost(j);
}

void hk_json_int(HK_JSON_OUT *j, int32_t v)
{
	char tmp[16];
	size_t n = 0;
	int32_t x;
	int neg;
	if (j == NULL || j->overflow)
		return;
	jpre(j);
	if (v < 0)
	{
		neg = 1;
		/* 小心 INT32_MIN：先转成正值域再取模 */
		x = -(v + 1);
		x += 1;
	}
	else
	{
		neg = 0;
		x = v;
	}
	if (x == 0)
		tmp[n++] = '0';
	while (x > 0)
	{
		tmp[n++] = (char)('0' + (int)(x % 10));
		x /= 10;
	}
	if (neg)
		jputc(j, '-');
	while (n > 0)
		jputc(j, tmp[--n]);
	jpost(j);
}

void hk_json_bool(HK_JSON_OUT *j, int v)
{
	jpre(j);
	if (v)
		jput(j, "true", 4);
	else
		jput(j, "false", 5);
	jpost(j);
}

void hk_json_null(HK_JSON_OUT *j)
{
	jpre(j);
	jput(j, "null", 4);
	jpost(j);
}

int hk_json_ok(const HK_JSON_OUT *j)
{
	return (j != NULL && !j->overflow) ? 1 : 0;
}

size_t hk_json_len(const HK_JSON_OUT *j)
{
	return (j == NULL) ? 0 : j->pos;
}

/* ================================================================== */
/* 读取侧：受限扫描器                                                  */
/* ================================================================== */

int hk_json_peek(HK_JSON_SCAN *s)
{
	if (s == NULL || s->pos >= s->len)
		return 0;
	return (int)s->p[s->pos];
}

void hk_json_skip_ws(HK_JSON_SCAN *s)
{
	while (s->pos < s->len)
	{
		char c = (char)s->p[s->pos];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			s->pos++;
		else
			break;
	}
}

/* 判断当前位置是否以字面量 key（如 "aid"）开头，且后面（跳过空白）是冒号。
 * 匹配成功时把 pos 推进到冒号之后。 */
static int match_key(HK_JSON_SCAN *s, const char *key)
{
	size_t save, klen;
	if (hk_json_peek(s) != '"')
		return 0;
	save = s->pos;
	s->pos++;
	klen = strlen(key);
	if (s->pos + klen > s->len || memcmp(s->p + s->pos, key, klen) != 0)
	{
		s->pos = save;
		return 0;
	}
	s->pos += klen;
	if (hk_json_peek(s) != '"')
	{
		s->pos = save;
		return 0;
	}
	s->pos++;
	hk_json_skip_ws(s);
	if (hk_json_peek(s) != ':')
	{
		s->pos = save;
		return 0;
	}
	s->pos++;
	hk_json_skip_ws(s);
	return 1;
}

/* 读一个数字（可选负号+整数）。返回 1 成功 */
static int scan_int(HK_JSON_SCAN *s, int32_t *out)
{
	int neg = 0;
	int32_t v = 0;
	int digits = 0;
	if (hk_json_peek(s) == '-')
	{
		neg = 1;
		s->pos++;
	}
	while (s->pos < s->len)
	{
		char c = (char)s->p[s->pos];
		if (c >= '0' && c <= '9')
		{
			if (v > 200000000)
				return 0; /* 明显过大，当作畸形 */
			v = v * 10 + (c - '0');
			digits++;
			s->pos++;
		}
		else
			break;
	}
	if (digits == 0)
		return 0;
	*out = neg ? -v : v;
	return 1;
}

/* 读一个字符串值（带引号），写入 dst。返回 1 成功 */
static int scan_string(HK_JSON_SCAN *s, char *dst, size_t cap)
{
	size_t n = 0;
	if (hk_json_peek(s) != '"')
		return 0;
	s->pos++;
	while (s->pos < s->len)
	{
		unsigned char c = s->p[s->pos];
		if (c == '"')
		{
			s->pos++;
			dst[n < cap ? n : cap - 1] = '\0';
			return 1;
		}
		if (c == '\\')
		{
			s->pos++;
			if (s->pos >= s->len)
				return 0;
			c = s->p[s->pos];
			s->pos++;
			switch (c)
			{
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u': /* 只处理 \uXXXX 的 ASCII 部分，其余吞掉 */
			{
				int k;
				unsigned int cp = 0;
				for (k = 0; k < 4 && s->pos < s->len; k++, s->pos++)
				{
					char h = (char)s->p[s->pos];
					unsigned int d;
					if (h >= '0' && h <= '9') d = (unsigned int)(h - '0');
					else if (h >= 'a' && h <= 'f') d = (unsigned int)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F') d = (unsigned int)(h - 'A' + 10);
					else return 0;
					cp = cp * 16 + d;
				}
				c = (unsigned char)(cp & 0x7f);
				break;
			}
			default: break;
			}
		}
		if (n + 1 < cap)
			dst[n++] = (char)c;
		else
			dst[cap - 1] = '\0'; /* 超长就丢尾部，不溢出 */
		s->pos++;
	}
	return 0; /* 字符串没闭合 */
}

/* 跳过一个任意值（用于跳掉不认识的键） */
static int skip_value(HK_JSON_SCAN *s)
{
	int c = hk_json_peek(s);
	if (c == '"')
	{
		char tmp[HK_JSON_STR_MAX];
		return scan_string(s, tmp, sizeof(tmp));
	}
	if (c == '{' || c == '[')
	{
		int depth = 0;
		while (s->pos < s->len)
		{
			unsigned char ch = s->p[s->pos];
			if (ch == '"')
			{
				char tmp[HK_JSON_STR_MAX];
				if (!scan_string(s, tmp, sizeof(tmp)))
					return 0;
				continue;
			}
			if (ch == '{' || ch == '[')
				depth++;
			else if (ch == '}' || ch == ']')
			{
				depth--;
				if (depth == 0)
				{
					s->pos++;
					return 1;
				}
			}
			s->pos++;
		}
		return 0;
	}
	/* 数字 / true / false / null */
	while (s->pos < s->len)
	{
		char ch = (char)s->p[s->pos];
		if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' ||
			ch == '\n' || ch == '\r')
			break;
		s->pos++;
	}
	return 1;
}

int hk_json_scan_writes(const uint8_t *buf, size_t len, HK_JITEM *out, int max)
{
	HK_JSON_SCAN s;
	int count = 0;
	int in_obj = 0;
	int have_aid = 0, have_iid = 0;
	HK_JITEM cur;

	if (buf == NULL || out == NULL || max <= 0)
		return 0;

	s.p = buf;
	s.len = len;
	s.pos = 0;
	memset(&cur, 0, sizeof(cur));

	while (s.pos < s.len)
	{
		unsigned char c = s.p[s.pos];

		if (c == '{')
		{
			s.pos++;
			in_obj = 1;
			have_aid = 0;
			have_iid = 0;
			memset(&cur, 0, sizeof(cur));
			continue;
		}
		if (c == '}')
		{
			s.pos++;
			if (in_obj && have_aid && have_iid && count < max)
			{
				out[count++] = cur;
			}
			in_obj = 0;
			continue;
		}
		if (!in_obj)
		{
			s.pos++;
			continue;
		}
		if (c == ',')
		{
			s.pos++;
			continue;
		}
		hk_json_skip_ws(&s);
		if (s.pos >= s.len)
			break;
		c = s.p[s.pos];

		if (match_key(&s, "aid"))
		{
			int32_t v;
			if (scan_int(&s, &v))
			{
				cur.aid = v;
				have_aid = 1;
			}
			continue;
		}
		if (match_key(&s, "iid"))
		{
			int32_t v;
			if (scan_int(&s, &v))
			{
				cur.iid = v;
				have_iid = 1;
			}
			continue;
		}
		if (match_key(&s, "value"))
		{
			int ch = hk_json_peek(&s);
			if (ch == 't' || ch == 'f')
			{
				cur.vtype = HK_JVAL_BOOL;
				cur.vb = (ch == 't') ? 1 : 0;
				while (s.pos < s.len && s.p[s.pos] >= 'a' && s.p[s.pos] <= 'z')
					s.pos++;
			}
			else if (ch == '"')
			{
				if (scan_string(&s, cur.vs, sizeof(cur.vs)))
					cur.vtype = HK_JVAL_STR;
			}
			else if (ch == 'n')
			{
				cur.vtype = HK_JVAL_NONE;
				while (s.pos < s.len && s.p[s.pos] >= 'a' && s.p[s.pos] <= 'z')
					s.pos++;
			}
			else
			{
				int32_t v;
				if (scan_int(&s, &v))
				{
					cur.vtype = HK_JVAL_INT;
					cur.vb = v;
				}
			}
			continue;
		}
		if (match_key(&s, "ev"))
		{
			int ch = hk_json_peek(&s);
			if (ch == 't' || ch == 'f')
			{
				cur.has_ev = 1;
				cur.ev = (ch == 't') ? 1 : 0;
				while (s.pos < s.len && s.p[s.pos] >= 'a' && s.p[s.pos] <= 'z')
					s.pos++;
			}
			else
			{
				int32_t v;
				if (scan_int(&s, &v))
				{
					cur.has_ev = 1;
					cur.ev = v ? 1 : 0;
				}
			}
			continue;
		}
		/*
		 * 不认识的键：跳到它的值**之前**就停手。
		 * 为什么不像通用解析器那样把整个值吞掉：HAP 的写请求长这样
		 *     {"characteristics":[{"aid":1,"iid":12,"value":true}]}
		 * 外层的 "characteristics" 正是"不认识的键"，它的值是**数组** ——
		 * 如果把数组吞了，里面真正要读的对象就一个都扫不到了
		 * （第一版就是这么错的，表现为"解析出 0 条"）。
		 * 所以只在值是标量时才跳过；遇到 '{' 或 '[' 就留给主循环去处理。
		 */
		if (c == '"')
		{
			s.pos++;
			while (s.pos < s.len && s.p[s.pos] != '"')
				s.pos++;
			if (s.pos < s.len)
				s.pos++;
			hk_json_skip_ws(&s);
			if (hk_json_peek(&s) == ':')
			{
				s.pos++;
				hk_json_skip_ws(&s);
				if (hk_json_peek(&s) != '{' && hk_json_peek(&s) != '[')
					skip_value(&s);
			}
			continue;
		}
		s.pos++;
	}

	return count;
}

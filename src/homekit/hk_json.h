/*
 * hk_json.h —— 给 HAP 用的极简 JSON（只写 + 受限读）
 *
 * 为什么不用现成的库：MCU 上塞不进通用 JSON 解析器（要动态分配、要递归），
 * 而 HAP 用到的 JSON 形状是**完全固定的** —— 只有 /accessories 的配件数据库
 * （往外写）和 /characteristics 的读写请求（往里读，键只有 aid/iid/value/ev）。
 *
 * 设计取舍：
 *   - 写入侧：一个"游标 + 容量"的结构，**溢出后永久进入只读状态**，
 *     后续所有写操作变成空操作。这样上层可以无脑往下写，最后用一个
 *     hk_json_ok() 判断成败 —— 比每个调用点都检查返回值可靠得多；
 *   - 读取侧：**受限扫描器**，只认 aid/iid/value/ev 四个键，遇到别的键跳过。
 *     它不是通用 JSON 解析器，**故意不假装是**（见 hk_json_scan 的说明）；
 *   - 全程零动态分配、零递归，栈固定。
 */
#ifndef HK_JSON_H
#define HK_JSON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 嵌套层数上限（HAP 的 JSON 最深就是 accessories>services>characteristics>perms） */
#define HK_JSON_MAX_DEPTH 8

/* 字符串值上限（Name / SerialNumber 之类），超出会被截断而不是溢出 */
#define HK_JSON_STR_MAX 64

typedef struct _HK_JSON_OUT
{
	uint8_t *buf;
	size_t pos;
	size_t cap;
	int overflow;				  /* 一旦置位，后续写全部丢弃 */
	int level;					  /* 当前嵌套层，-1 = 还没开写 */
	uint8_t need_comma[HK_JSON_MAX_DEPTH];
} HK_JSON_OUT;

void hk_json_init(HK_JSON_OUT *j, uint8_t *buf, size_t cap);
void hk_json_obj_begin(HK_JSON_OUT *j);
void hk_json_obj_end(HK_JSON_OUT *j);
void hk_json_arr_begin(HK_JSON_OUT *j);
void hk_json_arr_end(HK_JSON_OUT *j);

void hk_json_key(HK_JSON_OUT *j, const char *k);
void hk_json_str(HK_JSON_OUT *j, const char *s);
void hk_json_int(HK_JSON_OUT *j, int32_t v);
void hk_json_bool(HK_JSON_OUT *j, int v);
void hk_json_null(HK_JSON_OUT *j);

/* 直接塞一段已拼好的文本（只给 "pr" 这类常量用，不做任何转义） */
void hk_json_raw(HK_JSON_OUT *j, const char *s);

/* 溢出返回 0；长度是已写字节数（不含结尾 0，本实现也不写结尾 0） */
int hk_json_ok(const HK_JSON_OUT *j);
size_t hk_json_len(const HK_JSON_OUT *j);

/* ------------------------------------------------------------------ */

/* 解析出来的"写请求"条目 */
#define HK_JVAL_NONE 0 /* 没给 value（纯事件订阅） */
#define HK_JVAL_BOOL 1
#define HK_JVAL_INT 2
#define HK_JVAL_STR 3

typedef struct _HK_JITEM
{
	int32_t aid;
	int32_t iid;
	uint8_t vtype;
	int32_t vb;   /* HK_JVAL_BOOL: 0/1；HK_JVAL_INT: 数值 */
	char vs[HK_JSON_STR_MAX];
	int has_ev;   /* 请求里带了 "ev" */
	int ev;       /* ev 的值 */
} HK_JITEM;

typedef struct _HK_JSON_SCAN
{
	const uint8_t *p;
	size_t len;
	size_t pos;
} HK_JSON_SCAN;

/*
 * 扫描一条 /characteristics 请求体，把里面的 {aid,iid,value?,ev?} 抽出来。
 *
 * 这是**受限**解析器：
 *   - 只认 "aid"、"iid"、"value"、"ev" 四个键（HAP 的写请求就只有这些）；
 *   - 不校验整体 JSON 语法，遇到畸形输入尽量跳过而不是崩；
 *   - 只找**最外层 characteristics 数组里的对象**。因为 HAP 的数组只有一层，
 *     遇到 '{' 就开始收集、遇到 '}' 就收尾 —— 这对 HAP 的形状是充要的，
 *     但**不能**拿去解析任意 JSON（比如嵌套对象会串味）。
 *
 * 返回抽出的条目数（可能是 0）；-1 表示缓冲里有明显的畸形（如字符串未闭合）。
 * 最多写 max 条。
 */
int hk_json_scan_writes(const uint8_t *buf, size_t len, HK_JITEM *out, int max);

/* 跳过空白；返回当前字符（越界返回 0） */
int hk_json_peek(HK_JSON_SCAN *s);
void hk_json_skip_ws(HK_JSON_SCAN *s);

#ifdef __cplusplus
}
#endif

#endif /* HK_JSON_H */

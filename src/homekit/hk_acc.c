/*
 * hk_acc.c —— 6 路排插的 HAP 配件模型（见 hk_acc.h 的说明）
 */
#include "hk_acc.h"

#include <string.h>

/* ---- 小工具：把 HAP 类型渲染成短 UUID 字符串（大写 hex，无前导零） ---- */
static size_t type_str(uint16_t t, char *out)
{
	static const char *H = "0123456789ABCDEF";
	size_t n = 0;
	int started = 0;
	int shift;
	for (shift = 12; shift >= 0; shift -= 4)
	{
		int d = (t >> shift) & 0xf;
		if (d == 0 && !started && shift != 0)
			continue;
		started = 1;
		out[n++] = H[d];
	}
	if (!started)
		out[n++] = '0';
	return n;
}

static const char *fmt_name(uint8_t f)
{
	static const char *const names[] = {
		"bool", "uint8", "uint16", "uint32", "int32", "float", "string"};
	return (f <= HK_FMT_STRING) ? names[f] : "string";
}

/* ---- 静态数据库 ---- */

static const HK_CHAR g_info_chars[] = {
	{HK_IID_IDENTIFY, HK_CH_IDENTIFY, HK_PERM_PW, HK_FMT_BOOL, 0, 0, NULL, 0, 0, 0, 0},
	{HK_IID_MANUFACTURER, HK_CH_MANUFACTURER, HK_PERM_PR, HK_FMT_STRING, 0, 0, "PHICOMM", 0, 0, 0, 0},
	{HK_IID_MODEL, HK_CH_MODEL, HK_PERM_PR, HK_FMT_STRING, 0, 0, "TC1 A2", 0, 0, 0, 0},
	{HK_IID_ACC_NAME, HK_CH_NAME, HK_PERM_PR, HK_FMT_STRING, 0, 0, "TC1 智能排插", 0, 0, 0, 0},
	{HK_IID_SERIAL, HK_CH_SERIAL, HK_PERM_PR, HK_FMT_STRING, 0, 0, "TC1_9140", 0, 0, 0, 0},
	{HK_IID_FIRMWARE, HK_CH_FIRMWARE, HK_PERM_PR, HK_FMT_STRING, 0, 0, "1.0.0", 0, 0, 0, 0},
};

#define OUTLET_CHARS(n, nm)                                                                       \
	{                                                                                             \
		{HK_IID_OUTLET_NAME(n), HK_CH_NAME, HK_PERM_PR, HK_FMT_STRING, 0, 0, nm, 0, 0, 0, 0},    \
		{HK_IID_OUTLET_ON(n), HK_CH_ON, HK_PERM_PR | HK_PERM_PW | HK_PERM_EV, HK_FMT_BOOL,       \
		 1, 0, NULL, 0, 0, 1, 1},                                                                 \
	}

static const HK_CHAR g_outlet_chars[HK_NR_OUTLET][2] = {
	OUTLET_CHARS(0, "插口1"),
	OUTLET_CHARS(1, "插口2"),
	OUTLET_CHARS(2, "插口3"),
	OUTLET_CHARS(3, "插口4"),
	OUTLET_CHARS(4, "插口5"),
	OUTLET_CHARS(5, "插口6"),
};

static const HK_SERVICE g_services[] = {
	{HK_IID_INFO_SVC, HK_SVC_ACCESSORY_INFO, 0, 6, g_info_chars},
	{HK_IID_OUTLET_SVC(0), HK_SVC_OUTLET, 1, 2, g_outlet_chars[0]},
	{HK_IID_OUTLET_SVC(1), HK_SVC_OUTLET, 0, 2, g_outlet_chars[1]},
	{HK_IID_OUTLET_SVC(2), HK_SVC_OUTLET, 0, 2, g_outlet_chars[2]},
	{HK_IID_OUTLET_SVC(3), HK_SVC_OUTLET, 0, 2, g_outlet_chars[3]},
	{HK_IID_OUTLET_SVC(4), HK_SVC_OUTLET, 0, 2, g_outlet_chars[4]},
	{HK_IID_OUTLET_SVC(5), HK_SVC_OUTLET, 0, 2, g_outlet_chars[5]},
};

const HK_ACCESSORY_DB hk_acc_db = {1, 7, g_services};

int hk_iid_to_outlet(uint16_t iid)
{
	int i;
	for (i = 0; i < HK_NR_OUTLET; i++)
	{
		if (iid == HK_IID_OUTLET_ON(i))
			return i;
	}
	return -1;
}

const HK_CHAR *hk_acc_find(const HK_ACCESSORY_DB *db, uint16_t iid)
{
	uint8_t s;
	if (db == NULL)
		return NULL;
	for (s = 0; s < db->nsvc; s++)
	{
		const HK_SERVICE *svc = &db->svcs[s];
		uint8_t c;
		for (c = 0; c < svc->nchar; c++)
		{
			if (svc->chars[c].iid == iid)
				return &svc->chars[c];
		}
	}
	return NULL;
}

/*
 * 写一个 characteristic 的 "value": ... 字段（连键一起写）。
 * 返回 0 = 已写出；-1 = 取不到值（此时**一个字节都不写**，不会留下
 * 半个 "value": 变成非法 JSON）。
 */
static int put_char_value(HK_JSON_OUT *j, const HK_CHAR *ch, const HK_ACC_IO *io)
{
	int v = 0;

	if (ch->dyn)
	{
		int idx = hk_iid_to_outlet(ch->iid);
		if (idx < 0)
			return -1;
		if (io == NULL || io->get_on == NULL || io->get_on((uint8_t)idx, &v, io->ctx) != 0)
			return -1;
	}
	else
		v = ch->ival;

	hk_json_key(j, "value");
	switch (ch->format)
	{
	case HK_FMT_BOOL:
		hk_json_bool(j, v ? 1 : 0);
		break;
	case HK_FMT_STRING:
		hk_json_str(j, ch->sval ? ch->sval : "");
		break;
	default:
		hk_json_int(j, v);
		break;
	}
	return 0;
}

static void put_perms(HK_JSON_OUT *j, uint8_t perms)
{
	hk_json_arr_begin(j);
	if (perms & HK_PERM_PR)
		hk_json_str(j, "pr");
	if (perms & HK_PERM_PW)
		hk_json_str(j, "pw");
	if (perms & HK_PERM_EV)
		hk_json_str(j, "ev");
	hk_json_arr_end(j);
}

size_t hk_acc_render_accessories(const HK_ACCESSORY_DB *db, const HK_ACC_IO *io,
								 uint8_t *out, size_t cap)
{
	HK_JSON_OUT j;
	uint8_t s;
	char tbuf[8];
	size_t n;

	if (db == NULL || out == NULL || cap == 0)
		return 0;

	hk_json_init(&j, out, cap);
	hk_json_obj_begin(&j);
	hk_json_key(&j, "accessories");
	hk_json_arr_begin(&j);

	hk_json_obj_begin(&j);
	hk_json_key(&j, "aid");
	hk_json_int(&j, (int32_t)db->aid);
	hk_json_key(&j, "services");
	hk_json_arr_begin(&j);

	for (s = 0; s < db->nsvc; s++)
	{
		const HK_SERVICE *svc = &db->svcs[s];
		uint8_t c;

		hk_json_obj_begin(&j);
		hk_json_key(&j, "iid");
		hk_json_int(&j, (int32_t)svc->iid);
		hk_json_key(&j, "type");
		n = type_str(svc->type, tbuf);
		tbuf[n] = '\0';
		hk_json_str(&j, tbuf);
		if (svc->primary)
		{
			hk_json_key(&j, "primary");
			hk_json_bool(&j, 1);
		}
		hk_json_key(&j, "characteristics");
		hk_json_arr_begin(&j);

		for (c = 0; c < svc->nchar; c++)
		{
			const HK_CHAR *ch = &svc->chars[c];

			hk_json_obj_begin(&j);
			hk_json_key(&j, "iid");
			hk_json_int(&j, (int32_t)ch->iid);
			hk_json_key(&j, "type");
			n = type_str(ch->type, tbuf);
			tbuf[n] = '\0';
			hk_json_str(&j, tbuf);
			hk_json_key(&j, "perms");
			put_perms(&j, ch->perms);
			hk_json_key(&j, "format");
			hk_json_str(&j, fmt_name(ch->format));

			/* 只写特性（Identify）不能带 value */
			if (ch->perms & HK_PERM_PR)
				put_char_value(&j, ch, io);
			hk_json_obj_end(&j);
		}
		hk_json_arr_end(&j);
		hk_json_obj_end(&j);
	}

	hk_json_arr_end(&j); /* services */
	hk_json_obj_end(&j); /* accessory */
	hk_json_arr_end(&j); /* accessories */
	hk_json_obj_end(&j); /* root */

	if (!hk_json_ok(&j))
		return 0;
	return hk_json_len(&j);
}

/*
 * 处理 /characteristics 的请求条目。
 * 每条结果的形状：
 *   成功：{"aid":1,"iid":12,"value":true}
 *   失败：{"aid":1,"iid":99,"status":-70407}
 * 事件订阅请求（带 ev）回 {"aid":..,"iid":..,"ev":true}。
 */
size_t hk_acc_render_characteristics(const HK_ACCESSORY_DB *db, const HK_ACC_IO *io,
									 const HK_JITEM *items, int n,
									 uint8_t *out, size_t cap)
{
	HK_JSON_OUT j;
	int i;

	if (db == NULL || out == NULL || cap == 0)
		return 0;

	hk_json_init(&j, out, cap);
	hk_json_obj_begin(&j);
	hk_json_key(&j, "characteristics");
	hk_json_arr_begin(&j);

	for (i = 0; i < n; i++)
	{
		const HK_JITEM *it = &items[i];
		const HK_CHAR *ch;
		int status = HK_STATUS_OK;

		hk_json_obj_begin(&j);
		hk_json_key(&j, "aid");
		hk_json_int(&j, it->aid);
		hk_json_key(&j, "iid");
		hk_json_int(&j, it->iid);

		if ((uint32_t)it->aid != db->aid)
			status = HK_STATUS_NO_CHAR;

		ch = (status == HK_STATUS_OK) ? hk_acc_find(db, (uint16_t)it->iid) : NULL;
		if (status == HK_STATUS_OK && ch == NULL)
			status = HK_STATUS_NO_CHAR;

		if (status == HK_STATUS_OK && it->has_ev)
		{
			/* 事件订阅：先接受下来（真正的推送在第四阶段做） */
			if (!(ch->perms & HK_PERM_EV))
				status = HK_STATUS_NO_NOTIFY;
			else
			{
				hk_json_key(&j, "ev");
				hk_json_bool(&j, it->ev ? 1 : 0);
			}
		}
		else if (status == HK_STATUS_OK && it->vtype != HK_JVAL_NONE)
		{
			int idx;

			if (!(ch->perms & HK_PERM_PW))
			{
				status = HK_STATUS_READ_ONLY;
			}
			else if (ch->format == HK_FMT_BOOL &&
					 it->vtype != HK_JVAL_BOOL && it->vtype != HK_JVAL_INT)
			{
				status = HK_STATUS_INVALID_TYPE;
			}
			else if (ch->format != HK_FMT_BOOL && it->vtype == HK_JVAL_BOOL)
			{
				status = HK_STATUS_INVALID_TYPE;
			}
			else if ((idx = hk_iid_to_outlet(ch->iid)) < 0)
			{
				status = HK_STATUS_NO_CHAR;
			}
			else if (io == NULL || io->set_on == NULL ||
					 io->set_on((uint8_t)idx, it->vb ? 1 : 0, io->ctx) != 0)
			{
				status = HK_STATUS_SVC_COMM;
			}
			else
			{
				int v = 0;
				if (io->get_on != NULL && io->get_on((uint8_t)idx, &v, io->ctx) == 0)
				{
					hk_json_key(&j, "value");
					hk_json_bool(&j, v ? 1 : 0);
				}
			}
		}
		else if (status == HK_STATUS_OK)
		{
			/* 纯读 */
			if (!(ch->perms & HK_PERM_PR))
				status = HK_STATUS_WRITE_ONLY;
			else if (put_char_value(&j, ch, io) != 0)
			{
				status = HK_STATUS_SVC_COMM;
			}
		}

		if (status != HK_STATUS_OK)
		{
			hk_json_key(&j, "status");
			hk_json_int(&j, status);
		}
		hk_json_obj_end(&j);
	}

	hk_json_arr_end(&j);
	hk_json_obj_end(&j);

	if (!hk_json_ok(&j))
		return 0;
	return hk_json_len(&j);
}

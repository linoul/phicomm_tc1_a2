/*
 * hk_acc.h —— HomeKit 配件模型（6 路排插）
 *
 * 这一层把"6 个继电器"翻译成 HAP 能懂的形状：
 *   aid=1 一个配件，里面 1 个 AccessoryInformation 服务 + 6 个 Outlet 服务，
 *   每个 Outlet 有 Name 和 On 两个 characteristic。
 *
 * 两个容易搞错、错了 iOS 会直接不认的地方：
 *
 *   1) HAP 的 type 字段是**短 UUID**（"3E"、"47"、"25"），不是完整的
 *      "0000003E-0000-1000-8000-0026BB765291"。短形式是官方允许的，
 *      HAP-NodeJS 输出的也是短形式。
 *   2) iid 在同一个 aid 内必须唯一且**稳定持久化**（不能每次开机变），
 *      否则 iOS 那边已经缓存的实体会对不上。本工程用固定规律生成
 *      （见下面 HK_IID_* 宏），所以天然稳定。
 *
 * 值怎么来：本层不直接操作 GPIO。静态值（Name/型号）编译期写死在 const 里，
 * 动态值（On）走 HK_ACC_IO 回调 —— 这样第三阶段接 user_relay 时不用改这里。
 */
#ifndef HK_ACC_H
#define HK_ACC_H

#include <stdint.h>
#include <stddef.h>

#include "hk_json.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- HAP 服务类型 ---- */
#define HK_SVC_ACCESSORY_INFO 0x3E
#define HK_SVC_OUTLET 0x47
#define HK_SVC_SWITCH 0x49

/* ---- HAP characteristic 类型 ---- */
#define HK_CH_IDENTIFY 0x14
#define HK_CH_MANUFACTURER 0x20
#define HK_CH_MODEL 0x21
#define HK_CH_NAME 0x23
#define HK_CH_ON 0x25
#define HK_CH_OUTLET_IN_USE 0x26
#define HK_CH_SERIAL 0x30
#define HK_CH_FIRMWARE 0x52

/* ---- 权限位（对应 JSON 里的 perms 数组） ---- */
#define HK_PERM_PR 0x01 /* paired read */
#define HK_PERM_PW 0x02 /* paired write */
#define HK_PERM_EV 0x04 /* events */

/* ---- 值类型（对应 JSON 里的 format） ---- */
#define HK_FMT_BOOL 0
#define HK_FMT_UINT8 1
#define HK_FMT_UINT16 2
#define HK_FMT_UINT32 3
#define HK_FMT_INT32 4
#define HK_FMT_FLOAT 5
#define HK_FMT_STRING 6

/* ---- HAP 状态码（/characteristics 响应里的 "status"） ---- */
#define HK_STATUS_OK 0
#define HK_STATUS_SVC_COMM -70401
#define HK_STATUS_READ_ONLY -70402
#define HK_STATUS_WRITE_ONLY -70403
#define HK_STATUS_NO_NOTIFY -70404
#define HK_STATUS_OUT_OF_RANGE -70405
#define HK_STATUS_INVALID_TYPE -70406
#define HK_STATUS_NO_CHAR -70407

/* ---- iid 分配（固定规律，保证跨重启稳定） ---- */
#define HK_NR_OUTLET 6
#define HK_IID_INFO_SVC 1
#define HK_IID_IDENTIFY 2
#define HK_IID_MANUFACTURER 3
#define HK_IID_MODEL 4
#define HK_IID_ACC_NAME 5
#define HK_IID_SERIAL 6
#define HK_IID_FIRMWARE 7

/* 第 n 路（0 基）的 Outlet：服务 = 10+10n，Name = 11+10n，On = 12+10n */
#define HK_IID_OUTLET_SVC(n) (uint16_t)(10 + 10 * (n))
#define HK_IID_OUTLET_NAME(n) (uint16_t)(11 + 10 * (n))
#define HK_IID_OUTLET_ON(n) (uint16_t)(12 + 10 * (n))

/* 由 On 的 iid 反推路号；不是 On 返回 -1 */
int hk_iid_to_outlet(uint16_t iid);

typedef struct _HK_CHAR
{
	uint16_t iid;
	uint16_t type;
	uint8_t perms;
	uint8_t format;
	uint8_t dyn;		 /* 1 = 运行时值，生成 JSON 时要回调取 */
	uint8_t rsvd;
	const char *sval;	 /* format=string 且非动态时的静态值 */
	int32_t ival;		 /* 静态数值 / 动态时的初值 */
	int32_t min, max, step; /* 数值范围（HAP 要求有范围的必须给） */
} HK_CHAR;

typedef struct _HK_SERVICE
{
	uint16_t iid;
	uint16_t type;
	uint8_t primary; /* 1 = 主服务（一个配件只能有一个） */
	uint8_t nchar;
	const HK_CHAR *chars;
} HK_SERVICE;

typedef struct _HK_ACCESSORY_DB
{
	uint32_t aid;
	uint8_t nsvc;
	const HK_SERVICE *svcs;
} HK_ACCESSORY_DB;

/*
 * 运行时 I/O：本层只认"第几路"，具体是继电器还是别的由第三阶段填。
 * 返回 0 表示成功（get 的值写进 *v）。
 */
typedef struct _HK_ACC_IO
{
	int (*get_on)(uint8_t idx, int *v, void *ctx);
	int (*set_on)(uint8_t idx, int v, void *ctx);
	void (*identify)(void *ctx);
	void *ctx;
} HK_ACC_IO;

/* 6 路排插的静态数据库（const，放在 flash） */
extern const HK_ACCESSORY_DB hk_acc_db;

/*
 * 生成 /accessories 的响应体。
 * 返回写出的字节数；0 表示缓冲不够（此时 out 内容无意义）。
 */
size_t hk_acc_render_accessories(const HK_ACCESSORY_DB *db, const HK_ACC_IO *io,
								 uint8_t *out, size_t cap);

/*
 * 生成 /characteristics 的响应体（读或写的结果）。
 * items 是请求里的条目；io 用来真正读写。
 * 返回写出的字节数；0 表示缓冲不够。
 */
size_t hk_acc_render_characteristics(const HK_ACCESSORY_DB *db, const HK_ACC_IO *io,
									 const HK_JITEM *items, int n,
									 uint8_t *out, size_t cap);

/* 按 iid 找 characteristic；找不到返回 NULL */
const HK_CHAR *hk_acc_find(const HK_ACCESSORY_DB *db, uint16_t iid);

#ifdef __cplusplus
}
#endif

#endif /* HK_ACC_H */

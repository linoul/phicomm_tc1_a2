
#include <hsf.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "app_main.h"
#include "user_bl0937.h"
#include "user_config.h"
#include "user_gpio.h"
#include "user_function.h"
#include "user_rtc.h"

#define BL0937_DEBUG_LEVEL 4

/*
 * 把"总开关"和"单线开关"合成三个内部宏。
 * BL0937_ENABLE = 0 时三条线全部视为未接线，引脚/中断/定时器一概不碰，
 * 同时不会留下"定义了却没用到"的函数而报 unused-function 警告。
 */
#if BL0937_ENABLE
#define BL0937_CF_ON  BL0937_USE_CF
#define BL0937_CF1_ON BL0937_USE_CF1
#define BL0937_SEL_ON BL0937_USE_SEL
#else
#define BL0937_CF_ON  0
#define BL0937_CF1_ON 0
#define BL0937_SEL_ON 0
#endif

/* ---- 中断通道数编译期兜底 ----
 *
 * HF-LPX30 的 GPIO 中断一共只有 2 个通道（依据见 user_gpio.h 的说明，
 * 从 libLPBX3Kernel.a 的 gpio_irq_api.o 反汇编得到）。
 * 第 3 个中断引脚会撞 MBED_ASSERT，静默停机 + 看门狗复位循环 ——
 * 现场只能看到"设备起不来"，非常难查。所以这里把它变成编译错误。
 *
 * 用中断的引脚：按键(KEY_USE_IRQ) + CF(BL0937_USE_CF) + CF1(BL0937_USE_CF1)。
 */
#if (BL0937_CF_ON + BL0937_CF1_ON + KEY_USE_IRQ) > 2
#error "GPIO 中断通道超限（上限 2 个）: KEY_USE_IRQ + BL0937_USE_CF + BL0937_USE_CF1 <= 2。想要 CF1 就把 user_gpio.h 的 KEY_USE_IRQ 改成 0（按键改走轮询，功能不变）。"
#endif

/* ---- 计量参数与电量数据在 userbin 中的存放位置 ----
 *
 * userbin 共 4KB（hfflashlayout.h 的 USER_BIN_FILE_SIZE），实测占用：
 *	   0     ~ 1300 : MQTT_CONFIG   本应用（MQTT_CONFIG_USERBIN_ADDR）
 *	2048     ~ 2236 : PLUG_CONFIG   本应用（PLUG_CONFIG_USERBIN_ADDR）
 *	2864     ~ 3964 : SDK 自用      USER_BIN_FILE_MQTT_OFFSET，注释 size(1100)
 *
 * 教训：**绝对不要写到 2864 以后**。那是 SDK 自己的 socka/MQTT 配置区，
 * 写进去会让 SDK 的配置校验失败，表现为"开机跑到计量初始化就重启、循环不休"。
 * 本文件早期版本把校准放 3072、电量放 3200，正是踩了这个坑。
 *
 * 现在用 MQTT_CONFIG 之后、PLUG_CONFIG 之前的空闲段（1300~2048）：
 * 夹在本应用两个结构之间，既不会被本应用覆盖，也离 SDK 的区足够远。
 * 地址由 sizeof 推导 —— 结构体改了也不会错位，末尾还有编译期断言兜底。
 */
#define BL0937_CAL_ADDR (MQTT_CONFIG_USERBIN_ADDR + sizeof(MQTT_CONFIG) + 16)

#define BL0937_ENERGY_MAGIC 0xE937

/* 过载保护：连续 N 秒超阈值才动作，避免启动浪涌误跳 */
#define BL0937_OVERLOAD_TRIP_SEC 5

/* 电量周期落盘间隔（毫秒）= 1 小时。
 * ★ 两个条件同时满足才写 flash：① 距上次落盘已过 1 小时；② 电量真的变了。
 * 插口全关/待机不动时 energy_mwh 不变 → 一次也不写（用户明确要求）。
 * 1 小时 = 每天最多 24 次写，NOR flash 10 万次擦写寿命 > 10 年。
 * 代价：设备在两次落盘之间断电，最多丢 1 小时电量；跨天滚动
 * （bl0937_roll_day）和手动清零仍会立即落盘，这两处不能省。 */
#define BL0937_SAVE_INTERVAL_MS 3600000

BL0937_CAL bl0937_cal;
BL0937_DATA bl0937;

extern time_t utc8ts; /* 定义在 user_rtc.c，已经加过 UTC+8 偏移 */

static float energy_acc = 0.0f; /* 未满 1 毫瓦时的累积器，避免小功率被整数截断吃掉 */
static uint8_t overload_cnt = 0;
static uint32_t energy_last_save = 0;
static uint64_t energy_saved_mwh = 0; /* 上次落盘的 energy_mwh，周期保存只在有变化时写 flash */

/* ★ 电量"待落盘"标志。
 * bl0937_save_energy() 的调用点全在 bl0937_loop() 里，而 bl0937_loop()
 * 跑在 hftimer 回调上下文；hffile_userbin_write() 在这种上下文里会复位
 * 设备（10/10 实测：act=kt 保存任务必重启）。所以这里只置标志，
 * 真正的写 flash 交给 user_rtc.c 的 userbin 保存线程。 */
bool bl0937_energy_save_flag = false;

/* ★ 校准系数的"待落盘"标志，理由同上：bl0937_cal_save() 也会从 web 回调
 * （设置页 / 校准页 / 一键标定）调用，而 userbin 文件层必须有唯一写者。 */
bool bl0937_cal_save_flag = false;

/* 电量持久化自证计数，说明见 user_bl0937.h 里的声明处 */
uint8_t bl0937_energy_load_ok = 0;
uint32_t bl0937_energy_save_ok = 0;
uint32_t bl0937_energy_save_err = 0;

/*
 * 中断风暴保护（两级）。
 *
 * 引脚悬空或受强干扰时，CF/CF1 可能产生几十万次/秒的中断。ISR 会占满 CPU，
 * 整机表现为"灯亮、按键没反应、不出热点"。
 *
 * 第一级【必须在 ISR 里】：单窗口计数一超上限就"关闸"，之后 ISR 立即变成
 *   几个指令的空操作。只在 1 秒任务里检测是没用的 —— 系统根本活不到那一刻。
 * 第二级【在 1 秒任务里】：连续 N 个窗口都爆表，就用 SDK 接口彻底关掉该路中断。
 */
#if BL0937_CF_ON
static uint8_t cf_bad_windows = 0;
static uint8_t cf_irq_on = BL0937_CF_ON;
static volatile uint8_t cf_gate = 0; /* 1 = 允许 ISR 计数 */
static volatile uint8_t cf_over = 0; /* 本窗口 ISR 触发了超限关闸 */
/* 最近一次 CF 脉冲的毫秒时间戳（与 bl0937.cf_period_ms 配套，算脉冲间隔）。
 * 无符号减法天然免疫 49.7 天回绕。hfsys_get_time → rt_time_get：单条 ldr
 * 读全局 os_time（已反汇编核实），ISR 里调用无锁无 RTOS 依赖，安全。 */
static volatile uint32_t cf_last_ms = 0;
#endif

#if BL0937_CF1_ON
static uint8_t cf1_bad_windows = 0;
static uint8_t cf1_irq_on = BL0937_CF1_ON;
static volatile uint8_t cf1_gate = 0;
static volatile uint8_t cf1_over = 0;
#endif

/* 前几个窗口把原始脉冲数打出来，方便判断引脚到底有没有信号 */
static uint8_t dbg_windows = 0;
#define BL0937_DBG_WINDOWS 5

typedef struct _BL0937_ENERGY_STORE
{
	uint16_t magic;
	uint64_t energy_mwh;
	uint32_t energy_start;
	uint32_t today_mwh;
	uint32_t yesterday_mwh;
	int16_t today_dayno;
	uint8_t today_valid;
	uint8_t reserved;
	uint16_t crc;
} BL0937_ENERGY_STORE;

/* 电量数据紧跟在校准参数之后（见文件上方 userbin 布局说明） */
#define BL0937_ENERGY_ADDR (BL0937_CAL_ADDR + sizeof(BL0937_CAL) + 16)

/*
 * 编译期兜底：两块数据一旦大到越过 PLUG_CONFIG 的起点，这里直接编译失败。
 * 宁可在编译时就报错，也不要运行期悄悄写坏别的配置。
 */
typedef char bl0937_userbin_layout_ok
	[(BL0937_ENERGY_ADDR + sizeof(BL0937_ENERGY_STORE) <= PLUG_CONFIG_USERBIN_ADDR) ? 1 : -1];

/*------------------------------------------------------------- 工具函数 */

void USER_FUNC bl0937_fmt_float(char *out, float v, uint8_t dec)
{
	int32_t scale = 1;
	uint8_t i;
	int32_t ip, fp;
	uint8_t neg = 0;
	float av = v;

	for (i = 0; i < dec; i++)
		scale *= 10;

	if (av < 0.0f)
	{
		neg = 1;
		av = -av;
	}

	ip = (int32_t)av;
	fp = (int32_t)((av - (float)ip) * (float)scale + 0.5f);
	if (fp >= scale)
	{
		ip++;
		fp = 0;
	}

	if (dec == 0)
		sprintf(out, neg ? "-%d" : "%d", (int)ip);
	else
		sprintf(out, neg ? "-%d.%0*d" : "%d.%0*d", (int)ip, (int)dec, (int)fp);
}

/* 与工程里 crc_calc 一致的累加和校验 */
static uint16_t USER_FUNC bl0937_sum16(const void *p, int len)
{
	const uint8_t *d = (const uint8_t *)p;
	uint16_t s = 0;
	int i;
	for (i = 0; i < len; i++)
		s = (uint16_t)(s + d[i]);
	return s;
}

/*------------------------------------------------------------- 中断计数 */

/* 中断里只做加法和比较。任何耗时操作（打日志、算浮点）都会丢脉冲，
 * 而悬空引脚又会让中断爆量 —— 所以超限后立刻关闸自保。 */
#if BL0937_CF_ON
static void USER_FUNC bl0937_cf_irq(uint32_t arg1, uint32_t arg2)
{
	uint32_t now;

	(void)arg1;
	(void)arg2;

	if (!cf_gate)
		return;

	if (bl0937.cf_cnt >= BL0937_MAX_PULSE_PER_SEC)
	{
		cf_gate = 0; /* 第一级保护：本窗口不再计数 */
		cf_over = 1;
		return;
	}

	/* 给脉冲打时间戳，算出与上一个脉冲的间隔（低功率周期法用）。
	 * 只有 3 条指令的额外开销，风暴场景下也扛得住。 */
	now = hfsys_get_time();
	bl0937.cf_period_ms = now - cf_last_ms;
	cf_last_ms = now;
	bl0937.cf_pulse_total++;
	bl0937.cf_cnt++;
}
#endif

#if BL0937_CF1_ON
static void USER_FUNC bl0937_cf1_irq(uint32_t arg1, uint32_t arg2)
{
	(void)arg1;
	(void)arg2;

	if (!cf1_gate)
		return;

	if (bl0937.cf1_cnt >= BL0937_MAX_PULSE_PER_SEC)
	{
		cf1_gate = 0;
		cf1_over = 1;
		return;
	}
	bl0937.cf1_cnt++;
}
#endif

/*------------------------------------------------------------- SEL 控制 */

#if BL0937_SEL_ON
static void USER_FUNC bl0937_sel_apply(void)
{
#if BL0937_SEL_CURRENT_HIGH
	if (bl0937.sel_state)
		hfgpio_fset_out_high(BL0937_SEL_FID);
	else
		hfgpio_fset_out_low(BL0937_SEL_FID);
#else
	if (bl0937.sel_state)
		hfgpio_fset_out_low(BL0937_SEL_FID);
	else
		hfgpio_fset_out_high(BL0937_SEL_FID);
#endif
}
#endif

/*
 * CF1 档位锁定。标定时必须把 CF1 固定在某一档，否则一秒测电压、一秒测电流，
 * 读到的频率交替变化，没法用来反算系数。
 * 这里立刻刷新 SEL 电平，不用等下一个采样周期。
 */
void USER_FUNC bl0937_sel_lock(uint8_t mode)
{
	if (mode > 2)
		mode = 0;

	bl0937.sel_lock = mode;

#if BL0937_SEL_ON
	if (mode == 1)
		bl0937.sel_state = 0; /* 电压档 */
	else if (mode == 2)
		bl0937.sel_state = 1; /* 电流档 */

	bl0937_sel_apply();
#endif
}

/*------------------------------------------------------------- 参数存取 */

void USER_FUNC bl0937_cal_load(void)
{
	memset((char *)&bl0937_cal, 0, sizeof(BL0937_CAL));
	hffile_userbin_read(BL0937_CAL_ADDR, (char *)&bl0937_cal, sizeof(BL0937_CAL));

	if (bl0937_cal.magic != BL0937_CAL_MAGIC ||
		bl0937_cal.crc != bl0937_sum16(&bl0937_cal, sizeof(BL0937_CAL) - 2))
	{
		memset((char *)&bl0937_cal, 0, sizeof(BL0937_CAL));
		bl0937_cal.magic = BL0937_CAL_MAGIC;
		bl0937_cal.power_pulses_per_kw = BL0937_DEF_PULSES_PER_KW;
		bl0937_cal.voltage_pulses_per_v = BL0937_DEF_PULSES_PER_V;
		bl0937_cal.current_pulses_per_a = BL0937_DEF_PULSES_PER_A;
		bl0937_cal.nominal_v = BL0937_DEF_NOMINAL_V;
		bl0937_cal.max_power = 0;
		bl0937_cal_save();
		HF_Debug(BL0937_DEBUG_LEVEL, "bl0937: load default calibration\n");
	}
}

void USER_FUNC bl0937_cal_save(void)
{
	/* 系数合理区间（对应 0.01~100 W/脉冲）。越界多半是手滑或旧版
	 * 反算 bug 的产物，兜底回 A1 实测默认值，同时避免除零 */
	/* 系数合理区间 [100, 100000] 脉冲/kW（即 0.01~10 W/Hz，对齐 A1 的
	 * 合法区间 X100=100~10000）。旧错误系数 58.48 落在区间外，直接回默认 */
	if (bl0937_cal.power_pulses_per_kw < 100.0f ||
		bl0937_cal.power_pulses_per_kw > 100000.0f)
		bl0937_cal.power_pulses_per_kw = BL0937_DEF_PULSES_PER_KW;
	if (bl0937_cal.voltage_pulses_per_v < 0.01f)
		bl0937_cal.voltage_pulses_per_v = BL0937_DEF_PULSES_PER_V;
	if (bl0937_cal.current_pulses_per_a < 0.01f)
		bl0937_cal.current_pulses_per_a = BL0937_DEF_PULSES_PER_A;
	/* 电网电压只接受合理范围，防止手滑填 0 导致电流算出无穷大 */
	if (bl0937_cal.nominal_v < 50.0f || bl0937_cal.nominal_v > 300.0f)
		bl0937_cal.nominal_v = BL0937_DEF_NOMINAL_V;

	bl0937_cal.magic = BL0937_CAL_MAGIC;
	bl0937_cal.crc = bl0937_sum16(&bl0937_cal, sizeof(BL0937_CAL) - 2);
	bl0937_cal_save_flag = true; /* 只登记，由 userbin 保存线程写 flash */
}

/* 真正写 flash。★★ 只允许在 user_rtc.c 的 userbin 保存线程里调用。 */
void USER_FUNC bl0937_cal_save_now(void)
{
	bl0937_cal_save_flag = false;
	hffile_userbin_write(BL0937_CAL_ADDR, (char *)&bl0937_cal, sizeof(BL0937_CAL));
}

/* 登记"待落盘"。可以安全地在 hftimer 回调里调用（见 bl0937_energy_save_flag 的说明）。 */
void USER_FUNC bl0937_save_energy(void)
{
	/* ★ 这里【不能】更新 energy_saved_mwh。
	 *
	 * energy_saved_mwh 的语义是"flash 里已经存了这个数"，它只能由
	 * bl0937_save_energy_now() 在【写成功之后】更新。
	 *
	 * 旧写法在这里就把它对齐成当前值，等于"还没写就宣称已经写好了"：
	 * 一旦 hffile_userbin_write 失败（返回值没人看），
	 *   - energy_saved_mwh 已是当前值 → bl0937_loop 里的
	 *     "energy_mwh != energy_saved_mwh" 永远为假 → 整整一小时不再重试；
	 *   - 更要命的是它让"没写成功"这件事在代码层面彻底隐形，
	 *     现场表现就是"电量每次重启都从 0 爬"，而代码看上去全对。
	 */
	energy_last_save = hfsys_get_time();
	bl0937_energy_save_flag = true;
}

/* 真正写 flash。
 * ★★ 只允许在 user_rtc.c 的 userbin 保存线程里调用 ——
 *    在 hftimer 回调上下文里调 hffile_userbin_write() 会复位设备。 */
void USER_FUNC bl0937_save_energy_now(void)
{
	BL0937_ENERGY_STORE st;
	int wr;

	bl0937_energy_save_flag = false; /* 先清后写：写期间的新请求留到下一拍 */
	memset((char *)&st, 0, sizeof(st));
	st.magic = BL0937_ENERGY_MAGIC;
	st.energy_mwh = bl0937.energy_mwh;
	st.energy_start = bl0937.energy_start;
	st.today_mwh = bl0937.today_mwh;
	st.yesterday_mwh = bl0937.yesterday_mwh;
	st.today_dayno = bl0937.today_dayno;
	st.today_valid = bl0937.today_valid;
	st.crc = bl0937_sum16(&st, sizeof(st) - 2);

	/* ★ 必须看返回值：hffile_userbin_write 写失败不会报错，只会返回短长度
	 *   （或者 0）。以前这里直接丢弃返回值，于是"写失败"和"写成功"在系统里
	 *   毫无区别 —— 这正是电量持久化查不动的根本原因之一。 */
	wr = hffile_userbin_write(BL0937_ENERGY_ADDR, (char *)&st, sizeof(st));
	if (wr == (int)sizeof(st))
	{
		bl0937_energy_save_ok++;
		energy_saved_mwh = bl0937.energy_mwh; /* 写成功才更新"flash 里已有"的基准 */
	}
	else
	{
		bl0937_energy_save_err++;
		/* 失败就【不动】energy_saved_mwh：下一小时到点会自动重试 */
	}
	energy_last_save = hfsys_get_time();
}

void USER_FUNC bl0937_load_energy(void)
{
	BL0937_ENERGY_STORE st;

	memset((char *)&st, 0, sizeof(st));
	hffile_userbin_read(BL0937_ENERGY_ADDR, (char *)&st, sizeof(st));

	if (st.magic == BL0937_ENERGY_MAGIC &&
		st.crc == bl0937_sum16(&st, sizeof(st) - 2))
	{
		bl0937_energy_load_ok = 1; /* /status 的 en_load：本次开机的总计是从 flash 续上的 */
		bl0937.energy_mwh = st.energy_mwh;
		bl0937.energy_start = st.energy_start;
		bl0937.today_mwh = st.today_mwh;
		bl0937.yesterday_mwh = st.yesterday_mwh;
		bl0937.today_dayno = st.today_dayno;
		bl0937.today_valid = st.today_valid;
		energy_saved_mwh = st.energy_mwh; /* flash 里已有这个值，周期保存不用重写 */
	}
	else
	{
		bl0937_energy_load_ok = 0; /* flash 里没有有效记录：总计只能从 0 起 */
		bl0937.energy_mwh = 0;
		bl0937.energy_start = 0;
		bl0937.today_mwh = 0;
		bl0937.yesterday_mwh = 0;
		bl0937.today_dayno = -1;
		bl0937.today_valid = 0;
		energy_saved_mwh = 0; /* flash 里没有有效数据，对基准值也归零，
		                       * 免得"无变化不落盘"的判断用了个陈旧基准 */
	}
}

/*------------------------------------------------------------- 初始化 */

/* 1 秒采样定时器。TIMER1 被按键占用、TIMER2 被 RTC 占用，这里用 3 */
#define BL0937_TIMER_ID 3

static void USER_FUNC bl0937_timer_func(hftimer_handle_t htimer)
{
	(void)htimer;
	bl0937_loop();
}

void USER_FUNC bl0937_init(void)
{
#if !BL0937_ENABLE
	/* 总开关关闭：不碰硬件、不建定时器（二分定位用） */
	u_printf("[BL] disabled (BL0937_ENABLE=0)\n");
	return;
#else
	/*
	 * 每一步打一行标记。排插没有屏幕，串口是唯一观测手段：万一某一步把
	 * 系统搞挂，串口上最后出现的那个编号就是出错位置，不用靠猜。
	 * 这些标记不会影响计量（只在开机时打一次）。
	 *
	 * 编号是固定的（关了的功能只是不打那一行，不重新编号，方便对照历史日志）：
	 *   1 enter / 2 cal / 3 energy / 4 time / 5 sel
	 *   6 cf pin / 7 cf irq / 8 cf1 pin / 9 cf1 irq / A timer / B done
	 * 【致命位置看这里】卡在 7 或 9（中断注册）而不是日志一句没打，
	 * 就是 GPIO 中断通道撞了上限，见 user_bl0937.h 顶部的说明。
	 */
	u_printf("[BL] 1 enter\n");

	memset((char *)&bl0937, 0, sizeof(BL0937_DATA));
	bl0937.sel_state = 1;
	bl0937.today_dayno = -1;

	bl0937_cal_load();
	u_printf("[BL] 2 cal loaded\n");

	bl0937_load_energy();
	u_printf("[BL] 3 energy loaded\n");

	if (bl0937.energy_start == 0)
	{
		update_time();
		bl0937.energy_start = (uint32_t)utc8ts;
	}
	u_printf("[BL] 4 time ok\n");

	bl0937.running = 1;
	bl0937.valid = 1;
	energy_last_save = hfsys_get_time();

	/* ★ 开机基线落盘（10/11 加）。
	 *
	 * flash 里没有有效电量记录时，立刻写一份出去 —— 而不是干等 1 小时后的
	 * 周期落盘。两个好处：
	 *   1. 从这一刻起 flash 里就有一份有效记录。此后即便设备在 1 小时内掉电
	 *      或被重启，总计也能从 flash 续上，不会再"从 0 开始爬"。
	 *   2. 它同时是一根探针：烧录后几秒内看 /status 的 en_save 就能判断
	 *      "userbin 落盘线程到底有没有在跑、写 flash 到底成不成功"，
	 *      不必再等 1 小时、也不用靠猜。
	 * 这里只置标志，真正写 flash 由 ubsave 线程完成（见 userbin_save_thread_func）。 */
	if (!bl0937_energy_load_ok)
	{
		u_printf("[BL] no energy record -> request baseline save\n");
		bl0937_save_energy();
	}

#if BL0937_SEL_ON
	u_printf("[BL] 5 sel ret=%d\n", hfgpio_fset_out_low(BL0937_SEL_FID));
	bl0937_sel_apply();
#endif

#if BL0937_CF_ON
	u_printf("[BL] 6 cf pin  ret=%d\n",
			 hfgpio_configure_fpin(BL0937_CF_FID, HFPIO_DEFAULT | HFM_IO_TYPE_INPUT));
	cf_gate = 1;
	u_printf("[BL] 7 cf irq  ret=%d\n",
			 hfgpio_configure_fpin_interrupt(BL0937_CF_FID, HFPIO_IT_RISE_EDGE, bl0937_cf_irq, 1));
#endif

#if BL0937_CF1_ON
	u_printf("[BL] 8 cf1 pin ret=%d\n",
			 hfgpio_configure_fpin(BL0937_CF1_FID, HFPIO_DEFAULT | HFM_IO_TYPE_INPUT));
	cf1_gate = 1;
	u_printf("[BL] 9 cf1 irq ret=%d\n",
			 hfgpio_configure_fpin_interrupt(BL0937_CF1_FID, HFPIO_IT_RISE_EDGE, bl0937_cf1_irq, 1));
#endif

	{
		hftimer_handle_t t = hftimer_create("BL0937", 1000, 1, BL0937_TIMER_ID, bl0937_timer_func, 0);
		if (t != NULL)
			hftimer_start(t);
		u_printf("[BL] A timer %s\n", t ? "ok" : "FAIL");
	}

	u_printf("[BL] B init done (cf=%d cf1=%d sel=%d irq=%d/2)\n",
			 BL0937_CF_FID, BL0937_CF1_FID, BL0937_SEL_FID,
			 (int)(BL0937_CF_ON + BL0937_CF1_ON + KEY_USE_IRQ));
#endif /* BL0937_ENABLE */
}

void USER_FUNC bl0937_energy_clear(void)
{
	bl0937.energy_mwh = 0;
	bl0937.today_mwh = 0;
	bl0937.yesterday_mwh = 0;
	bl0937.today_valid = 0;
	energy_acc = 0.0f;

	update_time();
	bl0937.energy_start = (uint32_t)utc8ts;

	bl0937_save_energy();
}

/*------------------------------------------------------------- 周期任务 */

/* 跨天时把今日电量滚动到昨日 */
static void USER_FUNC bl0937_roll_day(void)
{
	struct tm *t;
	int16_t dayno;

	update_time();
	if (utc8ts < 1600000000)
		return; /* NTP 尚未同步，日期不可信 */

	/* utc8ts 已经加过 8 小时偏移，所以 gmtime 出来的就是北京时间 */
	t = gmtime(&utc8ts);
	if (t == NULL)
		return;

	dayno = (int16_t)t->tm_yday;

	if (!bl0937.today_valid)
	{
		bl0937.today_valid = 1;
		bl0937.today_dayno = dayno;
		bl0937.today_mwh = 0;
		bl0937_save_energy();
		return;
	}

	if (dayno != bl0937.today_dayno)
	{
		bl0937.yesterday_mwh = bl0937.today_mwh;
		bl0937.today_mwh = 0;
		bl0937.today_dayno = dayno;
		bl0937_save_energy();
		HF_Debug(BL0937_DEBUG_LEVEL, "bl0937: day rollover, dayno=%d\n", dayno);
	}
}

/* 过载保护：连续超阈值则断开所有继电器 */
static void USER_FUNC bl0937_overload_check(void)
{
	if (bl0937_cal.max_power == 0)
		return;

	if (bl0937.power > (float)bl0937_cal.max_power)
	{
		if (overload_cnt < 0xFF)
			overload_cnt++;

		if (overload_cnt == BL0937_OVERLOAD_TRIP_SEC)
		{
			HF_Debug(DEBUG_ERROR, "bl0937: overload %dW > %dW, cut off all relays\n",
					 (int)bl0937.power, bl0937_cal.max_power);
			user_relay_set_all(0);
		}
	}
	else
	{
		overload_cnt = 0;
	}
}

void USER_FUNC bl0937_loop(void)
{
	uint32_t cf, cf1;

	if (!bl0937.running)
		return;

	/* 1) 取走中断计数（Cortex-M4 上 32 位对齐读写是原子的） */
	cf = bl0937.cf_cnt;
	bl0937.cf_cnt = 0;
	cf1 = bl0937.cf1_cnt;
	bl0937.cf1_cnt = 0;

#if BL0937_CF_ON
	/* ISR 触发过超限关闸：这一窗口的数据已不可信，按爆表处理 */
	if (cf_over)
	{
		cf_over = 0;
		if (cf < BL0937_MAX_PULSE_PER_SEC)
			cf = BL0937_MAX_PULSE_PER_SEC + 1;
	}
#endif
#if BL0937_CF1_ON
	if (cf1_over)
	{
		cf1_over = 0;
		if (cf1 < BL0937_MAX_PULSE_PER_SEC)
			cf1 = BL0937_MAX_PULSE_PER_SEC + 1;
	}
#endif

	bl0937.cf_hz = cf;
	bl0937.cf1_hz = cf1;

	/* 开机后前几个窗口把原始脉冲数打出来：一眼看出引脚到底有没有信号。
	 * 空载时应当是很小的数（甚至 0），爆表则说明引脚悬空/受干扰。 */
	if (dbg_windows < BL0937_DBG_WINDOWS)
	{
		dbg_windows++;
		u_printf("[BL] win%d cf=%u cf1=%u\n", dbg_windows, (unsigned)cf, (unsigned)cf1);
	}

	/* 2) 有功功率 */
#if BL0937_CF_ON
	if (cf > BL0937_MAX_PULSE_PER_SEC)
	{
		/* 悬空引脚/强干扰的典型表现：脉冲数爆表。判为无效而不是当成大功率 */
		bl0937.valid = 0;
		bl0937.power = 0.0f;

		/* 连续几个窗口都这样，说明这只脚根本不干净，中断很可能正把 CPU
		 * 吃光（表现就是灯亮、按键没反应、连不上网）。关掉中断保住整机，
		 * 计量功能放弃——灯和按键比功率重要。 */
		if (cf_irq_on && ++cf_bad_windows >= BL0937_STORM_LIMIT)
		{
			cf_irq_on = 0;
			bl0937.cf_cnt = 0;
			/* ★ 必须用 fdisable 而不是再调一次 configure_fpin_interrupt。
			 * 后者即使 enable=0 也会走 gpio_irq_init()，而那里一进门就检查
			 * "两个通道是不是都被占了"，占了就 MBED_ASSERT 直接停机
			 * （见 user_bl0937.h 顶部）。按键+CF 恰恰就是"都占了"，
			 * 所以那样写会在第一次遇到脏引脚时把整机搞死。
			 * fdisable 只走 gpio_irq_set(obj,type,0)，不碰通道分配。 */
			hfgpio_fdisable_interrupt(BL0937_CF_FID);
			u_printf("[BL] CF pin noisy -> irq off\n");
		}
	}
	else
	{
		uint32_t now = hfsys_get_time();
		uint32_t since = now - cf_last_ms;

		cf_bad_windows = 0;
		if (cf_irq_on)
			bl0937.valid = 1;

		if (cf >= BL0937_COUNT_MODE_MIN)
		{
			/* 高功率（kp=584.8 时 20 脉冲/秒 ≈ 34W 以上）：窗口计数法。
			 * 量化台阶 1000/kp≈1.7W，在这个功率段相对误差 <5%，
			 * 且完全不受时钟抖动影响 */
			bl0937.power = (float)cf * 1000.0f / bl0937_cal.power_pulses_per_kw;
			bl0937.cf_eff_hz = (float)cf;
		}
		else if (bl0937.cf_pulse_total >= 2 &&
				 bl0937.cf_period_ms >= BL0937_PERIOD_MIN_MS &&
				 since <= bl0937.cf_period_ms * 2 + BL0937_PERIOD_GLUE_MS)
		{
			/* 低功率：脉冲周期法。P = 1000/kp × 1000ms/period_ms。
			 * 持续有效性由"距上一脉冲 ≤ 2×周期+宽限"保证——
			 * 负载消失后最多等一个多周期就归零，不会像 A1 那样
			 * 把最后一次的功率永远定格在那（A1 没有归零逻辑）。 */
			bl0937.power = 1000000.0f / bl0937_cal.power_pulses_per_kw /
						   (float)bl0937.cf_period_ms;
			bl0937.cf_eff_hz = 1000.0f / (float)bl0937.cf_period_ms;
		}
		else
		{
			/* 没有可用的新鲜周期（开机从无脉冲 / 负载已消失） */
			bl0937.power = 0.0f;
			bl0937.cf_eff_hz = 0.0f;
		}

		/* 周期法只在 cf<20 时启用，真功率若高于计数法边界，窗口里
		 * 自然会有 ≥20 个脉冲。周期法算出更高值只能是毛刺/干扰，钳掉 */
		if (cf < BL0937_COUNT_MODE_MIN)
		{
			float count_max = (float)BL0937_COUNT_MODE_MIN * 1000.0f /
							  bl0937_cal.power_pulses_per_kw;
			if (bl0937.power > count_max)
				bl0937.power = count_max;
		}
		if (bl0937.power > 10000.0f)
			bl0937.power = 10000.0f;
	}

	/* 重新开闸，让下一个窗口能正常计数；中断已彻底关掉则保持关闭 */
	cf_gate = cf_irq_on;
#else
	bl0937.power = 0.0f;
#endif

	/* 3) 电压 / 电流 */
#if BL0937_CF1_ON
	/* 有 CF1：每 2 秒在电压/电流档之间切换 SEL，实测脉冲频率 */
	if (cf1 <= BL0937_MAX_PULSE_PER_SEC)
	{
		cf1_bad_windows = 0;

		if (bl0937.sel_state)
		{
			bl0937.current = (float)cf1 / bl0937_cal.current_pulses_per_a;
			if (bl0937.current > 100.0f)
				bl0937.current = 100.0f;
		}
		else
		{
			bl0937.voltage = (float)cf1 / bl0937_cal.voltage_pulses_per_v;
			if (bl0937.voltage > 300.0f)
				bl0937.voltage = 300.0f;
		}
	}
	else if (cf1_irq_on && ++cf1_bad_windows >= BL0937_STORM_LIMIT)
	{
		/* 同 CF：CF1 悬空时也会把中断打满，关掉它（同样只能用 fdisable，
		 * 再调 configure_fpin_interrupt 会撞通道数断言，理由见上面 CF 那段） */
		cf1_irq_on = 0;
		bl0937.cf1_cnt = 0;
		hfgpio_fdisable_interrupt(BL0937_CF1_FID);
		u_printf("[BL] CF1 pin noisy -> irq off\n");
	}

	/* 重新开闸（同 CF） */
	cf1_gate = cf1_irq_on;

#if BL0937_SEL_ON
	/* 标定时把档位锁住，否则 CF1 频率会在电压/电流之间来回跳，读不出稳定值 */
	if (bl0937.sel_lock == 0)
	{
		bl0937.sel_state = bl0937.sel_state ? 0 : 1;
		bl0937_sel_apply();
	}
#endif
#else
	/* 没有 CF1（默认）：电压取标定里的电网电压常数，电流由 P/V 反算。
	 * 精确度足够家用看数 —— 市电电压本来就稳，功率又是由 CF 实测的。 */
	bl0937.voltage = bl0937_cal.nominal_v;
	if (bl0937.voltage > 1.0f && bl0937.power > 0.0f)
	{
		bl0937.current = bl0937.power / bl0937.voltage;
		if (bl0937.current > 100.0f)
			bl0937.current = 100.0f;
	}
	else
	{
		bl0937.current = 0.0f;
	}
#endif

	/* 4) 电量累积：功率(W) × 1 秒 → 毫瓦时 = W × 1000 / 3600 */
	energy_acc += bl0937.power * (1000.0f / 3600.0f);
	if (energy_acc >= 1.0f)
	{
		uint32_t add = (uint32_t)energy_acc;
		bl0937.energy_mwh += add;
		if (bl0937.today_valid)
			bl0937.today_mwh += add;
		energy_acc -= (float)add;
	}

	bl0937_roll_day();
	bl0937_overload_check();

	/* 5) 周期落盘：1 小时一次，且电量没变化就不写 flash。
	 *    · 插口全关 / 待机静止 → energy_mwh 与上次落盘值相等 → 直接跳过；
	 *    · energy_last_save 只在"已登记待写"时才推进（bl0937_save_energy 里），
	 *      所以长时间不变之后一旦开始用电，下一拍立刻补写一次，不用再等 1 小时。
	 *    · 这里只置标志，真正的 hffile_userbin_write 由 userbin 保存线程做。 */
	if (bl0937.energy_mwh != energy_saved_mwh &&
		hfsys_get_time() - energy_last_save >= BL0937_SAVE_INTERVAL_MS)
	{
		bl0937_save_energy();
	}
}

/*------------------------------------------------------------- 对外取值 */

float USER_FUNC bl0937_energy_kwh(void)
{
	return (float)(bl0937.energy_mwh / 1000ULL) / 1000.0f;
}

float USER_FUNC bl0937_today_kwh(void)
{
	return (float)(bl0937.today_mwh / 1000ULL) / 1000.0f;
}

float USER_FUNC bl0937_yesterday_kwh(void)
{
	return (float)(bl0937.yesterday_mwh / 1000ULL) / 1000.0f;
}

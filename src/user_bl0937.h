
#ifndef __USER_BL0937_H_
#define __USER_BL0937_H_

#include <hsf.h>

/*
 * BL0937 单相电能计量芯片驱动
 *
 * ---- 硬件事实（已核实，不要再当成"待确认"） ----
 * TC1 排插原厂主板【自带】BL0937 计量电路，A1/A2 硬件相同（只差主控）。
 * 依据：
 *   - zTC1 项目 README：TC1 分 a1/a2 两版，"a1 a2 两个版本仅主控不同，
 *     除此之外其他无任何区别"，且功能列表里有"app 实时显示功率"；
 *   - 恩山论坛维修帖：TC1 功率不显示 -> 找到 BL0937 -> 更换后功率恢复；
 *   - 社区 ESPHome 改造文称"TC1 的电量统计功能是它的亮点之一"。
 * 所以本驱动是把原厂这颗芯片用起来，而不是外挂改装。
 *
 * ---- 引脚（依据 limbang/TC1A2 的实测管脚表） ----
 *	 BL0937 CF  (有功功率脉冲)	 -> LPx30_GPIO7	插板功能"PW 功率"
 *	 BL0937 CF1 (电压/电流脉冲) -> LPx30_GPIO4	插板功能"电流电压"
 *	 BL0937 SEL (CF1 选择)	 -> LPx30_GPIO12	插板功能"切换电流电压"
 * CF 这一路与上游 user_gpio.h 里那个从未被使用的
 *	 #define POWER (HFGPIO_F_USER_DEFINE+8)
 * 完全对得上（该下标在映射表里正指向 GPIO7），两边相互印证。
 *
 * ---- 已知的硬件短板 ----
 * limbang/TC1A2 提到：电压/电流采样部分原板有缺件，"想要有电压电流显示
 * 的需要把红框缺的元件补上去"。所以即使 CF1/SEL 引脚接法正确，
 * 电压/电流读数也可能一直是 0，直到补齐元件。功率(CF)不受此影响。
 * 另外该文还提到功率计量电路本身偏脆弱，BL0937 损坏会导致功率恒为 0。
 *
 * ---- 硬件连接 ----
 *	 CF	 -> 有功功率脉冲输出，频率与有功功率成正比
 *	 CF1 -> 电压/电流脉冲输出，由 SEL 引脚选择是哪一路
 *	 SEL -> 电平选择 CF1 的输出对象（极性见 BL0937_SEL_CURRENT_HIGH）
 *
 * 只接 CF 也能完整使用功率与电量统计。没接的线把对应的
 * BL0937_USE_xxx 改成 0，驱动会自动跳过。
 */

/* ---- 总开关 ----
 * 0 = 完全不碰 BL0937 硬件：不配引脚、不开中断、不建定时器、不读写校准数据。
 *
 * 用途是【二分定位】。设备刷完起不来时，把这里改成 0 重新编译烧录：
 *	 - 改 0 之后恢复正常 -> 问题出在计量这部分（引脚/中断/userbin 存储）
 *	 - 改 0 之后依旧不正常 -> 与计量无关，另找原因
 *
 * 开机时串口会打 [BL] 1 ~ [BL] B 共 11 行标记，最后出现的那一行就是出错位置。
 */
#ifndef BL0937_ENABLE
#define BL0937_ENABLE 1
#endif

/* ---- 引脚（fid，对应 app_main.c 里 hf_gpio_fid_to_pid_map_table 的下标） ---- */
#define BL0937_CF_FID  (HFGPIO_F_USER_DEFINE + 8)  /* LPx30_GPIO7  —— 原 POWER 脚 */
#define BL0937_CF1_FID (HFGPIO_F_USER_DEFINE + 9)  /* LPx30_GPIO4  */
#define BL0937_SEL_FID (HFGPIO_F_USER_DEFINE + 10) /* LPx30_GPIO12 */

/* ---- 每根线是否接线 ----
 * 用 #ifndef 包着，这样既能在源码里改，也能在命令行覆盖（-DBL0937_USE_CF1=1），
 * Keil 里对应 C/C++ → Preprocessor Symbols。
 *
 * ★ 但是【不能全开】—— 全系统同时最多只有 2 个 GPIO 中断引脚，详下。
 */
#ifndef BL0937_USE_CF
#define BL0937_USE_CF 1 /* 有功功率。必须开，这是计量的主功能 */
#endif
#ifndef BL0937_USE_CF1
#define BL0937_USE_CF1 0 /* ← 默认【关】，原因见下面的"2 个中断通道" */
#endif
#ifndef BL0937_USE_SEL
#define BL0937_USE_SEL 0 /* SEL 只为 CF1 服务，CF1 关了它就没意义 */
#endif

/*
 * ================== GPIO 中断只有 2 个通道（踩过的坑） ==================
 *
 * BL0937 的 CF 和 CF1 都得靠数脉冲，最自然的做法是给两根脚各开一个 GPIO
 * 中断。但 HF-LPX30 的 GPIO 中断【总共只有 2 个通道】，而按键已经占了一个。
 * 于是 CF（第 2 个）通、CF1（第 3 个）炸：
 *   gpio_irq_init()  → MBED_ASSERT(CHANNEL_NUM != obj->ch)
 *   （原文见 libLPBX3Kernel.a 里 gpio_irq_api.o 的 .rodata.str1.1）
 * 断言之后是静默停机 + 看门狗复位，串口上【一个字都不多】，
 * 表现为"灯亮、按键没反应、不出热点、日志一遍遍从头刷"。
 * 复现特征：开机日志正好停在
 *   [BL] 8 cf1 pin ret=0
 * 而不出现 [BL] 9。上一版就是这样卡死的。
 *
 * 所以三个用中断的引脚（按键 / CF / CF1）只能要两个，默认选：
 *   按键(1) + CF(1) = 2 个通道  →  功率和电量完全准确
 *
 * 电压/电流怎么办？两个办法，都不用 CF1：
 *   - 电压：用标定里的 nominal_v（默认 230V，网页"校准"页可改），
 *     市电电压本来就很稳，写死一个实测值完全够用；
 *   - 电流：直接由 P/V 算出来（首页/状态页显示的就是这个）。
 * 另外原板 CF1 那一路本来就"可能缺采样元件"（limbang/TC1A2 的说法），
 * 补件之前接了也是 0 —— 默认关掉反而少一个误导项。
 *
 * ★ 想真用 CF1 测电压/电流：把下面两行改成
 *       #define BL0937_USE_CF1 1
 *       #define BL0937_USE_SEL 1
 *   同时把 user_gpio.h 里的 KEY_USE_IRQ 改成 0（按键改走轮询，功能不变）。
 *   通道数由 user_bl0937.c 里的 #error 兜底，配错了会直接编译失败，
 *   不会再出现"烧进去才发现起不来"。
 *
 * ★★ 第二个坑（同样致命，改代码时容易踩）：
 *   gpio_irq_init() 是在【进入函数的最开头】检查"两个通道是否都被占用"，
 *   而 hfgpio_configure_fpin_interrupt() **无论 enable 传 0 还是 1 都会调它**。
 *   所以在通道已满的情况下，想"关掉某个中断"而再去调
 *   hfgpio_configure_fpin_interrupt(fid, type, handler, 0) —— 一样会停机！
 *   关中断必须用 hfgpio_fdisable_interrupt(fid)（底层只走 gpio_irq_set，不碰通道）。
 *   本驱动的"中断风暴保护"就是按后者写的。
 * =====================================================================
 */

/*
 * BL0937 手册原文：SEL = 0 时 CF1 输出【电流】RMS 脉冲，SEL = 1 时输出【电压】RMS。
 * 即"测电流"对应 SEL 低电平，所以这里取 0。
 * （SEL 内部有下拉，悬空即默认 0 = 电流档。）
 * 若你的板上 SEL 有反相或有上拉，改成 1。
 */
#define BL0937_SEL_CURRENT_HIGH 0

/* 1 秒窗口内允许的脉冲数上限，超过视为引脚悬空/强干扰，判为无效数据 */
#define BL0937_MAX_PULSE_PER_SEC 20000

/*
 * ---- 低功率测量：脉冲周期法（方案 2，A1 思路修正版） ----
 *
 * 窗口计数法的量化台阶 = 1000/kp ≈ 1.71W（BL0937B 为 1.395W）：
 * 1 秒数到的脉冲是整数，1.7W 以下的负载读 0 或跳变，电量也跟着漏。
 *
 * 周期法用"最近两个 CF 脉冲的间隔"反推功率：P = 1000/kp × 1000ms/period_ms。
 * 分辨率取决于毫秒时钟（rt_time_get 读 os_time 变量，ISR 安全，已反汇编确认）：
 * 1.71W → 10s 间隔照测，0.17W（100s 间隔）也可测，只是刷新慢。
 *
 * 两种方法的误差交叉点在 cf ≈ 700Hz（计数 ±0.5 脉冲 vs 周期 ±1ms），
 * 远高于任何真实功率（kp=584.8 时 20 脉冲/秒 = 34W），取 20 意味着
 * 常见负载（<34W）全部走周期法，精度最好。
 */
#define BL0937_COUNT_MODE_MIN 20
/* 脉冲间隔短于此判为毛刺（真实功率 ≥ 计数法边界约 34W 时窗口里早有
 * ≥20 个脉冲、走计数法了），防止窄干扰成对出现时算出假大功率 */
#define BL0937_PERIOD_MIN_MS 3
/* 宽限：距最后一个脉冲超过 2×周期+此值 仍未等到下一个 → 判定负载消失，功率归零 */
#define BL0937_PERIOD_GLUE_MS 500

/*
 * 连续多少个窗口超限，就彻底关掉那一路的 GPIO 中断。
 * 引脚悬空时中断可能打到几十万次/秒，ISR 会把 CPU 吃光，整机表现为
 * "灯亮、按键没反应、不出热点" —— 关掉中断是为了保住整机。
 */
#define BL0937_STORM_LIMIT 3

/*
 * 校准系数默认值：取自 A1 版第三方固件的真机实测标定（FUjr/tc1_a1_firmware，
 * TC1/user_power.h）。A1 与 A2 仅主控不同，计量板完全相同，系数可直接沿用。
 *
 * ★★ A1 的功率单位是 0.1W（分瓦）不是 W —— 铁证 user_mqtt_client.c:929
 *	 sprintf(..., "{\"power\":\"%.3f\"}", real_time_power / 10);
 * 所以真机实测换算：
 *	 POWER_COEFFICIENT_BL0937_X100  = 1710 → 17.10 × 0.1 = 1.710 W/(脉冲/秒)
 *	 POWER_COEFFICIENT_BL0937B_X100 = 1395 → 1.395 W/(脉冲/秒)（换过 BL0937B 的板）
 * 交叉验证（三条独立证据链全部指向 1.71 而非 17.10）：
 *	 ① BL0937 手册公式 + TC1 外围（分压 1711、1mΩ）推导 Kp≈678 脉冲/kW，
 *	    即 1.475 W/Hz —— 与 1.71 差 14%（元件容差），与 17.10 差 11.6 倍（不可能）；
 *	 ② A1 的电量公式 coeff×N/3.6e7 在 0.1W 单位下精确成立（17.10/3.6e7
 *	    = 1.71/3.6e6 = 每脉冲 4.75e-7 kWh），单位按 W 理解它才是"bug"；
 *	 ③ A1 校准合法区间 X100=100~10000（0.1~10 W/Hz），17.10 越界、1.71 在内。
 *	 ④ 实测方糖待机 CF≈1.368Hz：×1.71=2.34W（符合常识），×17.10=23.4W（不可能）。
 * 换算成"1kW 时的 CF 脉冲率"：kp = 1000 / (W/Hz)
 *	 BL0937  → 1000 / 1.71 ≈ 584.8
 *	 BL0937B → 1000 / 1.395 ≈ 716.8
 */
#define BL0937_COEFF_W_PER_HZ_BL0937  1.710f /* A1 真机实测 ÷10（其功率单位 0.1W） */
#define BL0937_COEFF_W_PER_HZ_BL0937B 1.395f
#define BL0937_DEF_PULSES_PER_KW 584.8f
#define BL0937_DEF_PULSES_PER_V	 7.39f
#define BL0937_DEF_PULSES_PER_A	 77.7f

/*
 * CF1 关闭时用的"电网电压"（V）。市电电压常年很稳，直接当常数用，
 * 电流由 P/V 反算。网页"校准"页填一次实测值即可。
 */
#define BL0937_DEF_NOMINAL_V 220.0f

/*
 * ★ 校准结构版本（magic 即版本）。0xB938 → 0xB939 的原因：
 * 0xB938 版存的是 58.48（A1 系数漏除其 0.1W 单位，大了 10 倍），
 * 不可信。bump 之后旧 flash 数据 magic 不匹配，开机自动载入正确默认
 * 584.8 并重写，无需用户手工清理。
 */
#define BL0937_CAL_MAGIC 0xB939

typedef struct _BL0937_CAL
{
	uint16_t magic;
	float power_pulses_per_kw;	/* 1000W 时 CF 每秒脉冲数 */
	float voltage_pulses_per_v; /* 1V 时 CF1 每秒脉冲数 */
	float current_pulses_per_a; /* 1A 时 CF1 每秒脉冲数 */
	float nominal_v;			/* CF1 关闭时假定的电网电压 */
	uint16_t max_power;			/* 过载保护阈值（W），0 = 关闭 */
	uint16_t crc;
} BL0937_CAL;

typedef struct _BL0937_DATA
{
	volatile uint32_t cf_cnt;  /* CF 中断计数（ISR 累加） */
	volatile uint32_t cf1_cnt; /* CF1 中断计数（ISR 累加） */
	volatile uint32_t cf_period_ms; /* 最近两个 CF 脉冲的间隔 ms（ISR 维护） */
	volatile uint32_t cf_pulse_total; /* CF 脉冲总数（ISR 累加，≥2 才有有效周期） */

	uint32_t cf_hz;	 /* 最近一个窗口的 CF 频率（脉冲数/秒） */
	uint32_t cf1_hz; /* 最近一个窗口的 CF1 频率（脉冲数/秒） */
	float cf_eff_hz; /* 有效脉冲率：计数法=窗口数，周期法=1000/period_ms（标定用） */

	float voltage; /* V */
	float current; /* A */
	float power;   /* W */

	uint64_t energy_mwh;   /* 累计电量，单位毫瓦时 */
	uint32_t energy_start; /* 累计起始时间戳（UTC+8 秒） */

	uint32_t today_mwh;		/* 今日电量，单位毫瓦时 */
	uint32_t yesterday_mwh; /* 昨日电量，单位毫瓦时 */
	int16_t today_dayno;	/* 今日 day-of-year，-1 表示时间未知 */
	uint8_t today_valid;

	uint8_t valid;	  /* 1 = 计量数据可信 */
	uint8_t sel_state; /* 1 = 本窗口 CF1 测的是电流，0 = 电压 */
	uint8_t sel_lock;  /* 0 = 自动轮换，1 = 锁定电压档，2 = 锁定电流档（标定用） */
	uint8_t running;
} BL0937_DATA;

extern BL0937_CAL bl0937_cal;
extern BL0937_DATA bl0937;

void USER_FUNC bl0937_init(void);
void USER_FUNC bl0937_loop(void); /* 由 1 秒周期任务调用 */
void USER_FUNC bl0937_energy_clear(void);

/* CF1 档位锁定（标定用）：0 = 自动轮换，1 = 锁电压档，2 = 锁电流档。
 * 会立即刷新 SEL 引脚电平，不必等下一个采样周期。 */
void USER_FUNC bl0937_sel_lock(uint8_t mode);

/* 校准系数落盘同样拆成两步（理由见 .c 里的 bl0937_cal_save_flag 说明）：
 *   bl0937_cal_save()     只置标志，可在任意线程/回调里调；
 *   bl0937_cal_save_now() 真正写 flash，只能在 userbin 保存线程里调。 */
extern bool bl0937_cal_save_flag;
void USER_FUNC bl0937_cal_load(void);
void USER_FUNC bl0937_cal_save(void);
void USER_FUNC bl0937_cal_save_now(void);

/* 电量落盘拆成两步：
 *   bl0937_save_energy()     只置"待落盘"标志，可在定时器回调里安全调用；
 *   bl0937_save_energy_now()  真正写 flash，只能在 userbin 保存线程里调
 *                            （hffile_userbin_write 在 hftimer 上下文会复位设备）。 */
extern bool bl0937_energy_save_flag;
void USER_FUNC bl0937_save_energy(void);
void USER_FUNC bl0937_save_energy_now(void);
void USER_FUNC bl0937_load_energy(void);

/* 电量持久化"自证"计数（/status 暴露 en_load / en_save / en_err）。
 *
 * 背景：电量之前几十年如一日"每次重启都从 0 爬"，但代码链路看起来是完整的，
 * 光看代码分不清到底是"没写"还是"写了没读回来"。把这三件事做成可观测量，
 * 烧录后看一眼 /status 就能定论：
 *   en_load = 1  → 本次开机 flash 里的电量记录有效，总计是从 flash 续上的
 *   en_save > 0  → 落盘确实发生过（每成功写一次 +1）
 *   en_err  > 0  → hffile_userbin_write 返回长度不符，写 flash 失败
 * 判据：正常设备跑 10 小时后 en_save 应 ≥1 且下次开机 en_load=1；
 *      若 en_save 一直 0 而 en_err 也 0，说明落盘线程压根没跑（标志无人消费）。 */
extern uint8_t bl0937_energy_load_ok;
extern uint32_t bl0937_energy_save_ok;
extern uint32_t bl0937_energy_save_err;

/* 浮点转定点字符串。newlib-nano 的 printf 默认不带浮点，页面输出统一走这里 */
void USER_FUNC bl0937_fmt_float(char *out, float v, uint8_t dec);

float USER_FUNC bl0937_energy_kwh(void);
float USER_FUNC bl0937_today_kwh(void);
float USER_FUNC bl0937_yesterday_kwh(void);

#endif

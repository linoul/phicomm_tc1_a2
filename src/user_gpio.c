
#include "app_main.h"
#include "user_gpio.h"
#include "user_rtc.h"
#include "user_mqtt.h"
#include "user_config.h"
#include <string.h>

#ifdef HK_ENABLE
#include "homekit/hk_net.h"
#endif

// 按键的回调函数里不能发送 mqtt, 不然会重启

#define TIMER1_ID (1) //定时器id

hfthread_mutex_t keythread_lock = NULL_MUTEX;
uint8_t key_time = 0;
uint8_t led_status = 0;
#if !KEY_USE_IRQ
/* KEY_USE_IRQ=0 时的"上一拍按键是按下状态"记录，用来在松手那一拍补出边沿 */
static uint8_t key_poll_down = 0;
#endif
// time_t key_press_time = 0;

int relay[] = {SW1, SW2, SW3, SW4, SW5, SW6}; //对应每路继电器GPIO

/*
 * ---- 按键：连击 + 长按映射 + 童锁（参考 A1 固件 tc1_a1_firmware 的思路） ----
 *
 * 映射表 user_plug_config.key_map[KEY_MAP_NUM]：
 *   [0]=单击 [1]=双击 [2]=长按约3s [3]=长按约5s [4]=长按约10s
 * 值是 KEY_FUNC_xxx 功能码，Web"按键"页可改，掉电保存。
 *
 * 判定（TIMER1 100ms 一拍）：
 *   - 按下累计 key_time；松手时 <10 拍(1s) 算短按，计入连击并等 400ms
 *     看有没有下一击（click_wait），到点把累计击数映射成动作；
 *   - 松手时 >=10 拍算长按，按 1~4s / 4~8s / >=8s 归入三档立即触发；
 *   - 按住期间的 LED 闪烁只是视觉反馈（50 拍起每 5 拍翻转、100 拍起每 2 拍）。
 *
 * 童锁：user_plug_config.enable_child_lock=1 时，除"切换童锁"（双击默认）
 * 以外的一切按键功能都被忽略 —— 否则锁死了没有物理解锁出口。
 * 救命通道不在这里：上电时按住按键会直接进配网（见 app_main.c）。
 */
static uint8_t click_cnt = 0;  /* 已累计的连击数（封顶 2，三击并入双击） */
static uint8_t click_wait = 0; /* 松手后等下一击的剩余拍数，100ms/拍 */

/* 执行一个按键槽位的功能。童锁在这里统一拦截。
 * 传槽位下标而不是功能码，因为"单开"还要查同槽位的 key_mask。 */
static void USER_FUNC key_exec(uint8_t slot)
{
	uint8_t f = user_plug_config.key_map[slot];

	if (f == KEY_FUNC_NONE)
		return;

	if (user_plug_config.enable_child_lock && f != KEY_FUNC_CHILD_LOCK)
		return;

	if (f == KEY_FUNC_ALL)
	{
		if (relay_out())
			user_relay_set_all(0);
		else
			user_relay_set_all(1);
	}
	else if (f == KEY_FUNC_SOLO)
	{
		/* 单开：只翻转 key_mask 勾选的那几路（bit0~bit5=口1~口6）。
		 * 一个没勾就不动 —— 防止配了单开却忘勾插口时按键"失灵"困惑。 */
		uint8_t m = user_plug_config.key_mask[slot];
		uint8_t i;

		for (i = 0; i < PLUG_NUM; i++)
			if (m & (1u << i))
				user_relay_set(i, plug_status.plug[i] ? 0 : 1);
	}
	else if (f == KEY_FUNC_REBOOT_SOFT)
	{
		system_soft_restart_flag = true; /* rtc 线程执行，回调里置位安全 */
	}
	else if (f == KEY_FUNC_AP)
	{
		system_start_ap_flag = true;
	}
	else if (f == KEY_FUNC_SMTLK)
	{
		hfsmtlk_start();
	}
	else if (f == KEY_FUNC_REBOOT)
	{
		system_restart_flag = true;
	}
	else if (f == KEY_FUNC_CHILD_LOCK)
	{
		/* 切换童锁并落盘。哪怕童锁已开也放行 —— 这就是解锁出口。 */
		user_plug_config.enable_child_lock = user_plug_config.enable_child_lock ? 0 : 1;
		update_plug_config_flag = true;
	}
}

/* 连击窗口到期：把累计的击数映射成动作 */
static void USER_FUNC key_click_fire(void)
{
	uint8_t n = click_cnt;

	click_cnt = 0;
	if (n == 0)
		return;
	if (n > 2)
		n = 2;
	key_exec(n - 1);
}

void USER_FUNC user_led_set(char x)
{
    if (x == -1)
    {
        if (hfgpio_fpin_is_high(LED))
        {
            hfgpio_fset_out_low(LED);
            led_status = 0;
        }
        else
        {
            hfgpio_fset_out_high(LED);
            led_status = 1;
        }
    }
    else if (x)
    {
        hfgpio_fset_out_high(LED);
        led_status = 1;
    }
    else
    {
        hfgpio_fset_out_low(LED);
        led_status = 0;
    }
}

bool USER_FUNC relay_out(void)
{
    uint8_t i;
    for (i = 0; i < PLUG_NUM; i++)
    {
        if (plug_status.plug[i] != 0)
        {
            return true;
        }
    }
    return false;
}

/*user_relay_set
 * 设置继电器开关
 * x:编号 0-5
 * y:开关 0:关 1:开
 */
void USER_FUNC user_relay_set(uint8_t x, uint8_t y)
{

    if (x >= PLUG_NUM)
        return;

    if (y == 1)
        hfgpio_fset_out_high(relay[x]);
    else
        hfgpio_fset_out_low(relay[x]);

    plug_status.plug[x] = y;
    update_plug_status_flag[x] = true;
    /*
     * 通知 HomeKit（第四阶段）。hk_net_notify 只置一个位掩码就返回，
     * 不碰网络 —— 这里会被按键回调、MQTT 回调、httpd 回调共同调用，
     * 任何阻塞都会把调用方拖死。真正的发送由 HAP 线程轮询去做。
     */
#ifdef HK_ENABLE
    hk_net_notify(x, y ? 1 : 0);
#endif
    if (relay_out())
        user_led_set(1);
    else
        user_led_set(0);
}

uint8_t USER_FUNC key_status(void)
{
    if (hfgpio_fpin_is_high(KEY))
    {
        return KEY_UP;
    }
    else
    {
        return KEY_DOWN;
    }
}

/*
 * 设置所有继电器开关
 * y:0:全部关   1:根据记录状态开关所有
 *
 */
void USER_FUNC user_relay_set_all(uint8_t y)
{
    uint8_t i;
    for (i = 0; i < PLUG_NUM; i++)
        user_relay_set(i, y);
}

void USER_FUNC key_timeout_handler(void *arg)
{
    // any button pressed
    if (key_status() == KEY_DOWN)
    {
#if !KEY_USE_IRQ
        /* 轮询模式下这里记下"按着"，松手时（下面那个分支）才触发动作 */
        key_poll_down = 1;
#endif
        /* 新的一击开始：取消还在等待触发的连击 */
        click_wait = 0;

        // 100ms 每次
        key_time++;

        if (key_time > 200)
        {
            key_time = 151;
        }
        else if (key_time >= 100)
        {
            if (key_time % 2 == 0)
            {
                user_led_set(1-led_status);
            }
        }
        else if (key_time >= 50)
        {
            if (key_time % 5 == 0)
            {
                user_led_set(1-led_status);
            }
        }
    }
    else
    {
#if !KEY_USE_IRQ
        /* 轮询模式：上一次还按着、这一次松了 —— 就是"松手沿"。
         * 和中断模式一样交给 key_rising_irq_handler 判定按了多久，
         * 它内部会把 key_time 清零。必须赶在下面那句 key_time = 0 之前调。 */
        if (key_poll_down)
        {
            key_poll_down = 0;
            key_rising_irq_handler(0, 0);
        }
#endif
        key_time = 0;

        /* 松手状态下的每拍都在倒计时连击窗口 */
        if (click_wait > 0 && --click_wait == 0)
            key_click_fire();

        if (!relay_out())
        {
            user_led_set(0);
        }
    }
}

void USER_FUNC key_rising_irq_handler(uint32_t arg1, uint32_t arg2)
{
    if (key_time >= 10)
    {
        /* 长按（>=1 秒）：按时长归档 —— 1~4s→key_map[2]，4~8s→[3]，>=8s→[4] */
        uint8_t slot;

        if (key_time < 40)
            slot = 2;
        else if (key_time < 80)
            slot = 3;
        else
            slot = 4;

        key_time = 0;
        click_cnt = 0;
        click_wait = 0;
        key_exec(slot);
    }
    else if (key_time > 0)
    {
        /* 短按（<1 秒）：计入连击，等 400ms 看有没有下一击 */
        key_time = 0;
        if (click_cnt < 2)
            click_cnt++;
        click_wait = 4;
    }
}

void USER_FUNC key_init()
{
    if (hfthread_mutext_new(&keythread_lock) != 0)
    {
        HF_Debug(DEBUG_LEVEL, "create mutex fail\n");
        return;
    }
    hftimer_handle_t user_key_timer = hftimer_create("TIMER1", 100, 1, TIMER1_ID, key_timeout_handler, 0);
#if KEY_USE_IRQ
    /* 占一个 GPIO 中断通道。GPIO 中断一共只有 2 个通道（见 user_gpio.h），
     * BL0937 的 CF 要占另一个，所以 CF1 就只能放弃中断计数。 */
    hfgpio_configure_fpin_interrupt(KEY, HFPIO_IT_EDGE, key_rising_irq_handler, 1);
#else
    /* 不占通道：松手动作改由 100ms 定时器轮询电平触发（见 key_timeout_handler） */
#endif
    hftimer_start(user_key_timer);
}

void USER_FUNC load_key_status()
{
    for (uint8_t i = 0; i < PLUG_NUM; i++)
    {
        user_relay_set(i, plug_status.plug[i]);
    }
}

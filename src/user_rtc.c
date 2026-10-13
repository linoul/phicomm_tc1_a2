
#include "app_main.h"
#include "user_gpio.h"
#include "user_rtc.h"
#include <time.h>
#include <hsf.h>

#include "user_function.h"
#include "user_mqtt.h"
#include "user_bl0937.h"

#define TIMER2_ID (2) // timer2 for rtctime

time_t rtctimes = 1640966400; // 当前时间戳, init：2022-01-1 0:0:0

// 临时变量
struct tm *current_time; // 当前时间结构体
time_t utc8ts;           // UTC8 时间戳
time_t systime_last_ntp;
time_t systime_now;
time_t systime_wifi_link = 0;
time_t timestamp_start = 0;
bool ntptime_succeed = false;
char tmp_rsp[32] = {0};

// time_t ntp_res;

/*
 * ★★ NTP 同步：定时器回调只置标志，真正的联网调用交给 ntpsync 线程。
 *
 *   hfntp_get_time() 是【阻塞式联网】调用：要先 DNS 解析 ntpserver
 *   （"ntp1.aliyun.com"，见 user_config.c），再走 UDP 收发。把它放在
 *   hftimer 回调（rtc_timer_func，500ms）里执行，等于让阻塞网络 I/O 跑在
 *   SDK 定时器任务里 —— 实测会导致设备复位：
 *
 *     10/11 17:12:3x，开机满 10 小时 10 秒（正是下面 36000000 ms 的刷新点）
 *     设备复位了一次；再往前一次开机（10-11 07:12:2x）也是同一把时间尺子。
 *     实测间隔 10.00 h，与 36000000 + 开机 10s 首次同步的 10000 完全吻合。
 *
 *   同族教训（同一份代码里已经吃过两次）：
 *     - hffile_userbin_write() 在定时器回调里 → 复位（10/10 实测）；
 *     - key_exec() 在 GPIO 中断里发 mqtt   → 复位（user_gpio.c 顶部注释）。
 *   结论：凡是"阻塞 / 会碰网络或 flash"的动作，都不许在回调上下文里做。
 */
static volatile bool ntp_refresh_flag = false;

/* 诊断用计数（/status 里暴露） */
volatile uint32_t ntp_call_cnt = 0;
volatile uint32_t ntp_fail_cnt = 0;

void USER_FUNC do_update_rtc_time()
{
    /* 条件与上游完全一致，只是不再在这里联网：
     *   - 还没同步成功 → 每 10 秒重试一次；
     *   - 已同步成功   → 满 10 小时重新同步一次。 */
    if ((!ntptime_succeed && systime_now - systime_last_ntp > 10000) ||
        (systime_now - systime_last_ntp > 36000000))
    {
        ntp_refresh_flag = true;
    }
}

/* NTP 同步线程：本工程里【唯一】执行阻塞式 NTP 的地方。
 * 栈给 2048：DNS 解析 + socket 收发比 ubsave 那点结构体拷贝要重得多。 */
void USER_FUNC ntp_thread_func(void *arg)
{
    while (1)
    {
        msleep(200);
        if (!ntp_refresh_flag)
            continue;

        /* WiFi 没连上就别发：一来必然失败，二来失败会立刻触发 10 秒重试，
         * 变成"每 10 秒一次 DNS 超时"的空转（断网整夜时尤其明显）。 */
        if (!hfwifi_sta_is_connected())
            continue; /* 标志保持置位，等 WiFi 回来再补 */

        ntp_refresh_flag = false;

        /* 先记"调用起点"：语义与改前一致（改前回调被阻塞，systime_now
         * 停在调用开始那一刻，所以 systime_last_ntp 记的就是起点）。 */
        time_t t_call = hfsys_get_time();
        systime_last_ntp = t_call;

        ntp_call_cnt++;
        int time_mm = hfntp_get_time(ntpserver, 123, 200);
        // ntp_res = time_mm;
        if (time_mm < 0)
        {
            ntptime_succeed = false;
            ntp_fail_cnt++;
        }
        else
        {
            timestamp_start = time_mm - t_call / 1000;
            ntptime_succeed = true;
        }
    }
}

void USER_FUNC do_update_wifi_time()
{
    if (user_plug_config.enable_auto_restart == 0)
    {
        return;
    }
    // if (systime_now - systime_wifi_link < 29000)
    // {
    //     return;
    // }
    // if (ping(user_mqtt_config.seraddr) == HF_SUCCESS)
    // {
    //     systime_wifi_link = systime_now;
    // }
    if (hfwifi_sta_is_connected())
    {
        systime_wifi_link = systime_now;
    }
    else if (user_plug_config.enable_auto_restart == 1 && systime_now - systime_wifi_link > 600000)
    {
        system_soft_restart_flag = true;
    }
    else if (user_plug_config.enable_auto_restart > 1 && systime_now - systime_wifi_link > 600000)
    {
        system_restart_flag = true;
    }
}

void USER_FUNC do_schedule_tasks()
{
    if (!user_plug_config_enable)
    {
        return;
    }

    update_time();
    bool has_tasks = false;
    for (uint8_t i = 0; i < PLUG_NUM; i++)
    {
        for (uint8_t j = 0; j < PLUG_TIME_TASK_NUM; j++)
        {
            if (user_plug_config.plug[i].task[j].enable != 0)
            {
                if (current_time->tm_sec == user_plug_config.plug[i].task[j].second &&
                    current_time->tm_min == user_plug_config.plug[i].task[j].minute &&
                    current_time->tm_hour == user_plug_config.plug[i].task[j].hour &&
                    ((1 << (current_time->tm_wday + 1)) & user_plug_config.plug[i].task[j].repeat) > 0)
                {
                    if (plug_status.plug[i] != user_plug_config.plug[i].task[j].action)
                    {
                        user_relay_set(i, user_plug_config.plug[i].task[j].action);
                        update_plug_status_flag[i] = true;
                    }
                    if ((user_plug_config.plug[i].task[j].repeat & 1) == 0)
                    {
                        user_plug_config.plug[i].task[j].enable = 0;
                        update_plug_config_flag = true;
                    }
                    else
                        has_tasks = true;
                }
                else
                {
                    has_tasks = true;
                }
            }
        }
    }
    user_plug_config_enable = has_tasks;
}

void USER_FUNC do_update_plug_config()
{
    if (update_plug_config_flag)
    {
        update_plug_config_flag = false;
        save_plug_config(&user_plug_config);
    }
}

void USER_FUNC do_update_plug_status()
{
    bool need_update = false;
    for (uint8_t i = 0; i < PLUG_NUM; i++)
    {
        if (update_plug_status_flag[i])
        {
            need_update = true;
            update_plug_status_flag[i] = false;
            mqtt_report_plug_status(i);
        }
    }
    if (need_update)
    {
        /* 注意：save_plug_status 走的是 hfuflash_*（裸 flash 直写），
         * 不是 hffile_userbin_write —— 它在上游固件里本来就在定时器回调里调，
         * 行为正常，这里保持不动。只有 userbin 文件层的写才必须挪走。 */
        save_plug_status(&plug_status);
    }
    return;
}

void USER_FUNC do_update_mqtt_config()
{
    if (update_mqtt_config_flag)
    {
        /* ★ 上游这里清的是 update_plug_config_flag（笔误），
         * 导致 update_mqtt_config_flag 永不清零 → 之后每一拍都往 flash
         * 重写一次 MQTT 配置（既费寿命又拖慢系统）。一并修掉。 */
        update_mqtt_config_flag = false;
        update_mqtt_config(&user_mqtt_config);
        save_mqtt_config(&user_mqtt_config);
    }
}

/*
 * ★★ userbin（flash 文件层）统一落盘线程 —— 本文件最重要的一条约定。
 *
 *   hffile_userbin_write() 绝对不能在 hftimer 回调（rtc_timer_func /
 *   bl0937_timer_func）上下文里调用，会直接复位设备。10/10 实测：
 *     GET /api?act=kt&p=2&t=0&h=04&...  → 回 {"ok":1}，约 2 秒后设备重启，
 *                                        任务自然没写进去（"保存不生效"），
 *                                        重启后 /task 读到的还是旧值。
 *     同一条 save_plug_config() 放在 httpd 回调里（act=ks）则完全正常。
 *
 *   所以所有 userbin 写入都收敛到这条低优先级线程：定时器 / 中断 /
 *   HTTP / MQTT 回调只置标志位，由这里统一落盘。
 *   以后新增需要落盘的数据，请照此办理（加标志 + 在这里消费）。
 *
 *   栈 1024：只做结构体拷贝 + hffile_userbin_write。
 */
void USER_FUNC userbin_save_thread_func(void *arg)
{
    (void)arg;
    while (1)
    {
        msleep(200);
        do_update_plug_config();
        do_update_mqtt_config();
        if (bl0937_energy_save_flag)
            bl0937_save_energy_now();
        if (bl0937_cal_save_flag)
            bl0937_cal_save_now();
    }
}

void USER_FUNC rtc_thread_func(void *arg)
{
    while (1)
    {
        msleep(100);
        systime_now = hfsys_get_time();
        if (system_restart_flag)
        {
            system_restart_flag = false;
            hfsys_reset();
        }

        if (system_soft_restart_flag)
        {
            system_soft_restart_flag = false;
            hfsys_softreset();
        }

        if (system_start_ap_flag)
        {
            system_start_ap_flag = false;
            hfat_send_cmd("AT+WMODE=AP\r\n",sizeof("AT+WMODE=AP\r\n"),tmp_rsp,32);
            system_soft_restart_flag = true;
        }

        do_update_rtc_time();

        do_update_wifi_time();

        do_schedule_tasks();

        do_update_plug_status();

        /* ★ userbin 落盘刻意不在这里做：本函数是 hftimer 回调，
         *   hffile_userbin_write() 在这个上下文里会复位设备。
         *   改由 userbin_save_thread_func() 消费标志位后落盘。 */
    }
}

void USER_FUNC rtc_timer_func(void *arg)
{
    systime_now = hfsys_get_time();
    if (system_restart_flag)
    {
        system_restart_flag = false;
        hfsys_reset();
    }

    if (system_soft_restart_flag)
    {
        system_soft_restart_flag = false;
        hfsys_softreset();
    }

    if (system_start_ap_flag)
    {
        system_start_ap_flag = false;
        hfat_send_cmd("AT+WMODE=AP\r\n",sizeof("AT+WMODE=AP\r\n"),tmp_rsp,32);
        system_soft_restart_flag = true;
    }

    do_update_rtc_time();

    do_update_wifi_time();

    do_schedule_tasks();

    do_update_plug_status();

    /* ★ 同 rtc_thread_func：userbin 落盘交给 userbin_save_thread_func()。 */
}

// void USER_FUNC rtctime_timeout_handler(void *arg)
// {
//     rtctimes = rtctimes + 1;
// }

void USER_FUNC rtctime_init(void)
{
    systime_last_ntp = hfsys_get_time();
    systime_now = systime_last_ntp;
    hftimer_handle_t user_rtc_timer = hftimer_create("TIMER2", 500, 1, TIMER2_ID, rtc_timer_func, 0); //每一秒加一,模拟时钟在跑
    // hfthread_create(rtc_thread_func, "rtccontrol", 1024, NULL, HFTHREAD_PRIORITIES_LOW, NULL, NULL);
    // hftimer_handle_t user_rtc_timer = hftimer_create("TIMER2", 1000, 1, TIMER2_ID, rtctime_timeout_handler, 0); //每一秒加一,模拟时钟在跑
    hftimer_start(user_rtc_timer);

    /* 所有 userbin 落盘都在这条线程里做（见 userbin_save_thread_func 说明）。
     * 定时器回调只置标志 —— 千万别把 hffile_userbin_write 挪回回调里。
     *
     * 栈 2048：这条线程背着"所有 flash 写入"，里面会经过 hffile_userbin_write
     * （闭源 SDK，内部开销未知）+ 若干结构体拷贝。原来给 1024，比只做联网的
     * MQTT 线程(512)也就宽一倍，属于"够用但不放心"；涨到 2048 与 ntpsync 看齐，
     * 换来的是落盘这条路不会再因为栈紧而悄悄出问题。 */
    if (hfthread_create(userbin_save_thread_func, "ubsave", 2048, NULL,
                        HFTHREAD_PRIORITIES_LOW, NULL, NULL) != HF_SUCCESS)
        u_printf("[rtc] start ubsave thread FAIL\r\n");

    /* NTP 同步也单开一条线程（见 do_update_rtc_time 上方说明）：
     * 阻塞式 DNS/UDP 调用不能跑在 hftimer 回调里，否则满 10 小时那次
     * 自动重同步会把设备打复位。 */
    if (hfthread_create(ntp_thread_func, "ntpsync", 2048, NULL,
                        HFTHREAD_PRIORITIES_LOW, NULL, NULL) != HF_SUCCESS)
        u_printf("[rtc] start ntpsync thread FAIL\r\n");
}

void USER_FUNC update_time()
{
    utc8ts = timestamp_start + systime_now / 1000 + UTC8;
    current_time = gmtime(&utc8ts);
}

void USER_FUNC get_time_string(char *res, int len)
{
    update_time();
    strftime(res, len, "%Y-%m-%d %H:%M:%S", current_time);
    return;
}

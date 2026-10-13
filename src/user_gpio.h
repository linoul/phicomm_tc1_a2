
#ifndef __USER_KEY_H_
#define __USER_KEY_H_

#include <hsf.h>

#define SW1	(HFGPIO_F_USER_DEFINE)
#define SW2	(HFGPIO_F_USER_DEFINE+1)
#define SW3	(HFGPIO_F_USER_DEFINE+2)
#define SW4	(HFGPIO_F_USER_DEFINE+3)
#define SW5	(HFGPIO_F_USER_DEFINE+4)
#define SW6	(HFGPIO_F_USER_DEFINE+5)
#define LED	(HFGPIO_F_USER_DEFINE+6)
#define KEY	(HFGPIO_F_USER_DEFINE+7)
#define POWER	(HFGPIO_F_USER_DEFINE+8)

#define KEY_DOWN 1
#define KEY_UP 0

/*
 * ---- 按键是否占用一个 GPIO 中断通道 ----
 *
 * 【硬件事实】HF-LPX30 的 GPIO 中断只有 【2 个通道】。
 * 证据（从 sdk/4.14/libs/libLPBX3Kernel.a 里 gpio_irq_api.o 反汇编得到）：
 *   gpio_irq_init()  里 channel_ids[] 只有 2 项，第 3 个通道会走到
 *     MBED_ASSERT(CHANNEL_NUM != obj->ch)
 *   字符串原文在 .rodata.str1.1 里："CHANNEL_NUM != obj->ch"
 *   gpio_irq_set()   里 MBED_ASSERT(obj->ch <= 1)
 * 断言触发后是"静默停机 + 看门狗复位"，串口上什么都看不到 —— 表现就是
 * 灯亮、按键没反应、不出热点、日志一遍遍从头刷。
 *
 * 所以【全系统同时最多只能有 2 个 GPIO 中断引脚】。谁占用：
 *   - 按键（本文件，key_init 里注册）
 *   - BL0937 的 CF（功率，必须）
 *   - BL0937 的 CF1（电压/电流）
 * 三个只能要两个。默认给按键 + CF；CF1 默认关闭（见 user_bl0937.h）。
 *
 * KEY_USE_IRQ = 0 → 按键不占中断通道，改成在 100ms 定时器里轮询电平来
 * 判断"松手"，功能完全一样，只是按下时长最多差 100ms（最短的判定档是
 * 1 秒，人按不出来 100ms 的差别）。这样就能把通道让给 CF1。
 * 两个开关加起来不能超过 2，超了 user_bl0937.c 里有 #error 兜底。
 */
#ifndef KEY_USE_IRQ
#define KEY_USE_IRQ 1
#endif


void USER_FUNC user_led_set(char x);
void USER_FUNC key_init(void);
void USER_FUNC user_relay_set(uint8_t x,uint8_t y );
void USER_FUNC user_relay_set_all( uint8_t y );
bool USER_FUNC relay_out( void );
/* 按键动作统一走 key_exec()（功能码见 user_config.h 的 KEY_FUNC_xxx），
 * 童锁拦截也在里面；连击/长按的判定在 user_gpio.c 内部，外部无需关心。 */
void USER_FUNC key_timeout_handler( void* arg );
/* 按下动作在松手时判定。原先这里声明的是 key_falling_irq_handler /
 * key_Rising_irq_handler 两个名字，但全工程既没有定义也没有引用，是上游遗留的
 * 错名字；真正被 key_init() 注册、也需要被轮询模式调用的就是这个。 */
void USER_FUNC key_rising_irq_handler( uint32_t arg1,uint32_t arg2 );
void appRestoreDefault(  void  );

uint8_t USER_FUNC key_status(void);
void USER_FUNC load_key_status(void);
 
 void printTask();
#endif

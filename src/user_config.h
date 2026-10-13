
#ifndef __USER_STRUCT_H_
#define __USER_STRUCT_H_

#include <hsf.h>

#define VERSION "v2.1"
#define TYPE 1
#define TYPE_NAME "TC1A2"

// #define SETTING_MQTT_STRING_LENGTH_MAX 32 //必须 4 字节对齐。

#define PLUG_NAME_LENGTH 32
#define PLUG_NUM 6           //插座数量
#define PLUG_TIME_TASK_NUM 5 //每个插座最多5组定时任务

/* ---- 按键功能码（key_map[] 与 Web"按键"页共用，参考 A1 固件的功能码思路） ----
 * 注意：Web 的 /k.js 里 F[] 文案数组顺序必须与此一一对应。 */
#define KEY_FUNC_NONE 0		 /* 无动作 */
#define KEY_FUNC_ALL 1		 /* 切换全部插座 */
#define KEY_FUNC_SOLO 2		 /* 单开：只切换 key_mask 勾选的那几路插座 */
#define KEY_FUNC_REBOOT_SOFT 3  /* 软重启（GPIO 状态保留，继电器不抖） */
#define KEY_FUNC_AP 4			 /* 进 AP 配网模式 */
#define KEY_FUNC_SMTLK 5		 /* SmartLink 配网 */
#define KEY_FUNC_CHILD_LOCK 6  /* 切换童锁（童锁开启时唯一放行的按键功能） */
#define KEY_FUNC_REBOOT 7		 /* 普通重启（硬复位，继电器会闪断） */

/* key_map 下标：0=单击 1=双击 2=长按约3秒 3=长按约5秒 4=长按约10秒。
 * 短按只识别单击/双击（三击并入双击），长按按时长归入三档。 */
#define KEY_MAP_NUM 5

#define NETWORK_ADDR_LEN (16) /* IP网络地址的长度 */
#define HAL_MAC_LEN (17 + 1)  /* MAC地址的长度 */

#define Relay_ON 1
#define Relay_OFF 0

#define Relay_NUM PLUG_NUM

#define MQTT_CONFIG_USERBIN_ADDR 0
#define MQTT_SERADDR_MAX_LEN 128
#define MQTT_CLIENTID_MAX_LEN 128
#define MQTT_USERNAME_MAX_LEN 128
#define MQTT_PASSWORD_MAX_LEN 128
#define MQTT_TOPIC_MAX_LEN 128
#define MQTT_MESSAGE_MAX_LEN 128
#define MQTT_MAGIC_HEAD 0x5A5AA5A5

#define PLUG_STATUS_UFLASH_ADDR 0
#define PLUG_STATUS_MAGIC_HEAD 0x6B6BB6B6

#define PLUG_CONFIG_USERBIN_ADDR 2048
#define PLUG_CONFIG_MAGIC_HEAD 0x7C7CC7C7

#define USER_BUFF_SIZE 1024

typedef struct _TASK_CONFIG
{
    uint8_t hour;   //小时
    uint8_t minute; //分钟
    uint8_t second; //秒钟
    uint8_t action; //动作
    uint8_t enable; //开关
    uint8_t repeat;
} TASK_CONFIG;

typedef struct _PLUG_CONFIG_ONE
{
    TASK_CONFIG task[PLUG_TIME_TASK_NUM];
} PLUG_CONFIG_ONE;

typedef struct _PLUG_CONFIG
{
    unsigned int magic_head;
    PLUG_CONFIG_ONE plug[PLUG_NUM];
    unsigned char enable_auto_restart;
    unsigned char enable_ap_mode;
    unsigned char enable_smtlink;
    unsigned char enable_child_lock; /* 童锁：1=忽略除"切换童锁"外的一切按键功能 */
    unsigned char key_map[KEY_MAP_NUM];  /* 按键映射，见 KEY_FUNC_xxx */
    unsigned char key_mask[KEY_MAP_NUM]; /* 单开(SOLO)作用的插座位掩码，bit0~bit5=口1~口6 */
    unsigned char crc;
} PLUG_CONFIG;

typedef struct _MQTT_CONFIG
{
    unsigned int magic_head;
    char hass_topic[MQTT_CLIENTID_MAX_LEN];
    char seraddr[MQTT_SERADDR_MAX_LEN + 1];
    unsigned short port;
    char clientid[MQTT_CLIENTID_MAX_LEN + 1];
    char username[MQTT_USERNAME_MAX_LEN + 1];
    char password[MQTT_PASSWORD_MAX_LEN + 1];
    unsigned char enable_sub;
    char sub_topic[MQTT_TOPIC_MAX_LEN + 1]; // for at+mqautosub topic
    char topic[MQTT_TOPIC_MAX_LEN + 1];     // for at+mqsubscribe topic
    unsigned char sub_qos;
    unsigned char mqtt_ver;
    char will_topic[MQTT_TOPIC_MAX_LEN + 1];
    char will_msg[MQTT_MESSAGE_MAX_LEN + 1];
    char pub_topic[MQTT_TOPIC_MAX_LEN + 1];
    unsigned char crc;
} MQTT_CONFIG;

typedef struct _PLUG_STATUS
{
    unsigned int magic_head;
    uint8_t plug[PLUG_NUM];
    uint8_t tag;
    unsigned char crc;
} PLUG_STATUS;

extern HF_CONFIG_FILE g_hf_config_file;

PLUG_STATUS plug_status;
MQTT_CONFIG user_mqtt_config;
PLUG_CONFIG user_plug_config;

/// extern char rtc_init;
char strMac[13];
char strIp[32];
uint32_t power;
char deviceid[32];
char ntpserver[50];
char version[10];

/* 本次开机的复位原因（hfsys_get_reset_reason() 的原值），
 * 由 app_main.c 的 show_reset_reason() 填入，/status 里可见。
 * 判据：想区分"看门狗/上电/软重启"，看这一个数就够了。 */
extern uint32_t reset_reason_code;

void user_config_init(void);
unsigned char crc_calc(unsigned char *, int);
void get_user_config_str(char *, uint8_t);
void get_user_config_simple_str(char *);
void save_user_config(void);

void init_plug_status(void);
void default_plug_status(PLUG_STATUS *);
void save_plug_status(PLUG_STATUS *);

void init_plug_config(void);
void default_plug_config(PLUG_CONFIG *);
void save_plug_config(PLUG_CONFIG *);

bool system_restart_flag;
bool system_soft_restart_flag;
bool system_start_ap_flag;
bool update_mqtt_config_flag;
bool update_plug_config_flag;
bool update_plug_status_flag[PLUG_NUM];

bool user_plug_config_enable;

uint8_t plug_status_loaded;
uint8_t plug_config_loaded;
uint8_t mqtt_config_loaded;
uint8_t mqtt_is_connected;



hfthread_mutex_t user_buff_lock;
char* user_buff;

// uint8_t press_flag;
// uint8_t release_flag;
// time_t last_press_time;
// time_t last_release_time;
#endif

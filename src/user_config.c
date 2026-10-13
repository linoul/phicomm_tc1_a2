
#include <string.h>
#include <stddef.h> /* offsetof */
#include <stdio.h>  /* sprintf：/status 里把复位原因打成十六进制 */

#include "user_config.h"
#include "cJSON/cJSON.h"
#include "user_mqtt.h"
#include "user_rtc.h"	/* get_time_string() */
#include "user_bl0937.h" /* en_load / en_save / en_err 自证计数 */

/* 本次开机的复位原因原值（app_main.c 的 show_reset_reason() 启动时填入） */
uint32_t reset_reason_code = 0;

unsigned char crc_calc(unsigned char *data, int len)
{
	unsigned int crc = 0;
	int i;
	for (i = 0; i < len; i++)
		crc += data[i];

	return crc;
}

void get_user_config_str(char *res, uint8_t x)
{
	cJSON *json_send = cJSON_CreateObject();
	cJSON_AddStringToObject(json_send, "mac", strMac);
	cJSON_AddStringToObject(json_send, "IP", strIp);
	char time_s[20] = {0};
	get_time_string(time_s, 20);
	cJSON_AddStringToObject(json_send, "time", time_s);
	cJSON_AddStringToObject(json_send, "author", "yakin.chang@gmail.com");

	uint8_t i, j;

	if (x >= PLUG_NUM)
	{
		i = 0;
	}
	else
	{
		i = x;
	}

	// for (i = 0; i < PLUG_NUM; i++)
	{
		cJSON *json_plug_send = cJSON_CreateObject();
		// cJSON_AddNumberToObject(json_plug_send, "status", plug_status.plug[i]);

		cJSON *json_tasks_send = cJSON_CreateArray();
		for (j = 0; j < PLUG_TIME_TASK_NUM; j++)
		// j = 0;
		{
			uint8_t repeat = user_plug_config.plug[i].task[j].repeat;
			char plug_task_config_str[100];
			sprintf(plug_task_config_str,
					"id: %d, time: %d:%d:%d, repeat:%d, days:%d,%d,%d,%d,%d,%d,%d, action:%d, enable:%d",
					j,
					user_plug_config.plug[i].task[j].hour, user_plug_config.plug[i].task[j].minute, user_plug_config.plug[i].task[j].second,
					repeat & 1,
					1 * (repeat & (1 << 1)) >> 1,
					2 * (repeat & (1 << 2)) >> 2,
					3 * (repeat & (1 << 3)) >> 3,
					4 * (repeat & (1 << 4)) >> 4,
					5 * (repeat & (1 << 5)) >> 5,
					6 * (repeat & (1 << 6)) >> 6,
					7 * (repeat & (1 << 7)) >> 7,
					user_plug_config.plug[i].task[j].action,
					user_plug_config.plug[i].task[j].enable);
			// sprintf(plug_task_config_str, "%s", "tttttt");
			cJSON_AddItemToArray(json_tasks_send, cJSON_CreateString(plug_task_config_str));
		}

		char strTemp1[] = "plug_X";
		strTemp1[5] = i + '0';
		cJSON_AddStringToObject(json_plug_send, "name", strTemp1);
		cJSON_AddItemToObject(json_plug_send, "tasks", json_tasks_send);
		cJSON_AddItemToObject(json_send, strTemp1, json_plug_send);
	}

	char *json_str = cJSON_Print(json_send);
	strcpy(res, json_str);

	hfmem_free(json_str);
	cJSON_Delete(json_send);
}

void get_user_config_simple_str(char *res)
{
	cJSON *json_send = cJSON_CreateObject();
	cJSON_AddStringToObject(json_send, "mac", strMac);
	cJSON_AddStringToObject(json_send, "IP", strIp);
	cJSON_AddStringToObject(json_send, "ssid", (char *)g_hf_config_file.sta_ssid);
	cJSON_AddStringToObject(json_send, "password", (char *)g_hf_config_file.sta_key);
	char time_s[20] = {0};
	get_time_string(time_s, 20);
	cJSON_AddStringToObject(json_send, "time", time_s);

	cJSON_AddNumberToObject(json_send, "enable_ap_mode", user_plug_config.enable_ap_mode);
	cJSON_AddNumberToObject(json_send, "enable_smtlink", user_plug_config.enable_smtlink);
	cJSON_AddNumberToObject(json_send, "enable_auto_restart", user_plug_config.enable_auto_restart);

	int i;
	char strTemp1[] = "plug_X";

	for (i = 0; i < PLUG_NUM; i++)
	{
		cJSON *json_plug_send = cJSON_CreateObject();
		cJSON_AddNumberToObject(json_plug_send, "status", plug_status.plug[i]);

		strTemp1[5] = i + '0';
		cJSON_AddItemToObject(json_send, strTemp1, json_plug_send);
	}

	cJSON *json_mqtt_send = cJSON_CreateObject();
	cJSON_AddStringToObject(json_mqtt_send, "name", user_mqtt_config.clientid);
	cJSON_AddStringToObject(json_mqtt_send, "ip", user_mqtt_config.seraddr);
	cJSON_AddStringToObject(json_mqtt_send, "sub_topic", user_mqtt_config.sub_topic);
	cJSON_AddStringToObject(json_mqtt_send, "user_name", user_mqtt_config.username);
	cJSON_AddStringToObject(json_mqtt_send, "password", user_mqtt_config.password);
	cJSON_AddNumberToObject(json_mqtt_send, "connected", mqtt_is_connected);

	cJSON_AddItemToObject(json_send, "mqtt", json_mqtt_send);
	cJSON_AddNumberToObject(json_send, "plug_config_loaded", plug_config_loaded);
	cJSON_AddNumberToObject(json_send, "plug_status_loaded", plug_status_loaded);
	cJSON_AddNumberToObject(json_send, "mqtt_config_loaded", mqtt_config_loaded);
	cJSON_AddStringToObject(json_send, "author", "yakin.chang@gmail.com");

	cJSON_AddStringToObject(json_send, "version", version);

	/* --- 诊断字段（10/11 加）------------------------------------------
	 * reset_reason：本次开机的复位原因原值，十六进制。
	 *   例：8 = IRESET0，...（bit 含义见 SDK hfsys.h 的 HFSYS_RESET_REASON_*）
	 *   用途：以后再出现"莫名重启"，先看这一个数就能区分
	 *   上电复位 / 看门狗 / 软重启 / SmartLink 复位。
	 * ntp_call/ntp_fail：NTP 同步调用次数与失败次数。
	 *   用途：10 小时一次的重同步（do_update_rtc_time）如果再次把设备
	 *   打复位，这两个数会在重启前后对不上，一眼可见。 */
	char rr[12] = {0};
	sprintf(rr, "%08x", (unsigned int)reset_reason_code);
	cJSON_AddStringToObject(json_send, "reset_reason", rr);
	cJSON_AddNumberToObject(json_send, "ntp_call", (double)ntp_call_cnt);
	cJSON_AddNumberToObject(json_send, "ntp_fail", (double)ntp_fail_cnt);

	/* --- 电量持久化自证（10/11 加）---------------------------------------
	 * 一直以来"电量不跨重启累计"到底是没写、还是写了没读回来，光看代码
	 * 分不清。这三个数把链路两端都摆到明面上：
	 *   en_load : 1 = 本次开机 flash 里有有效电量记录（总计是续上的）
	 *   en_save : 成功写入 flash 的次数
	 *   en_err  : 写入失败的次数
	 * 正常设备开机后几秒内 en_save 就应 >0（基线落盘）；跑满 1 小时再加 1。
	 * 判据：重启后 en_load=0 → 没读回来；en_save 恒 0 且 en_err 恒 0 →
	 *       落盘线程没跑（标志无人消费）；en_err 增长 → 写 flash 真的失败。 */
	cJSON_AddNumberToObject(json_send, "en_load", bl0937_energy_load_ok);
	cJSON_AddNumberToObject(json_send, "en_save", (double)bl0937_energy_save_ok);
	cJSON_AddNumberToObject(json_send, "en_err", (double)bl0937_energy_save_err);

	// extern time_t timestamp_start;
	// extern time_t utc8ts;
	// extern time_t ntp_res;
	// extern time_t systime_now;
	// cJSON_AddNumberToObject(json_send, "ntp_res", ntp_res);
	// cJSON_AddNumberToObject(json_send, "sysnow", systime_now);
	// cJSON_AddNumberToObject(json_send, "start_time", timestamp_start);
	// cJSON_AddNumberToObject(json_send, "now_ts", utc8ts);

	char *json_str = cJSON_Print(json_send);
	strcpy(res, json_str);

	hfmem_free(json_str);
	cJSON_Delete(json_send);
}

void default_plug_config(PLUG_CONFIG *config)
{
	memset((char *)config, 0, sizeof(PLUG_CONFIG));
	config->enable_ap_mode = true;
	config->enable_smtlink = true;
	/* 默认关闭自动重启：WiFi 信号差的安装位会陷入"断 10 分钟就重启"的循环，
	 * 且重启会把未落盘的电量清掉。需要看门狗行为的设备用配置命令单独打开：
	 *   <hass>/switch/<clientid>/config/cmd  <-  {"setting":{"enable_auto_restart":1}} */
	config->enable_auto_restart = 0;
	config->enable_child_lock = 0;
	/* 按键默认映射：单击=切换全部（原行为）；双击=切换童锁（物理解锁出口）；
	 * 长按三档对应旧固件的 1~5s 软重启 / 5~10s AP / >10s SmartLink。 */
	config->key_map[0] = KEY_FUNC_ALL;
	config->key_map[1] = KEY_FUNC_CHILD_LOCK;
	config->key_map[2] = KEY_FUNC_REBOOT_SOFT;
	config->key_map[3] = KEY_FUNC_AP;
	config->key_map[4] = KEY_FUNC_SMTLK;
	for (uint8_t i = 0; i < PLUG_NUM; i++)
	{
		for (uint8_t j = 0; j < PLUG_TIME_TASK_NUM; j++)
		{
			config->plug[i].task[j].enable = 0;
			config->plug[i].task[j].enable = 0;
			config->plug[i].task[j].hour = 24;
			config->plug[i].task[j].minute = 60;
			config->plug[i].task[j].second = 60;
			config->plug[i].task[j].repeat = 0b11111111;
		}
	}
}

void save_plug_config(PLUG_CONFIG *config)
{
	config->magic_head = PLUG_CONFIG_MAGIC_HEAD;
	/* ★ CRC 必须用 offsetof 到 crc 为止，不能用 sizeof-1：
	 * 结构体是 4 字节对齐的，sizeof-1 会把 crc 自己算进去 → 要求累加和
	 * ≡0(mod 256)，永远校验失败、每次开机重写 flash（10/2 版踩过的坑）。
	 * offsetof 写法对以后再加字段也免疫（key_mask 又加了 5 字节也不用改）。 */
	config->crc = crc_calc((unsigned char *)config, offsetof(PLUG_CONFIG, crc));
	hffile_userbin_write(PLUG_CONFIG_USERBIN_ADDR, (char *)config, sizeof(PLUG_CONFIG));
}

void init_plug_config(void)
{
	unsigned char crc;
	memset((char *)&user_plug_config, 0, sizeof(PLUG_CONFIG));
	hffile_userbin_read(PLUG_CONFIG_USERBIN_ADDR, (char *)&user_plug_config, sizeof(PLUG_CONFIG));
	crc = crc_calc((unsigned char *)&user_plug_config, offsetof(PLUG_CONFIG, crc));
	if (!(PLUG_CONFIG_MAGIC_HEAD == user_plug_config.magic_head && crc == user_plug_config.crc))
	{
		/* 不做老格式迁移（结构变了就直接回默认，旧固件配的任务/设置会丢） */
		default_plug_config(&user_plug_config);
		save_plug_config(&user_plug_config);
		plug_config_loaded = 2;
	}
	else
	{
		plug_config_loaded = 1;
	}
}

void default_plug_status(PLUG_STATUS *config)
{
	memset((char *)config, 0, sizeof(PLUG_STATUS));
	for (uint8_t i = 0; i < PLUG_NUM; i++)
	{
		config->plug[i] = 1;
	}
}

void save_plug_status(PLUG_STATUS *config)
{
	config->magic_head = PLUG_STATUS_MAGIC_HEAD;
	config->crc = crc_calc((unsigned char *)config, sizeof(PLUG_STATUS) - 1);
	hfuflash_erase_page(PLUG_STATUS_UFLASH_ADDR, 1);
	hfuflash_write(PLUG_STATUS_UFLASH_ADDR, (char *)config, sizeof(PLUG_STATUS));
}

void init_plug_status(void)
{
	unsigned char crc;
	memset((char *)&plug_status, 0, sizeof(PLUG_STATUS));
	hfuflash_read(PLUG_STATUS_UFLASH_ADDR, (char *)&plug_status, sizeof(PLUG_STATUS));
	crc = crc_calc((unsigned char *)&plug_status, sizeof(PLUG_STATUS) - 1);
	if (!(PLUG_STATUS_MAGIC_HEAD == plug_status.magic_head && crc == plug_status.crc))
	{
		default_plug_status(&plug_status);
		save_plug_status(&plug_status);
		plug_status_loaded = 2;
	}
	else
	{
		plug_status_loaded = 1;
	}
}

void user_config_init()
{
	uint8_t i = 0;
	system_restart_flag = false;
	system_soft_restart_flag = false;
	system_start_ap_flag = false;
	user_plug_config_enable = false;
	strcpy(ntpserver, "ntp1.aliyun.com");
	update_mqtt_config_flag = false;
	update_plug_config_flag = false;
	for (i = 0; i < PLUG_NUM; i++)
	{
		update_plug_status_flag[i] = false;
	}

	plug_config_loaded = 0;
	plug_status_loaded = 0;
	mqtt_config_loaded = 0;
	mqtt_is_connected = 0;

	sprintf(version, VERSION);

	user_buff = (char *)hfmem_malloc(USER_BUFF_SIZE);
	memset((char *)user_buff, 0, USER_BUFF_SIZE);
	user_buff_lock = NULL_MUTEX;
	// hfthread_mutext_new(&user_buff_lock);

	// press_flag = 0;
	// release_flag = 0;
	// last_press_time = 0;
	// last_release_time = 0;

	init_plug_config();
	init_plug_status();
	// default_plug_config(&user_plug_config);
}
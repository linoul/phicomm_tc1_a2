#ifndef _WEBUI_H_
#define _WEBUI_H_

#include <hsf.h>

/*
 * 这个符号在 sdk/4.14/libs/libLPBX3Kernel.a 里有实现，但 SDK 头文件里没有原型。
 * 原仓库能编过是因为在 Keil 下隐式声明只是警告；GCC 带
 * -Werror=implicit-function-declaration 时是硬错误，所以这里补上声明。
 */
extern int hfhttpd_url_callback_register(hfhttpd_url_callback_t cb, int flag);

typedef struct _config_data
{
	int first_start_flag;
	int heartbeat_rate;
	int scan_frequence;
	int reset_frequence;
} web_info_data_t;

int USER_FUNC httpd_add_page(const char *url, void (*callback)(char *url, char *rsp));
/* 带 Content-Type 的注册；ctype 传 NULL 时默认 text/plain */
int USER_FUNC httpd_add_page_ex(const char *url, const char *ctype, void (*callback)(char *url, char *rsp));
int USER_FUNC httpd_arg_find(char *url,char *slot0,char *slot1,char *slot2,char *slot3,char *slot4,char *slot5);

int hfhttpd_yz_nvget(char *cfg_name,int name_len, char *value, int valu_len);
int hfhttpd_yz_nvset(char *cfg_name,int name_len, char *value, int value_len);

void USER_FUNC httpd_init(void);

#endif

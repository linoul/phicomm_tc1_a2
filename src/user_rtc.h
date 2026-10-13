
#ifndef __USER_RTC_H_
#define __USER_RTC_H_

#include "user_config.h"
#include "cJSON/cJSON.h"

#define UTC8 28800
void USER_FUNC rtctime_init(void);
void USER_FUNC key_timeout_handler( void* arg );
	
USER_FUNC void rtc_thread_func(void * arg);
USER_FUNC void ntp_thread_func(void * arg);
int printtime();

/* NTP 诊断计数（/status 暴露）。定义在 user_rtc.c。 */
extern volatile uint32_t ntp_call_cnt;
extern volatile uint32_t ntp_fail_cnt;

void update_rtctime();
void update_time();
void get_time_string(char*,int);
void get_user_config_info(cJSON*);


#endif

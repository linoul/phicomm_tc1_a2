/**
 * \file
 *
 * \brief Empty user application template
 *
 */

/**
 * \mainpage User Application template doxygen documentation
 *
 * \par Empty user application template
 *
 * Bare minimum empty user application template
 *
 * \par Content
 *
 * -# Include the ASF header files (through asf.h)
 * -# Minimal main function that starts with a call to board_init()
 * -# "Insert application code here" comment
 *-pipe -fno-strict-aliasing -Wall -Wstrict-prototypes -Wmissing-prototypes -Werror-implicit-function-declaration -Wpointer-arith -std=gnu99 -ffunction-sections -fdata-sections -Wchar-subscripts -Wcomment -Wformat=2 -Wimplicit-int -Wmain -Wparentheses -Wsequence-point -Wreturn-type -Wswitch -Wtrigraphs -Wunused -Wuninitialized -Wunknown-pragmas -Wfloat-equal -Wundef -Wshadow -Wbad-function-cast -Wwrite-strings -Wsign-compare -Waggregate-return  -Wmissing-declarations -Wformat -Wmissing-format-attribute -Wno-deprecated-declarations -Wpacked -Wredundant-decls -Wnested-externs -Wlong-long -Wunreachable-code -Wcast-align --param max-inline-insns-single=500
 */

/*
 * Include header files for all drivers that have been imported from
 * Atmel Software Framework (ASF).
 */
#include <hsf.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "user_gpio.h"
#include "app_main.h"
#include "user_rtc.h"
#include "user_mqtt.h"
#include "user_function.h"
#include "user_httpd.h"
#include "user_bl0937.h"

/*
 * HomeKit 默认**不启用**：整套密码学 + 协议约 35KB，会吃掉大半应用分区余量
 * （应用分区 588K，现有固件已 544K，余量约 56KB）。
 * 要启用就给工程加 -DHK_ENABLE（见 src/Makefile 的 HK_FLAGS）。
 * 启用后 homekit 目录下的 .o 才会被链接器提取
 *（它们打在 userapps.a 里，不引用就不会进固件）。
 */
#ifdef HK_ENABLE
#include "homekit/hk_net.h"
#endif


EXTERNC const int hf_gpio_fid_to_pid_map_table[HFM_MAX_FUNC_CODE];



#ifdef __LPT230__
int g_module_id= HFM_TYPE_LPT230;

const int hf_gpio_fid_to_pid_map_table[HFM_MAX_FUNC_CODE]=
{
    HFM_NOPIN,        //HFGPIO_F_JTAG_TCK
    HFM_NOPIN,        //HFGPIO_F_JTAG_TDO
    HFM_NOPIN,        //HFGPIO_F_JTAG_TDI
    HFM_NOPIN,        //HFGPIO_F_JTAG_TMS
    HFM_NOPIN,        //HFGPIO_F_USBDP
    HFM_NOPIN,        //HFGPIO_F_USBDM
    LPx30_GPIO2,    //HFGPIO_F_UART0_TX
    HFM_NOPIN,    //HFGPIO_F_UART0_RTS
    LPx30_GPIO1,    //HFGPIO_F_UART0_RX
    HFM_NOPIN,    //HFGPIO_F_UART0_CTS
    
    HFM_NOPIN,      //HFGPIO_F_SPI_MISO
    HFM_NOPIN,          //HFGPIO_F_SPI_CLK
    HFM_NOPIN,          //HFGPIO_F_SPI_CS
    HFM_NOPIN,      //HFGPIO_F_SPI_MOSI
    
    HFM_NOPIN,        //HFGPIO_F_UART1_TX,
    HFM_NOPIN,        //HFGPIO_F_UART1_RTS,
    HFM_NOPIN,        //HFGPIO_F_UART1_RX,
    HFM_NOPIN,        //HFGPIO_F_UART1_CTS,
    
    HFM_NOPIN,    //HFGPIO_F_NLINK
    HFM_NOPIN,    //HFGPIO_F_NREADY
    HFM_NOPIN,    //HFGPIO_F_NRELOAD
    HFM_NOPIN,        //HFGPIO_F_SLEEP_RQ
    HFM_NOPIN,        //HFGPIO_F_SLEEP_ON
    
    HFM_NOPIN,        //HFGPIO_F_WPS
    HFM_NOPIN,        //HFGPIO_F_RESERVE1
    HFM_NOPIN,        //HFGPIO_F_RESERVE2
    HFM_NOPIN,        //HFGPIO_F_RESERVE3
    HFM_NOPIN,        //HFGPIO_F_RESERVE4
    HFM_NOPIN,        //HFGPIO_F_RESERVE5
    
    LPx30_GPIO22,           //HFGPIO_F_USER_DEFINE
    LPx30_GPIO24,     //HFGPIO_F_USER_DEFINE+1   .....
    LPx30_GPIO8,
    LPx30_GPIO23,
    LPx30_GPADC0,
    LPx30_GPIO3,
    LPx30_GPIO5,
    LPx30_GPIO25,
    LPx30_GPIO7,        //HFGPIO_F_USER_DEFINE+8   BL0937 CF   插板功能"PW功率"(= 原 POWER 宏)
    LPx30_GPIO4,        //HFGPIO_F_USER_DEFINE+9   BL0937 CF1  插板功能"电流电压"
    LPx30_GPIO12,       //HFGPIO_F_USER_DEFINE+10  BL0937 SEL  插板功能"切换电流电压"
};



#else
#error "invalid project !you must define module type(__LPT230__/__LPT130__/__LPB130__/__LPT330__/__LPB135/__LPT130B)"
#endif




// char strMac[13] = { 0 };
// char strIp[32];
// char deviceid[32];
// uint32_t power;
//For type written and read
static int dev_param_read(char *wechat_type)
{
    unsigned char type[21];
    hffile_userbin_read(0, (char*)type, sizeof(type));

    unsigned char crc = 0;
    if(type[0] == 0x5A)
    {
        int i;
        char *p = (char *)(type+2);
        if(strlen(p) > 18)
            return -1;
        
        for(i=0; i<strlen(p); i++)
            crc += (unsigned char)*(p+i);

        if(type[1] == crc)
        {
            strcpy(wechat_type, p);
            return HF_SUCCESS;
        }
    }

    return -1;
}
    
static int dev_param_write(char *wechat_type)
{
    char type[21];
    unsigned char crc = 0;

    int i;
    for(i=0; i<strlen(wechat_type); i++)
        crc += (unsigned char)wechat_type[i];

    type[0] = 0x5A;
    type[1] = crc;
    strcpy(type+2, wechat_type);
    
    hffile_userbin_write(0, (char*)type, sizeof(type));
    return HF_SUCCESS;    
}

static int hf_cmd_get_version(pat_session_t s,int argc,char *argv[],char *rsp,int len)
{
    if(argc==0)
    { 
        sprintf(rsp,"=%s(%s %s)","Airkiss 2",__DATE__,__TIME__);

        return 0;
    }
    return -3;
}

static int hf_atcmd_set_type(pat_session_t s,int argc,char *argv[],char *rsp,int len)
{
    if(argc == 0)
    {
        char type[21];
        if(dev_param_read(type) == HF_SUCCESS)
            sprintf(rsp, "=%s", type);
        else
            sprintf(rsp, "=%s", "");
        return 0;
    }
    else if(argc == 1)
    {
        if(strlen(argv[0]) < 18)
        {
            if(dev_param_write(argv[0]) == HF_SUCCESS)
                return 0;
            else
                return -5;
        }
        return -4;
    }
    return -3;
}

const hfat_cmd_t user_define_at_cmds_table[]=
{
    // {"MQCLIENTID",hf_atcmd_mqclientid,"   AT+MQCLIENTID: MQTT ClientID.\r\n",NULL},
	// {"MQIPPORT",hf_atcmd_mqipport,"   AT+MQIPPORT: MQTT server address and port.\r\n",NULL},
	// {"MQUSERPWD",hf_atcmd_mquserpwd,"   AT+MQUSERPWD: MQTT username and password.\r\n",NULL},
	// {"MQSTATUS",hf_atcmd_mqstatus,"   AT+MQSTATUS: MQTT status.\r\n",NULL},
	// {"MQSTART",hf_atcmd_mqstart,"   AT+MQSTART: start MQTT.\r\n",NULL},
	// {"MQPUBLISH",hf_atcmd_mqpublish,"   AT+MQPUBLISH: MQTT publish data.\r\n",NULL},
	// {"MQSUBSCRIBE",hf_atcmd_mqsubscribe,"   AT+MQSUBSCRIBE: MQTT subscribe topic.\r\n",NULL},
	// {"MQUNSUBSCRIBE",hf_atcmd_mqunsubscribe,"   AT+MQUNSUBSCRIBE: MQTT unsubscribe topic.\r\n",NULL},
	// {"MQAUTOSUB",hf_atcmd_mqautosub,"   AT+MQAUTOSUB: MQTT auto subscribe topic.\r\n",NULL},
	// {"MQRES",hf_atcmd_mqres,"   AT+MQRES: MQTT reload parameter.\r\n",NULL},
	// {"MQVER",hf_atcmd_mqver,"   AT+MQVER: MQTT version.\r\n",NULL},
	// {"MQWILL",hf_atcmd_mqwill,"   AT+MQWILL: MQTT will topic and message.\r\n",NULL},
    // {"TYPE",hf_atcmd_set_type,"   AT+TYPE: airkiss device type.\r\n",NULL},    //Modify TYPE
    // {"APPVER",hf_cmd_get_version,"   AT+APPVER: get version\r\n", NULL},
    {NULL,NULL,NULL,NULL}
};

static int sys_event_callback( uint32_t event_id,void * param)
{
    switch(event_id)
    {
        case HFE_WIFI_STA_CONNECTED:
            u_printf("wifi sta connected!!\n");
            break;
        case HFE_WIFI_STA_DISCONNECTED:
            u_printf("wifi sta disconnected!!\n");
            break;
        case HFE_CONFIG_RELOAD:
            u_printf("reload!\n");
            break;
        case HFE_DHCP_OK:
            {
                uint32_t *p_ip;
                p_ip = (uint32_t*)param;
                u_printf("dhcp ok %s!\n", inet_ntoa(*p_ip));
                strcpy(strIp,inet_ntoa(*p_ip));
            }
            break;
        case HFE_SMTLK_OK:
            {
                u_printf("smtlk ok!\n");
                char *pwd=(char*)param;
                if(pwd == NULL)
                    u_printf("key is null!\n");
                else
                {
                    int i;
                    for(i=0; i<(int)strlen(pwd); i++)
                    {
                        if(pwd[i]==0x1b)
                            break;
                    }
                    for(i++; i<(int)strlen(pwd); i++)
                        HF_Debug(DEBUG_ERROR,"user info 0x%02x-[%c]\n", pwd[i], pwd[i]);
                    
                }
                msleep(100);
                return 1;
            }
            break;

        case HFE_WPS_OK:   
            {
                u_printf("wps ok, key: %s!\n", (char*)param);
                //return 1;
            }
            break;
    }
    return 0;
}
/*
static int USER_FUNC socketa_recv_callback(uint32_t event,char *data,uint32_t len,uint32_t buf_len)
{
    if(event==HFNET_SOCKETA_DATA_READY)

        HF_Debug(DEBUG_LEVEL_LOW,"socketa recv %d bytes %d\n",len,buf_len);
    else if(event==HFNET_SOCKETA_CONNECTED)
        u_printf("socket a connected!\n");
    else if(event==HFNET_SOCKETA_DISCONNECTED)
        u_printf("socket a disconnected!\n");
    
    return len;
}

static int USER_FUNC socketb_recv_callback(uint32_t event,char *data,uint32_t len,uint32_t buf_len)
{
    if(event==HFNET_SOCKETB_DATA_READY)
        HF_Debug(DEBUG_LEVEL_LOW,"socketb recv %d bytes %d\n",len,buf_len);
    else if(event==HFNET_SOCKETB_CONNECTED){
        hfgpio_fset_out_high(LED);
        u_printf("socket b connected!\n");
    }
        
    else if(event==HFNET_SOCKETB_DISCONNECTED){
        u_printf("socket b disconnected!\n");
        hfgpio_fset_out_low(LED);
    }
            
    return len;
}*/

static int USER_FUNC uart_recv_callback(uint32_t event,char *data,uint32_t len,uint32_t buf_len)
{
	HF_Debug(DEBUG_LEVEL_LOW,"[%d]uart recv %d bytes data %d\n",event,len,buf_len);
	if(hfsys_get_run_mode() == HFSYS_STATE_RUN_CMD)
		return len;
	
	return len;
}

static void show_reset_reason(void)
{
    uint32_t reset_reason=0;
    reset_reason = hfsys_get_reset_reason();
    reset_reason_code = reset_reason;   /* 暴露到 /status，见 user_config.h */
    
#if 1
    u_printf("reset_reason:%08x\n",reset_reason);
#else    
    if(reset_reason&HFSYS_RESET_REASON_ERESET)
    {
        u_printf("ERESET\n");
    }
    if(reset_reason&HFSYS_RESET_REASON_IRESET0)
    {
        u_printf("IRESET0\n");
    }
    if(reset_reason&HFSYS_RESET_REASON_IRESET1)
    {
        u_printf("IRESET1\n");
    }
    if(reset_reason==HFSYS_RESET_REASON_NORMAL)
    {
        u_printf("RESET NORMAL\n");
    }
    if(reset_reason&HFSYS_RESET_REASON_WPS)
    {
        u_printf("RESET FOR WPS\n");
    }
    if(reset_reason&HFSYS_RESET_REASON_SMARTLINK_START)
    {
        u_printf("RESET FOR SMARTLINK START\n");
    }
    if(reset_reason&HFSYS_RESET_REASON_SMARTLINK_OK)
    {
        u_printf("RESET FOR SMARTLINK OK\n");
    }
    if(reset_reason&HFSYS_RESET_REASON_WPS_OK)
    {
        u_printf("RESET FOR WPS OK\n");
    }
#endif
    
    return;
}

///关闭WPS,减少35k固件大小
int hfwifi_wps_main(void){return 0;}

void hfnet_register_socketa_atcmd(void){}
void hfnet_register_socketb_atcmd(void){}

void app_init(void)
{
    u_printf("app_init\n");    
}

char rsp[64]={0};
char *mac[2]={0};

/*--------------------------------------------------------------------------
 * 开机进度标记（排错用）
 *
 * 排插没有屏幕也没有调试口，串口是唯一的观测手段。下面每个 BOOT_MSG 对应
 * app_main 里一个初始化阶段：串口最后停在哪个编号，问题就出在它和下一个
 * 编号之间。常见现象对照：
 *
 *   一条 [BOOT] 都看不到              -> App 根本没跑起来，先去看串口里
 *                                        BootLoader 的报错（多半是烧录问题）
 *   停在 B2 且反复刷 gpio map file error -> 映射表里有引脚被重复占用
 *   停在 B6 / B7                     -> 读配置或启动 httpd 卡住
 *   停在 BC                          -> BL0937 初始化卡住
 *   走到 B10 之后 LED 不灭、继电器不响  -> App 其实正常，查继电器/按键硬件
 *
 * 正式版想关掉这些输出，把 BOOT_TRACE 改成 0 即可。
 *------------------------------------------------------------------------*/
#define BOOT_TRACE 1
#if BOOT_TRACE
#define BOOT_MSG(s) u_printf("[BOOT] " s "\n")
#else
#define BOOT_MSG(s) do { } while (0)
#endif

int USER_FUNC app_main (void)
{
    BOOT_MSG("B1 enter");
    hfdbg_set_level(3);
    HF_Debug(DEBUG_LEVEL,"sdk version(%s),the app_main start time is %s %s\n",hfsys_get_sdk_version(),__DATE__,__TIME__);
    BOOT_MSG("B2 gpio map check...");
    if(hfgpio_fmap_check(g_module_id)!=0)
    {
        while(1)
        {
            HF_Debug(DEBUG_ERROR,"gpio map file error\n");
            msleep(1000);
        }
    }
    BOOT_MSG("B2 ok");

    hfsys_register_system_event(sys_event_callback);
    show_reset_reason();
    BOOT_MSG("B3 event registered");

    while(hfsmtlk_is_start())
    {
        msleep(500);
    }
    BOOT_MSG("B4 smartlink idle");

	if(hfnet_start_uart(HFTHREAD_PRIORITIES_LOW,(hfnet_callback_t)uart_recv_callback)!=HF_SUCCESS)
	{
		HF_Debug(DEBUG_WARN,"start uart fail!\n");
	}
    BOOT_MSG("B5 uart started");

    user_config_init();
    BOOT_MSG("B6 config loaded");

    /*
     * 救命逻辑（参考 A1 固件）：上电时按住按键 → 无视童锁/一切配置，
     * 直接进 AP 配网。防止"童锁开着 + WiFi 配置损坏"的组合让设备彻底失联
     * （那时按键事件全被童锁拦截，只剩这条开机通道）。
     * 按住要持续 ~300ms 才算，插拔瞬间的毛刺不触发。
     * system_start_ap_flag 由 rtc 定时器消费：AT+WMODE=AP + 软重启。
     */
    if (key_status() == KEY_DOWN)
    {
        msleep(200);
        if (key_status() == KEY_DOWN)
        {
            system_start_ap_flag = true;
            BOOT_MSG("B6a boot key held -> ap mode");
        }
    }
        //Web Server
    if(hfnet_start_httpd(HFTHREAD_PRIORITIES_MID)!=HF_SUCCESS)
    {
        HF_Debug(DEBUG_WARN,"start httpd fail\n");
    }
    BOOT_MSG("B7 httpd service started");


    hfgpio_fset_out_high(LED);
    BOOT_MSG("B8 led on");

    key_init();
    BOOT_MSG("B9 key init done");

    rtctime_init();
    BOOT_MSG("BA rtc init done");
    // user_udp_init( );

    httpd_init();
    BOOT_MSG("BB web pages registered");

    /* BL0937 计量：必须在 rtctime_init() 之后（要读系统时间做跨天统计） */
    bl0937_init();
    BOOT_MSG("BC bl0937 init done");

    hfnet_get_mac_address(strMac);
    BOOT_MSG("BD mac read");

    hfgpio_fset_out_low(LED);
    load_key_status();
    BOOT_MSG("BE relays restored");
    while(!hfnet_wifi_is_active())
    {
        msleep(50);
    }
    BOOT_MSG("BF wifi active");

    /*
     * HomeKit 必须放在 hfnet_wifi_is_active() **之后**：mdns.h 明确要求
     * mdns_start() 只能在 network stack 起来之后调用。
     * 失败不致命 —— 排插的 Web 与 MQTT 功能不受影响，所以只告警。
     */
#ifdef HK_ENABLE
    if (hk_net_start("TC1") != 0)
    {
        HF_Debug(DEBUG_WARN, "start homekit fail\n");
    }
    else
    {
        BOOT_MSG("C0 homekit started");
    }
#endif

    mqtt_para_init();
    BOOT_MSG("B10 mqtt ready, app_main done");
    // user_mdns_init( );

    return 1;
}


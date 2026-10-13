/*
 * TC1 A2 后台管理页面
 *
 * ⚠ 设计约束（很重要，改这个文件前先读）
 * HF-LPX30 内置 httpd 的 url 回调只拿到一块固定长度的响应缓冲，SDK 头文件里
 * 没有公开它的大小，工程里能看到的证据只有 /task 那个约 580 字节的 JSON。
 * 为了不去赌缓冲大小，这里把页面切成若干个「小分片」，并遵守两条硬规则：
 *
 *	 1) 每个 URL 的响应内容都控制在 900 字节以内（含 85 字节响应头）；
 *	 2) 前端脚本保持极简——状态页/控制页/校准页的 HTML 由后端直接生成并带上
 *		数据，前端只负责把整块 innerHTML 换掉，不做逐字段渲染。
 *
 * URL 一览：
 *	 /admin  页面骨架
 *	 /a.css  基础样式（布局/导航/表格）
 *	 /b.css  控件样式（按钮/输入框/提示条）
 *	 /t.js   前端逻辑（框架 + 状态页刷新 + 控制）
 *	 /c.js   校准页逻辑，切到校准页时才按需加载
 *	 /u.js   设置页逻辑，切到设置页时才按需加载
 *	 /k.js   按键页逻辑，切到按键页时才按需加载
 *	 /k2.js  任务页行结构生成，末尾链式加载 /k3.js
 *	 /k3.js  任务页读写逻辑（插口下拉/读任务/存任务）
 *	 /t0     状态页（含实时数据）
 *	 /t1     控制页（含开关状态）
 *	 /t2     校准页（实时脉冲频率 + SEL 档位 + 一键标定）
 *	 /t3     设置页表单
 *	 /t4     按键页（童锁 + 连击/长按功能映射）
 *	 /t5     任务页（每插口 5 条定时任务）
 *	 /t6     关于页
 *	 /api    动作与 JSON：act=st|cg|cc|set|cal|cfg|adj|lock|clr|rst
 *	                    |kq|ks|kt（按键/任务页）
 *
 * ★ tab 号 = 菜单按钮在 #n 里的 DOM 序号（0~6），go(n) 固定去拉 /t{n}，
 *   所以页面 URL 必须和按钮顺序一一对应 —— 错一位就是"点 A 高亮 B"的老 bug。
 *
 * 另外 newlib-nano 的 printf 默认不带浮点，所有浮点输出统一走
 * bl0937_fmt_float() 转字符串，JSON 里的浮点字段也用字符串承载。
 */

#include <hsf.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_main.h"
#include "user_web.h"
#include "user_httpd.h"
#include "user_config.h"
#include "user_gpio.h"
#include "user_bl0937.h"
#include "user_function.h"
#include "user_mqtt.h"
#include "user_rtc.h"

/* 纯 C99 头、不依赖 SDK，所以无条件包含：HK 关着时页面也要回 hk 字段 */
#include "homekit/hk_code.h" /* HK_CODE_BUF */

#ifdef HK_ENABLE
#include "homekit/hk_net.h" /* hk_net_get_code / hk_net_set_code */
#endif

/*------------------------------------------------------------- 页面骨架 */

static const char web_index_html[] =
	"<!DOCTYPE html><meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>TC1 智能排插</title>"
	"<link rel=stylesheet href=/a.css><link rel=stylesheet href=/b.css>"
	"<h1>斐讯 TC1 插排</h1>"
	"<div id=n>"
	"<button class=on onclick=\"tab(0)\">状态</button>"
	"<button onclick=\"tab(1)\">控制</button>"
	"<button onclick=\"tab(2)\">校准</button>"
	"<button onclick=\"tab(3)\">设置</button>"
	"<button onclick=\"tab(4)\">按键</button>"
	"<button onclick=\"tab(5)\">任务</button>"
	"<button onclick=\"tab(6)\">关于</button>"
	"</div><div id=m></div><div id=p>加载中...</div>"
	"<script src=/t.js?v=3></script>";

static void USER_FUNC web_index_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_index_html);
}

/*------------------------------------------------------------- 样式表 */

/* 基础：布局 / 标题 / 导航 / 表格 */
static const char web_css_a[] =
	"*{box-sizing:border-box}"
	"body{font:16px/1.6 sans-serif;color:#333;max-width:640px;"
	"margin:0 auto;padding:10px 12px 40px}"
	"h1{font-size:20px;text-align:center;margin:12px 0 16px}"
	"#n{display:flex;border-bottom:1px solid #ddd}"
	"#n button{flex:1;padding:10px 0;border:0;background:#eee;font-size:15px;"
	"font-family:inherit;cursor:pointer;color:#333}"
	"#n button.on{background:#fff;color:#07d;font-weight:700;"
	"box-shadow:inset 0 -2px 0 #07d}"
	"table{width:100%;border-collapse:collapse;margin:14px 0}"
	"th,td{padding:9px;border:1px solid #ddd}"
	"th{background:#f5f5f5;text-align:left;font-size:15px}"
	"td{background:#fff}"
	"td:first-child{width:38%;color:#666}"
	".ct{text-align:center}";

/* 控件：按钮 / 输入框 / 提示 */
static const char web_css_b[] =
	"button.b{border:0;border-radius:3px;color:#fff;padding:9px 15px;font-size:15px;"
	"font-family:inherit;cursor:pointer;margin:3px 2px;min-width:56px}"
	"button.b:active{opacity:.75}"
	".ok{background:#5cb85c}.no{background:#9aa5b1}"
	".i{background:#5bc0de}.d{background:#d9534f}"
	"input{width:100%;padding:8px;border:1px solid #ccc;border-radius:3px;"
	"font-size:15px}"
	"input.s{width:46%;display:inline-block;vertical-align:middle}"
	/* 任务页/按键页多选：复选框自适应宽度（默认 100% 会撑满整行） */
	"input[type=checkbox]{width:auto;vertical-align:-2px;margin:0 2px 0 8px}"
	"label{white-space:nowrap}"
	"#tt table{margin:8px 0}"
	"div[id^=km]{font-size:13px;color:#888;margin-top:4px}"
	"#m{display:none;padding:10px;margin:12px 0;border-radius:3px;text-align:center}"
	"#m.on{display:block;background:#dff0d8;color:#3c763d}"
	"#m.on.er{background:#f2dede;color:#a94442}"
	".tip{font-size:13px;color:#888;margin-top:16px;line-height:1.5}";

static void USER_FUNC web_css_a_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_css_a);
}

static void USER_FUNC web_css_b_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_css_b);
}

/*------------------------------------------------------------- 前端脚本 */

/* 框架 + 状态页刷新 + 控制。刻意保持精简，注意控制总长度 */
static const char web_js[] =
	"function $(i){return document.getElementById(i)}"
	"function ajax(u,c,d){var x=new XMLHttpRequest();"
	"x.onload=function(){d&&d(x.responseText)};"
	"x.open(c?'POST':'GET',u);x.send(c||null)}"
	"function msg(t,e){var m=$('m');m.innerHTML=t;m.className=e?'on er':'on';"
	"setTimeout(\"$('m').className=''\",3e3)}"
	"var tm=0;"
	"function tab(n){var b=$('n').children,i=b.length;"
	"while(i--)b[i].className=i==n?'on':'';go(n)}"
	"function ld(f){var s=document.createElement('script');s.src='/'+f+'?_='+Math.random();document.head.appendChild(s)}"
	"function go(n){if(tm)clearTimeout(tm);tm=0;"
	"ajax('/t'+n,0,function(h){$('p').innerHTML=h;"
	"if(!n)tm=setTimeout(\"go(0)\",5e3);if(n==2)ld('c.js');if(n==3){ld('u.js');ld('h.js')};"
	"if(n==4)ld('k.js');if(n==5)ld('k2.js')})}"
	"function setplug(c,v){ajax('/api?act=set&c='+c+'&v='+v,0,function(t){"
	"if(t[6]=='1'){msg('操作成功');setTimeout(\"go(1)\",300)}"
	"else msg('操作失败',1)})}";

static void USER_FUNC web_js_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_js);
}

/* 校准页专用逻辑。切到校准页时由 /t.js 拉取并 eval 注入，末尾自动填系数 */
static const char web_js_c[] =
	"function loadCal(){ajax('/api?act=cc',0,function(c){var o=JSON.parse(c);"
	"$('kp').value=o.kp;$('ku').value=o.ku;$('ki').value=o.ki})}"
	"function lk(v){ajax('/api?act=lock&v='+v,0,function(){go(2)})}"
	"function adj(t){var e=t=='p'?'bp':t=='u'?'bu':'bi',v=$(e).value;"
	"if(!v){msg('请先填实测值',1);return}"
	"ajax('/api?act=adj&t='+t+'&v='+v,0,function(r){"
	"if(r[6]=='1'){loadCal();msg('标定完成，记得点保存系数')}"
	"else msg('标定失败：当前没有脉冲，或数值非法',1)})}"
	"function saveCal(){ajax('/api?act=cal&kp='+$('kp').value+'&ku='+$('ku').value"
	"+'&ki='+$('ki').value,0,function(t){"
	"msg(t[6]=='1'?'系数已保存':'保存失败',t[6]!='1')})}"
	"loadCal();";

static void USER_FUNC web_js_c_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_js_c);
}

/* 设置页专用逻辑，切到设置页时由 /t.js 拉取并 eval 注入 */
static const char web_js_u[] =
	/*
	 * E 是 encodeURIComponent 的别名。这个分片贴着 httpd 的缓冲上限，
	 * 而汉字在 UTF-8 下 3 字节一个，提示句在这里很贵 —— 所以确认文案
	 * 都压缩过（完整说明在 doc/HOMEKIT.md）。
	 *
	 * 配对码相关的 doHk() 不在这里，在独立的 /h.js 分片里 ——
	 * 硬塞进来的话 /u.js 会冲破 950 字节的安全线。
	 */
	"var E=encodeURIComponent;"
	"function fillCfg(){ajax('/api?act=cg',0,function(t){"
	"var c=JSON.parse(t),f=['mq','pt','us','pw','nm','hs','mx','hk'];"
	"for(var i=0;i<8;i++)$(f[i]).value=c[f[i]];$('hkst').innerHTML=c.hkst})}"
	"function saveCfg(){ajax('/api?act=cfg&mq='+E($('mq').value)"
	"+'&pt='+(+$('pt').value||1883)"
	"+'&us='+E($('us').value)"
	"+'&pw='+E($('pw').value)"
	"+'&nm='+E($('nm').value)"
	"+'&hs='+E($('hs').value)"
	"+'&mx='+$('mx').value+'&hc='+E($('hk').value),0,function(t){"
	"msg(t[6]=='1'?'已保存，MQTT需重启':'保存失败',t[6]!='1')})}"
	"function doClr(){if(confirm('清零电量？'))ajax('/api?act=clr',0,function(){msg('已清零')})}"
	"function doRst(){if(confirm('重启设备？'))ajax('/api?act=rst',0,function(){msg('重启中')})}"
	"fillCfg();";

static void USER_FUNC web_js_u_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_js_u);
}

/*
 * HomeKit 专用逻辑。拆成独立分片纯粹是为了体积：/u.js 已经顶到
 * httpd 的响应缓冲上限，塞不下这一段。
 *
 * 注意加载顺序：/h.js 与 /u.js 都是由 go(3) 用 ld() 注入 <script> 的，
 * 两个请求并发，**谁先到不确定**。所以这里不能引用 /u.js 里定义的
 * 任何东西（连 E 都不行），只能用 /t.js 里的 $ / ajax / msg。
 */
static const char web_js_h[] =
	"function doHk(){if(confirm('换配对码？已配对设备需重配'))"
	"ajax('/api?act=hkg',0,function(t){var o=JSON.parse(t);"
	"if(o.hc){$('hk').value=o.hc;$('hkst').innerHTML='未配对';msg('已换码')}"
	"else msg('换码失败',1)})}";

static void USER_FUNC web_js_h_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_js_h);
}

/*
 * 按键页(/t4)逻辑。
 * web_js_k 的 F[] 是 KEY_FUNC_xxx 的页面文案，顺序必须和 user_config.h
 * 的功能码一致：0无 1总开 2单开 3软启 4AP 5SL 6童锁 7重启。
 * 选中"单开"时该行出现 6 个插口复选框（掩码 bit0~bit5=口1~口6）。
 */
static const char web_js_k[] =
	"var F=['无','总开','单开','软启','AP','SL','童锁','重启'];"
	"function ki(j){$('km'+j).style.display=$('k'+j).value==2?'':'none'}"
	"function fk(){var j,i,s,h;"
	"for(j=0;j<5;j++){s=$('k'+j);for(i=0;i<8;i++)s.options[i]=new Option(F[i],i);"
	"for(h='',i=0;i<6;i++)h+='<label><input type=checkbox id=m'+j+i+'>'+(i+1)+'</label>';"
	"$('km'+j).innerHTML=h}}"
	"function loadK(){ajax('/api?act=kq&p=0',0,function(x){var o=JSON.parse(x),j,i;"
	"$('cl').value=o.cl;"
	"for(j=0;j<5;j++){$('k'+j).value=o.k[j];"
	"for(i=0;i<6;i++)$('m'+j+i).checked=!!(o.m[j]&1<<i);ki(j)}})}"
	"function saveKey(){var u='/api?act=ks&cl='+$('cl').value,j,i,m;"
	"for(j=0;j<5;j++){m=0;for(i=0;i<6;i++)if($('m'+j+i).checked)m+=1<<i;"
	"u+='&m'+j+'='+m}"
	"for(j=0;j<5;j++)u+='&k'+j+'='+$('k'+j).value;"
	"ajax(u,0,function(t){msg(t[6]=='1'?'已保存':'保存失败',t[6]!='1')})}"
	"fk();loadK();";

static void USER_FUNC web_js_k_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_js_k);
}

/*
 * 任务页逻辑拆成两个分片（单片 ~1700B 塞不下 httpd 缓冲）：
 *   /k2.js —— 生成 5 行任务的 HTML（每行 3 个时间下拉 + 动作/启用下拉 +
 *             每周复选框 + 7 个星期复选框），末尾 ld('k3.js') 链式加载；
 *   /k3.js —— 填插口下拉、读任务（loadT）、存任务（saveT），末尾自动 loadT。
 * 链式加载保证顺序：k2 执行完才插入 k3，k3 一定后跑（行结构已就绪）。
 *
 * 数据格式（act=kq 的 t[j]）：[时,分,秒,周码,动作,启用]。
 * 周码：bit0=每周重复，bit1~bit7 依次为 日一二三四五六（1<<(d+1)）。
 * 行控件 id：e 启用 / h 时 / i 分 / s 秒 / a 动作 / r 每周 / w{j}{d} 星期。
 * z() 补零函数在 k2 定义、k3 也要用 —— 链式加载保证了可见性。
 */
static const char web_js_k2[] =
	"var D='日一二三四五六',H='',M='',x;"
	"function z(n){return(n>9?'':'0')+n}"
	"for(x=0;x<24;x++)H+='<option>'+z(x);"
	"for(x=0;x<60;x++)M+='<option>'+z(x);"
	"function build(){var s='',j,d;"
	"for(j=0;j<5;j++){s+='<table><tr><th>任务'+(j+1)"
	/* ★ 启用下拉必须挂 onchange=st(j) —— st() 是置灰切换的唯一入口，
	 * 漏挂的话用户手动切换启用/禁用没有任何响应，整行永远不可编辑 */
	"+' <select id=e'+j+' onchange=st('+j+')>"
	"<option value=0>禁用<option value=1>启用</select></th></tr><tr><td>"
	"<select id=h'+j+'>'+H+'</select>:<select id=i'+j+'>'+M"
	"+'</select>:<select id=s'+j+'>'+M+'</select> <select id=a'+j"
	"+'><option value=0>关闭<option value=1>打开</select></td></tr><tr><td>"
	"<input type=checkbox id=r'+j+'>每周';"
	"for(d=0;d<7;d++)s+='<input type=checkbox id=w'+j+d+'>'+D[d];"
	"s+='</td></tr></table>'}"
	"$('tt').innerHTML=s}"
	"function st(j){var d,on=$('e'+j).value==1,L='hisar';"
	"for(d=0;d<5;d++)$(L[d]+j).disabled=!on;"
	"for(d=0;d<7;d++)$('w'+j+d).disabled=!on}"
	"build();ld('k3.js');";

static void USER_FUNC web_js_k2_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_js_k2);
}

static const char web_js_k3[] =
	"function fk(){var s='',j;for(j=0;j<6;j++)s+='<option value='+j+'>'+(j+1);"
	"$('tp').innerHTML=s}"
	"function loadT(){ajax('/api?act=kq&p='+$('tp').value,0,function(x){"
	"var o=JSON.parse(x),j,t,d;"
	"for(j=0;j<5;j++){t=o.t[j];$('e'+j).value=t[5];"
	"$('h'+j).value=z(t[0]);$('i'+j).value=z(t[1]);$('s'+j).value=z(t[2]);"
	"$('a'+j).value=t[4];$('r'+j).checked=t[3]&1;"
	"for(d=0;d<7;d++)$('w'+j+d).checked=!!(t[3]&1<<d+1);st(j)}})}"
	"function saveT(){var p=$('tp').value,n=0,j,d,u,rp;"
	"for(j=0;j<5;j++){rp=$('r'+j).checked?1:0;"
	"for(d=0;d<7;d++)if($('w'+j+d).checked)rp+=1<<d+1;"
	"u='/api?act=kt&p='+p+'&t='+j+'&h='+$('h'+j).value+'&i='+$('i'+j).value"
	"+'&s='+$('s'+j).value+'&r='+rp+'&a='+$('a'+j).value+'&e='+$('e'+j).value;"
	"ajax(u,0,function(t){if(t[6]!='1')msg('保存失败',1);"
	"else if(++n==5)msg('已保存5条')})}}"
	"fk();loadT();";

static void USER_FUNC web_js_k3_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_js_k3);
}

/*------------------------------------------------------------- 各 Tab 内容 */

/* 状态页：后端把数据直接拼进 HTML，前端只做整块替换。
 * 只放功率与电量 —— 电压/电流/CF 调试用，调试时看校准页或 MQTT。 */
static void USER_FUNC web_t0_cbk(char *url, char *rsp)
{
	char p[12];
	char et[16], ey[16], e[16];
	char up[24];
	char time_s[24] = {0};
	uint32_t sec = hfsys_get_time() / 1000;

	bl0937_fmt_float(p, bl0937.power, 1);
	bl0937_fmt_float(et, bl0937_today_kwh(), 3);
	bl0937_fmt_float(ey, bl0937_yesterday_kwh(), 3);
	bl0937_fmt_float(e, bl0937_energy_kwh(), 3);

	get_time_string(time_s, 24);

	sprintf(up, "%ud %02u:%02u:%02u", (unsigned)(sec / 86400),
			(unsigned)(sec / 3600 % 24), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));

	sprintf(rsp,
			"<table><tr><th colspan=2>设备</th></tr>"
			"<tr><td>WiFi</td><td>%s</td></tr>"
			"<tr><td>IP</td><td>%s</td></tr>"
			"<tr><td>MAC</td><td>%s</td></tr>"
			"<tr><td>时间</td><td>%s</td></tr>"
			"<tr><td>运行</td><td>%s</td></tr></table>"
			"<table><tr><th colspan=2>电量</th></tr>"
			"<tr><td>功率</td><td>%s W</td></tr>"
			"<tr><td>今日</td><td>%s kWh</td></tr>"
			"<tr><td>昨日</td><td>%s kWh</td></tr>"
			"<tr><td>总计</td><td>%s kWh</td></tr></table>"
			"<div class=ct>%s</div>",
			(char *)g_hf_config_file.sta_ssid, strIp, strMac, time_s, up,
			p, et, ey, e,
			bl0937.valid ? "" : "<p class=tip>当前计量数据被判定为无效"
								"（脉冲异常），请检查 BL0937 是否损坏。</p>");
}

/* 控制页：按钮上直接带开关状态。整块由后端生成，前端只替换 innerHTML */
static void USER_FUNC web_t1_cbk(char *url, char *rsp)
{
	char *r = rsp;
	uint8_t i;

	r += sprintf(r, "<table><tr><th colspan=2>插座开关</th></tr>"
					"<tr><td colspan=2 class=ct>");

	for (i = 0; i < PLUG_NUM; i++)
		r += sprintf(r, "<button class=\"b %s\" onclick=\"setplug(%d,-1)\">%d</button>",
					 plug_status.plug[i] ? "ok" : "no", i, i + 1);

	r += sprintf(r, "</td></tr></table><div class=ct>"
					"<button class=\"b ok\" onclick=\"setplug('all',1)\">全开</button> "
					"<button class=\"b no\" onclick=\"setplug('all',0)\">全关</button></div>");
	*r = '\0';
}

/* 校准页：实时脉冲频率 + CF1 档位 + 一键标定。
 * 长度吃紧，所以电压/电流/功率的实时值不在这里重复（状态页已有），
 * 只放标定真正需要的脉冲频率。改这个函数务必重跑字节数检查。 */
static void USER_FUNC web_t2_cbk(char *url, char *rsp)
{
#if BL0937_USE_CF1
	const char *sl;
	char cf_s[8];

	/* 显示有效脉冲率（周期法下带小数），窗口计数在低功率时只有 0/1 没意义 */
	bl0937_fmt_float(cf_s, bl0937.cf_eff_hz, 1);

	if (bl0937.sel_lock == 0)
		sl = "自动轮换";
	else
		sl = bl0937.sel_state ? "电流锁定" : "电压锁定";

	sprintf(rsp,
			"<p class=ct>CF %s / CF1 %u Hz / %s</p>"
			"<table><tr><th colspan=2>标定</th></tr>"
			"<tr><td>功率W</td><td><input id=bp class=s>"
			"<button class=\"b i\" onclick=\"adj('p')\">定</button></td></tr>"
			"<tr><td>电压V</td><td><input id=bu class=s>"
			"<button class=\"b i\" onclick=\"adj('u')\">定</button></td></tr>"
			"<tr><td>电流A</td><td><input id=bi class=s>"
			"<button class=\"b i\" onclick=\"adj('i')\">定</button></td></tr>"
			"<tr><td>脉冲/kW</td><td><input id=kp></td></tr>"
			"<tr><td>脉冲/V</td><td><input id=ku></td></tr>"
			"<tr><td>脉冲/A</td><td><input id=ki></td></tr>"
			"<tr><td>CF1</td><td><button class=\"b i\" onclick=\"lk(1)\">电压</button>"
			"<button class=\"b i\" onclick=\"lk(2)\">电流</button>"
			"<button class=\"b no\" onclick=\"lk(0)\">自动</button></td></tr>"
			"</table>"
			"<div class=ct><button class=\"b ok\" onclick=saveCal()>保存</button></div>",
			cf_s, (unsigned)bl0937.cf1_hz, sl);
#else
	/* CF1 关闭（默认）：没有脉冲频率可标，电压改成手填一个电网常数，
	 * 电流由 P/V 反算。ku 这个输入框承载的就是这个常数，
	 * 隐藏的 ki 只是为了让共用的 loadCal()/saveCal() 不去访问不存在的元素。 */
	char cf_s[8];
	bl0937_fmt_float(cf_s, bl0937.cf_eff_hz, 1);

	sprintf(rsp,
			"<p class=ct>CF %s Hz</p>"
			"<table><tr><th colspan=2>标定</th></tr>"
			"<tr><td>功率W</td><td><input id=bp class=s>"
			"<button class=\"b i\" onclick=\"adj('p')\">定</button></td></tr>"
			"<tr><td>实测V</td><td><input id=bu class=s>"
			"<button class=\"b i\" onclick=\"adj('u')\">存</button></td></tr>"
			"<tr><td>脉冲/kW</td><td><input id=kp></td></tr>"
			"<tr><td>电网V</td><td><input id=ku><input type=hidden id=ki></td></tr>"
			"</table>"
			"<p class=tip>CF1 未启用（GPIO 中断只有 2 个通道，按键+CF 已占满）。"
			"电压用上面的常数，电流按 P/V 算。</p>"
			"<div class=ct><button class=\"b ok\" onclick=saveCal()>保存</button></div>",
			cf_s);
#endif
}

/* 按键页(/t4)：童锁 + 连击/长按功能映射。select 的选项与"单开"的插口
 * 多选框都由 /k.js 生成（省字节）。行标签必须和 KEY_MAP_NUM 对应
 * （0单击 1双击 2长3s 3长5s 4长10s）。 */
static void USER_FUNC web_key_cbk(char *url, char *rsp)
{
	static const char *lbl[KEY_MAP_NUM] = {"单击", "双击", "长按3秒", "长按5秒", "长按10秒"};
	char *r = rsp;
	uint8_t i;

	r += sprintf(r, "<table><tr><th colspan=2>按键与童锁</th></tr>"
					"<tr><td>童锁</td><td><select id=cl>"
					"<option value=0>关<option value=1>开</select></td></tr>");
	for (i = 0; i < KEY_MAP_NUM; i++)
		r += sprintf(r, "<tr><td>%s</td><td>"
					 "<select id=k%d onchange=ki(%d)></select>"
					 "<div id=km%d></div></td></tr>",
					 lbl[i], i, i, i);
	strcpy(r, "</table>"
			  "<p class=tip>童锁开启后按键全部失效（双击=童锁开关仍有效，"
			  "这是唯一的物理解锁出口）；上电时按住按键会强制进配网。"
			  "改完点保存。</p>"
			  "<div class=ct><button class=\"b ok\" onclick=saveKey()>保存</button></div>");
}

/* 任务页(/t5)：骨架极简 —— 5 行任务的下拉/复选框结构由 /k2.js 生成。
 * 后端拼 5 行控件会顶爆响应缓冲，所以这里只留容器。 */
static void USER_FUNC web_task_cbk(char *url, char *rsp)
{
	strcpy(rsp, "<table><tr><th>定时任务 插口 "
				"<select id=tp onchange=loadT()></select></th></tr></table>"
				"<div id=tt></div>"
				"<p class=tip>禁用的行整体置灰。不勾每周=只执行一次，"
				"执行后自动停用；星期多选决定哪几天生效，全不勾则永不触发。</p>"
				"<div class=ct><button class=\"b ok\" onclick=saveT()>保存</button></div>");
}

/* 设置页表单。输入框 id 用短名，和 /u.js 里的字段数组一一对应 */
static const char web_t3_html[] =
	"<table><tr><th colspan=2>MQTT</th></tr>"
	"<tr><td>服务器</td><td><input id=mq></td></tr>"
	"<tr><td>端口</td><td><input id=pt></td></tr>"
	"<tr><td>用户名</td><td><input id=us></td></tr>"
	"<tr><td>密码</td><td><input id=pw></td></tr>"
	"<tr><td>设备名</td><td><input id=nm></td></tr>"
	"<tr><td>发现前缀</td><td><input id=hs></td></tr></table>"
	"<table><tr><th colspan=2>保护</th></tr>"
	"<tr><td>过载</td><td><input id=mx>W，0=关</td></tr></table>"
	/* HomeKit：配对码是唯一能在没有屏幕的排插上看到它的地方 */
	/*"<table><tr><th colspan=2>HomeKit</th></tr>"
	"<tr><td>配对码</td><td><input id=hk></td></tr>"
	"<tr><td>状态</td><td id=hkst>-</td></tr></table>"*/

	/* 提示刻意写短：/t3 贴着 httpd 的缓冲上限，UTF-8 下一个汉字 3 字节。
	 * 完整解释在 doc/HOMEKIT.md。 */

	/* "<p class=tip>改码=解绑重配</p>" */

	"<div class=ct>"
	"<button class=\"b i\" onclick=saveCfg()>保存</button> "
	/* "<button class=\"b i\" onclick=doHk()>换码</button> "*/
	"<button class=\"b d\" onclick=doClr()>清电量</button> "
	"<button class=\"b d\" onclick=doRst()>重启</button></div>";

static void USER_FUNC web_t3_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_t3_html);
}

/* 关于页(/t6)：只留固件信息 */
static const char web_about_html[] =
	"<table><tr><th colspan=2>固件信息</th></tr>"
	"<tr><td>型号</td><td>TC1 A2 (LPT230)</td></tr>"
	"<tr><td>版本</td><td>" VERSION "</td></tr>"
	"<tr><td>编译</td><td>" __DATE__ " " __TIME__ "</td></tr>"
	"<tr><td>基础项目</td><td>phicomm_tc1_a2</td></tr>";

static void USER_FUNC web_about_cbk(char *url, char *rsp)
{
	strcpy(rsp, web_about_html);
}

/*------------------------------------------------------------- 数据/动作 */

/* 自己解析浮点字符串，避免依赖 newlib-nano 里可能被裁剪的 strtod */
static float USER_FUNC parse_float(const char *s, float def)
{
	int32_t ip = 0, fp = 0;
	int32_t dec = 0;
	uint8_t neg = 0, dot = 0, any = 0;

	if (s == NULL)
		return def;

	if (*s == '-')
	{
		neg = 1;
		s++;
	}

	while (*s)
	{
		if (*s == '.')
		{
			if (dot)
				break;
			dot = 1;
		}
		else if (*s >= '0' && *s <= '9')
		{
			any = 1;
			if (!dot)
			{
				if (ip < 1000000)
					ip = ip * 10 + (*s - '0');
			}
			else if (dec < 4)
			{
				fp = fp * 10 + (*s - '0');
				dec++;
			}
		}
		else
			break;
		s++;
	}

	if (!any)
		return def;

	{
		int32_t scale = 1;
		while (dec-- > 0)
			scale *= 10;
		float v = (float)ip + (float)fp / (float)scale;
		return neg ? -v : v;
	}
}

/* 取 url 中 "key=" 之后的内容起始指针 */
static char *USER_FUNC url_get(char *url, const char *key)
{
	char *p = strstr(url, key);
	return p ? p + strlen(key) : NULL;
}

static int USER_FUNC url_hex(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/*
 * 把 query 里的 %XX 解码后拷进 dst，最多写 size-1 个字符并保证 NUL 结尾。
 * 前端对输入框内容做了 encodeURIComponent，后端必须解码，
 * 否则密码/地址里的 @ : / % 等会被原样存进去。
 * encodeURIComponent 不产生 '+'（空格编码为 %20），所以不用处理 '+'。
 * 注意必须在原始 '&' 处停止 —— query 里的 '&' 是参数分隔符
 * （值里的 '&' 已被前端编码成 %26），否则会把后面的参数整个吞进当前字段。
 */
static void USER_FUNC url_field(char *dst, const char *src, size_t size)
{
	size_t o = 0;

	if (src == NULL || size == 0)
		return;
	while (src[0] != '\0' && src[0] != '&' && o + 1 < size)
	{
		if (src[0] == '%' && url_hex(src[1]) >= 0 && url_hex(src[2]) >= 0)
		{
			dst[o++] = (char)(url_hex(src[1]) * 16 + url_hex(src[2]));
			src += 3;
		}
		else
			dst[o++] = *src++;
	}
	dst[o] = '\0';
}

/* act=st：状态 JSON（供脚本/上位机使用，页面走 /t0） */
static void USER_FUNC web_status_json(char *res)
{
	char v[12], a[12], p[12];
	char et[16], ey[16], e[16];
	uint8_t i;
	char *r = res;

	bl0937_fmt_float(v, bl0937.voltage, 1);
	bl0937_fmt_float(a, bl0937.current, 2);
	bl0937_fmt_float(p, bl0937.power, 1);
	bl0937_fmt_float(et, bl0937_today_kwh(), 3);
	bl0937_fmt_float(ey, bl0937_yesterday_kwh(), 3);
	bl0937_fmt_float(e, bl0937_energy_kwh(), 3);

	r += sprintf(r, "{\"voltage\":%s,\"current\":%s,\"power\":%s,"
					"\"today\":%s,\"yesterday\":%s,\"energy\":%s,"
					"\"cf\":%u,\"cf1\":%u,\"valid\":%u,\"up\":%u,\"cl\":%u,",
				 v, a, p, et, ey, e,
				 (unsigned)bl0937.cf_hz, (unsigned)bl0937.cf1_hz,
				 (unsigned)bl0937.valid, (unsigned)(hfsys_get_time() / 1000),
				 user_plug_config.enable_child_lock ? 1u : 0u);

	for (i = 0; i < PLUG_NUM; i++)
		r += sprintf(r, "\"plug_%d\":%d,", i, plug_status.plug[i] ? 1 : 0);

	if (*(r - 1) == ',')
		r--;
	*r++ = '}';
	*r = '\0';
}

/* act=cg：当前 MQTT 与保护配置（设置页用） */
static void USER_FUNC web_cfg_json(char *res)
{
	char hk[HK_CODE_BUF];
	const char *hkst = "未启用";

	/*
	 * HomeKit 未启用时也要回这两个字段：设置页的 HTML 是无条件输出的
	 * （不想让"启用开关"把页面结构搞出两套），前端 fillCfg 会去填
	 * $('hk')，字段缺失会让整段 JS 抛异常、后面的填充全部不执行。
	 */
	hk[0] = '\0';
#ifdef HK_ENABLE
	{
		const char *c = hk_net_get_code();
		if (c != NULL && c[0])
			snprintf(hk, sizeof(hk), "%s", c);
		hkst = hk_net_is_paired() ? "已配对" : "未配对";
	}
#endif

	sprintf(res,
			"{\"mq\":\"%s\",\"pt\":%d,\"us\":\"%s\",\"pw\":\"%s\",\"nm\":\"%s\",\"hs\":\"%s\","
			"\"mx\":%d,\"hk\":\"%s\",\"hkst\":\"%s\"}",
			user_mqtt_config.seraddr, (int)user_mqtt_config.port,
			user_mqtt_config.username, user_mqtt_config.password,
			user_mqtt_config.clientid, user_mqtt_config.hass_topic,
			(int)bl0937_cal.max_power, hk, hkst);
}

/* act=cc：只回计量系数（校准页用） */
static void USER_FUNC web_cal_json(char *res)
{
	char kp[16], ku[16], ki[16];

	bl0937_fmt_float(kp, bl0937_cal.power_pulses_per_kw, 2);
	bl0937_fmt_float(ki, bl0937_cal.current_pulses_per_a, 3);
#if BL0937_USE_CF1
	bl0937_fmt_float(ku, bl0937_cal.voltage_pulses_per_v, 3);
#else
	/* 没有 CF1 时 ku 这个输入框改成"电网电压"（常数），页面逻辑不用改 */
	bl0937_fmt_float(ku, bl0937_cal.nominal_v, 1);
#endif

	sprintf(res, "{\"kp\":\"%s\",\"ku\":\"%s\",\"ki\":\"%s\"}", kp, ku, ki);
}

static void USER_FUNC web_api_cbk(char *url, char *rsp)
{
	char *act = url_get(url, "act=");

	if (act == NULL)
	{
		strcpy(rsp, "{\"ok\":0,\"msg\":\"missing act\"}");
		return;
	}

	/* 状态 */
	if (!strncmp(act, "st", 2))
	{
		web_status_json(rsp);
		return;
	}

	/* 当前配置 */
	if (!strncmp(act, "cg", 2))
	{
		web_cfg_json(rsp);
		return;
	}

	/* 当前计量系数 */
	if (!strncmp(act, "cc", 2))
	{
		web_cal_json(rsp);
		return;
	}

	/*
	 * act=kq&p=N：童锁 + 按键映射/掩码 + 插口 N 的 5 条任务，按键/任务页用。
	 * 任务数组元素顺序 = [时,分,秒,周码,动作,启用]，与 /k2.js 对应。
	 * k=功能码、m=单开掩码（下标 0~4 = 单击/双击/长3s/长5s/长10s）。
	 */
	if (!strncmp(act, "kq", 2))
	{
		char *pp = url_get(url, "&p=");
		char *r = rsp;
		uint8_t i, p;

		p = pp ? (uint8_t)atoi(pp) : 0;
		if (p >= PLUG_NUM)
			p = 0;

		r += sprintf(r, "{\"cl\":%u,\"k\":[%u,%u,%u,%u,%u],\"m\":[%u,%u,%u,%u,%u],\"t\":[",
					 user_plug_config.enable_child_lock ? 1u : 0u,
					 user_plug_config.key_map[0], user_plug_config.key_map[1],
					 user_plug_config.key_map[2], user_plug_config.key_map[3],
					 user_plug_config.key_map[4],
					 user_plug_config.key_mask[0], user_plug_config.key_mask[1],
					 user_plug_config.key_mask[2], user_plug_config.key_mask[3],
					 user_plug_config.key_mask[4]);
		for (i = 0; i < PLUG_TIME_TASK_NUM; i++)
		{
			TASK_CONFIG *t = &user_plug_config.plug[p].task[i];

			r += sprintf(r, "%s[%u,%u,%u,%u,%u,%u]", i ? "," : "",
						 t->hour, t->minute, t->second, t->repeat,
						 t->action, t->enable);
		}
		strcpy(r, "]}");
		return;
	}

	/* act=ks&cl=&m0=..&m4=&k0=..&k4=：保存童锁/按键映射/单开掩码（落盘立即生效） */
	if (!strncmp(act, "ks", 2))
	{
		char *p;
		int i;

		p = url_get(url, "&cl=");
		if (p)
			user_plug_config.enable_child_lock = atoi(p) ? 1 : 0;
		for (i = 0; i < KEY_MAP_NUM; i++)
		{
			char key[6];

			sprintf(key, "&m%d=", i);
			p = url_get(url, key);
			if (p)
				user_plug_config.key_mask[i] = (uint8_t)(atoi(p) & 0x3F);
		}
		for (i = 0; i < KEY_MAP_NUM; i++)
		{
			char key[6];

			sprintf(key, "&k%d=", i);
			p = url_get(url, key);
			if (p)
			{
				int f = atoi(p);

				/* 功能码越界一律按"无动作"存，防止写进垃圾值后按键乱动作 */
				if (f < KEY_FUNC_NONE || f > KEY_FUNC_REBOOT)
					f = KEY_FUNC_NONE;
				user_plug_config.key_map[i] = (uint8_t)f;
			}
		}
		/* 只置标志，由 userbin 保存线程统一落盘（唯一写者，见 user_rtc.c）。
		 * ★ 不要在这里同步 save_plug_config()：userbin 文件层必须只有一个
		 *   写者，多线程并发擦同一页会互相覆盖。 */
		update_plug_config_flag = true;
		strcpy(rsp, "{\"ok\":1}");
		return;
	}

	/*
	 * act=kt&p=&t=&h=&i=&s=&r=&a=&e=：写单条定时任务。
	 * 页面逐条发短 URL —— 不走 /config?json=，那边 URL 稍长就会被
	 * httpd 截断（返回 json invalid 还是 200，纯坑）。
	 * 只置 update_plug_config_flag，由 userbin 保存线程落盘（改内存≠落盘的老教训）。
	 * ★ 千万别挪回同步 save_plug_config()，也别放回 rtc 定时器回调：
	 *   hffile_userbin_write() 在 hftimer 上下文里会复位设备（10/10 实测）。
	 */
	if (!strncmp(act, "kt", 2))
	{
		char *pp = url_get(url, "&p=");
		char *pt = url_get(url, "&t=");
		char *pv;
		int p = pp ? atoi(pp) : -1;
		int t = pt ? atoi(pt) : -1;
		TASK_CONFIG *task;

		if (p < 0 || p >= PLUG_NUM || t < 0 || t >= PLUG_TIME_TASK_NUM)
		{
			strcpy(rsp, "{\"ok\":0}");
			return;
		}
		task = &user_plug_config.plug[p].task[t];

		pv = url_get(url, "&h=");
		if (pv)
			task->hour = (uint8_t)atoi(pv);
		pv = url_get(url, "&i=");
		if (pv)
			task->minute = (uint8_t)atoi(pv);
		pv = url_get(url, "&s=");
		if (pv)
			task->second = (uint8_t)atoi(pv);
		pv = url_get(url, "&r=");
		if (pv)
			task->repeat = (uint8_t)atoi(pv);
		pv = url_get(url, "&a=");
		if (pv)
			task->action = atoi(pv) ? 1 : 0;
		pv = url_get(url, "&e=");
		if (pv)
		{
			task->enable = atoi(pv) ? 1 : 0;
			if (task->enable)
				user_plug_config_enable = true; /* 唤醒任务调度（同 json 通道的做法） */
		}

		/* 只置标志，由 userbin 保存线程统一落盘。
		 * ★ 不要在这里同步 save_plug_config()：页面 saveT() 是 5 条并发发
		 *   请求，同步写会连擦 5 次 flash；而且 userbin 文件层必须有唯一
		 *   写者，多线程并发擦同一页会互相覆盖。
		 * 读回走 /task 或 act=kq，读的是内存值，落盘前就已生效。 */
		update_plug_config_flag = true;
		strcpy(rsp, "{\"ok\":1}");
		return;
	}

	/*
	 * 重新生成一个随机配对码（设置页的"换码"按钮）。
	 * 用 TRNG，不走用户输入的校验路径 —— 但 hk_net_gen_code 内部
	 * 仍会过一遍 hk_code_valid，保证不会推出被规范禁用的码。
	 */
	if (!strncmp(act, "hkg", 3))
	{
#ifdef HK_ENABLE
		char code[HK_CODE_BUF];

		if (hk_net_gen_code(code, sizeof(code)) == 0)
		{
			sprintf(rsp, "{\"ok\":1,\"hc\":\"%s\",\"hkst\":\"%s\"}",
					code, hk_net_is_paired() ? "已配对" : "未配对");
		}
		else
		{
			strcpy(rsp, "{\"ok\":0,\"msg\":\"gen code fail\"}");
		}
#else
		strcpy(rsp, "{\"ok\":0,\"msg\":\"homekit disabled\"}");
#endif
		return;
	}

	/* 开关：act=set&c=<0-5|all>&v=<0|1|-1>，-1 表示取反 */
	if (!strncmp(act, "set", 3))
	{
		char *pc = url_get(url, "&c=");
		char *pv = url_get(url, "&v=");
		int ch, val;

		if (pv == NULL)
		{
			strcpy(rsp, "{\"ok\":0,\"msg\":\"missing v\"}");
			return;
		}
		val = atoi(pv);

		if (pc != NULL && !strncmp(pc, "all", 3))
		{
			if (val < 0)
				val = relay_out() ? 0 : 1;
			user_relay_set_all(val ? 1 : 0);
		}
		else if (pc != NULL)
		{
			ch = atoi(pc);
			if (ch < 0 || ch >= PLUG_NUM)
			{
				strcpy(rsp, "{\"ok\":0,\"msg\":\"bad channel\"}");
				return;
			}
			if (val < 0)
				val = plug_status.plug[ch] ? 0 : 1;
			user_relay_set((uint8_t)ch, val ? 1 : 0);
		}
		else
		{
			strcpy(rsp, "{\"ok\":0,\"msg\":\"missing c\"}");
			return;
		}

		strcpy(rsp, "{\"ok\":1}");
		return;
	}

	/*
	 * 保存设置页：act=cfg&mq=&pt=&us=&pw=&nm=&hs=&mx=
	 * nm 是 MQTT clientid（页面上的"设备名"），hs 是 hass_topic，
	 * 两者被 update_mqtt_config() 用来派生 pub/sub topic。
	 */
	if (!strncmp(act, "cfg", 3))
	{
		char *p;

		url_field(user_mqtt_config.seraddr, url_get(url, "&mq="),
				  sizeof(user_mqtt_config.seraddr));
		p = url_get(url, "&pt=");
		if (p)
		{
			int port = atoi(p);

			if (port > 0 && port <= 65535)
				user_mqtt_config.port = (unsigned short)port;
		}
		url_field(user_mqtt_config.username, url_get(url, "&us="),
				  sizeof(user_mqtt_config.username));
		url_field(user_mqtt_config.password, url_get(url, "&pw="),
				  sizeof(user_mqtt_config.password));
		/*
		 * hs/nm 不能放满 128：update_mqtt_config() 用它们派生 topic
		 * （最长的是 cmd_topic = hass+"/switch/"+clientid+"/config/cmd"，
		 * 各 topic 缓冲只有 129 字节）。hass<=64 且 clientid<=40 时
		 * 最长派生 128 字节（含 NUL），贴边但不溢出。
		 */
		url_field(user_mqtt_config.hass_topic, url_get(url, "&hs="), 64);
		url_field(user_mqtt_config.clientid, url_get(url, "&nm="), 40);

		p = url_get(url, "&mx=");
		if (p)
			bl0937_cal.max_power = (uint16_t)atoi(p);

#ifdef HK_ENABLE
		/*
		 * 配对码。**只有真的改了才落盘** ——
		 * hk_net_set_code() 的语义是"改码 = 恢复出厂"（会解绑所有 iPhone、
		 * 重新生成 LTSK/LTPK，原因见 hk_net.h 的声明）。而用户点"保存"
		 * 往往只是改 MQTT 或过载阈值，页面上的配对码是原样带回来的；
		 * 不加这个比较的话，每保存一次设置就把 HomeKit 恢复出厂一次。
		 */
		p = url_get(url, "&hc=");
		if (p)
		{
			char code[HK_CODE_BUF];

			url_field(code, p, sizeof(code));
			if (code[0] && strcmp(code, hk_net_get_code()) != 0)
				hk_net_set_code(code);
		}
#endif

		update_mqtt_config(&user_mqtt_config); /* 刷新内存里的派生 topic */
		/* 只置标志，由 userbin 保存线程统一写 flash（唯一写者）。
		 * bl0937_cal_save() 现在也只置标志，max_power 由线程一起落盘。 */
		update_mqtt_config_flag = true;
		bl0937_cal_save();

		strcpy(rsp, "{\"ok\":1}");
		return;
	}

	/* 保存计量校准：act=cal&kp=&ku=&ki=&mx=（只传需要改的项） */
	if (!strncmp(act, "cal", 3))
	{
		char *p;

		p = url_get(url, "kp=");
		if (p)
			bl0937_cal.power_pulses_per_kw = parse_float(p, bl0937_cal.power_pulses_per_kw);
		p = url_get(url, "ku=");
		if (p)
#if BL0937_USE_CF1
			bl0937_cal.voltage_pulses_per_v = parse_float(p, bl0937_cal.voltage_pulses_per_v);
#else
			bl0937_cal.nominal_v = parse_float(p, bl0937_cal.nominal_v);
#endif
		p = url_get(url, "ki=");
		if (p)
			bl0937_cal.current_pulses_per_a = parse_float(p, bl0937_cal.current_pulses_per_a);
		p = url_get(url, "mx=");
		if (p)
			bl0937_cal.max_power = (uint16_t)atoi(p);

		bl0937_cal_save();
		strcpy(rsp, "{\"ok\":1}");
		return;
	}

	/*
	 * 一键标定：act=adj&t=<p|u|i>&v=<实测值>
	 * 用当前这一个采样窗口的脉冲频率反算系数，所以要在负载稳定时操作。
	 * 电压/电流必须先锁档并等几秒，否则 CF1 可能正处在另一档上。
	 */
	if (!strncmp(act, "adj", 3))
	{
		char *pt = url_get(url, "&t=");
		char *pv = url_get(url, "&v=");
		float val = parse_float(pv, 0.0f);

		if (pt == NULL || pv == NULL || val <= 0.0f)
		{
			strcpy(rsp, "{\"ok\":0,\"msg\":\"bad value\"}");
			return;
		}

		if (*pt == 'p')
		{
			/* 用有效脉冲率（计数法=窗口数，周期法=1000/间隔ms）。
			 * 这样小负载（周期法生效时窗口脉冲数只有 0/1）也能标定 */
			if (bl0937.cf_eff_hz <= 0.0f)
			{
				strcpy(rsp, "{\"ok\":0,\"msg\":\"no cf pulse\"}");
				return;
			}
			/* power = eff*1000/kp ⇒ kp = eff*1000/实测W。
			 * 旧版写成 val*1000/cf（分子分母反了），标定结果差 cf²/val² 倍。 */
			bl0937_cal.power_pulses_per_kw = bl0937.cf_eff_hz * 1000.0f / val;
		}
		else if (*pt == 'u' || *pt == 'i')
		{
#if BL0937_USE_CF1
			if (bl0937.cf1_hz == 0)
			{
				strcpy(rsp, "{\"ok\":0,\"msg\":\"no cf1 pulse\"}");
				return;
			}
			if (*pt == 'u')
				bl0937_cal.voltage_pulses_per_v = (float)bl0937.cf1_hz / val;
			else
				bl0937_cal.current_pulses_per_a = (float)bl0937.cf1_hz / val;
#else
			/* 没有 CF1：电压这条改成"把实测电压存成电网电压常数"，直接赋值。
			 * 电流是 P/V 反算出来的，没有可标定的系数。 */
			if (*pt == 'u')
				bl0937_cal.nominal_v = val;
			else
			{
				strcpy(rsp, "{\"ok\":0,\"msg\":\"no cf1, I=P/V\"}");
				return;
			}
#endif
		}
		else
		{
			strcpy(rsp, "{\"ok\":0,\"msg\":\"bad t\"}");
			return;
		}

		bl0937_cal_save();
		strcpy(rsp, "{\"ok\":1}");
		return;
	}

	/* CF1 档位锁定：act=lock&v=<0=自动 1=锁电压 2=锁电流> */
	if (!strncmp(act, "loc", 3))
	{
		char *pv = url_get(url, "&v=");
		bl0937_sel_lock(pv ? (uint8_t)atoi(pv) : 0);
		strcpy(rsp, "{\"ok\":1}");
		return;
	}

	/* 清零电量 */
	if (!strncmp(act, "clr", 3))
	{
		bl0937_energy_clear();
		strcpy(rsp, "{\"ok\":1}");
		return;
	}

	/* 重启 */
	if (!strncmp(act, "rst", 3))
	{
		strcpy(rsp, "{\"ok\":1}");
		system_restart_flag = true;
		return;
	}

	strcpy(rsp, "{\"ok\":0,\"msg\":\"unknown act\"}");
}

/*------------------------------------------------------------- 注册 */

void USER_FUNC user_web_init(void)
{
	httpd_add_page_ex("/admin", "text/html; charset=utf-8", web_index_cbk);
	httpd_add_page_ex("/a.css", "text/css; charset=utf-8", web_css_a_cbk);
	httpd_add_page_ex("/b.css", "text/css; charset=utf-8", web_css_b_cbk);
	httpd_add_page_ex("/t.js", "text/javascript; charset=utf-8", web_js_cbk);
	httpd_add_page_ex("/c.js", "text/javascript; charset=utf-8", web_js_c_cbk);
	httpd_add_page_ex("/u.js", "text/javascript; charset=utf-8", web_js_u_cbk);
	httpd_add_page_ex("/h.js", "text/javascript; charset=utf-8", web_js_h_cbk);
	httpd_add_page_ex("/t0", "text/html; charset=utf-8", web_t0_cbk);
	httpd_add_page_ex("/t1", "text/html; charset=utf-8", web_t1_cbk);
	httpd_add_page_ex("/t2", "text/html; charset=utf-8", web_t2_cbk);
	httpd_add_page_ex("/t3", "text/html; charset=utf-8", web_t3_cbk);
	httpd_add_page_ex("/t4", "text/html; charset=utf-8", web_key_cbk);
	httpd_add_page_ex("/t5", "text/html; charset=utf-8", web_task_cbk);
	httpd_add_page_ex("/t6", "text/html; charset=utf-8", web_about_cbk);
	httpd_add_page_ex("/k.js", "text/javascript; charset=utf-8", web_js_k_cbk);
	httpd_add_page_ex("/k2.js", "text/javascript; charset=utf-8", web_js_k2_cbk);
	httpd_add_page_ex("/k3.js", "text/javascript; charset=utf-8", web_js_k3_cbk);
	httpd_add_page_ex("/api", "application/json; charset=utf-8", web_api_cbk);
}

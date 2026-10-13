include ../Makefile.mk

OBJECTDIR=objs

# 注意：原仓库这里只列了 app_main.c，导致 user_*.c 根本不参与编译，
# 只有 Keil 工程（projects/LPT230_gcc.uvprojx）能完整构建。这里补全。
USERAPP_SRC = \
app_main.c \
user_config.c \
user_function.c \
user_gpio.c \
user_httpd.c \
user_mqtt.c \
user_rtc.c \
user_bl0937.c \
user_web.c \
cJSON/cJSON.c \
$(HOMEKIT_SRC)

# HomeKit（src/homekit/*.c）分阶段实现，见 doc/HOMEKIT.md。
#
# 这些文件被打包进 userapps.a 而不是直接链进固件 —— .a 是静态库，
# 链接器**只提取被引用的 .o**。所以只要 app_main.c 不去调 hk_net_start()，
# 这 ~35KB 就一个字节都不会进固件。
# 想启用就在编译时给 app_main.c 加 -DHK_ENABLE。
HOMEKIT_SRC = \
homekit/hk_sha512.c \
homekit/hk_kdf.c \
homekit/hk_gf.c \
homekit/hk_chachapoly.c \
homekit/hk_curve25519.c \
homekit/hk_ed25519.c \
homekit/hk_bignum.c \
homekit/hk_srp.c \
homekit/hk_tlv.c \
homekit/hk_pair.c \
homekit/hk_json.c \
homekit/hk_acc.c \
homekit/hk_hap.c \
homekit/hk_store.c \
homekit/hk_net.c \
homekit/hk_code.c

# HK_NET_PLATFORM 打开 hk_net.c 里的平台那一半（lwIP / mDNS / userbin / TRNG）。
# 不开的话它只编纯逻辑那一半，主机自测要用。
#
# HK_ENABLE 是"真正启用 HomeKit"的总开关，必须**全局**给 ——
# 它同时影响三个文件：
#   app_main.c  : 是否调 hk_net_start()
#   user_web.c  : 设置页是否显示/可改配对码
#   user_gpio.c : 继电器变化时是否调 hk_net_notify()
# 所以不能只给 app_main.c（那样 Web 页上会显示"未启用"但按钮还在，
# 或者更糟：前后端状态不一致）。
#
# 启用方式：  make HK_FLAGS=-DHK_ENABLE
HK_FLAGS ?=
APPCFLAGS = -DSUPPORT_UART_THROUGH -I. -DHK_NET_PLATFORM $(HK_FLAGS)

USERAPP_OBJS=${addprefix $(OBJECTDIR)/,$(USERAPP_SRC:.c=.o)}

all:
	@mkdir -p objs
	make userapps.a 

userapps.a: $(USERAPP_OBJS)
	echo "$(AR) $(AROPTS) $@ $^"
	$(AR) $(AROPTS) $@ $^

print:
	echo "USERAPP_SRC=$(USERAPP_SRC)"
	echo "USERAPP_OBJS=$(USERAPP_OBJS)"

$(OBJECTDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "-----------------make z1 target:$@ $(makefn3)-----------------"
	$(CC) $(CFLAGS) $(APPCFLAGS) -c $< -o $@

clean:
	/bin/rm -f *.o *.elf *.a *.bin *.asm 
	/bin/rm -rf $(OBJECTDIR)


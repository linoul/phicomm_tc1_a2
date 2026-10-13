/*
 * hk_code.h —— HomeKit 配对码（Setup Code）的校验与生成
 *
 * 为什么单独拆一个模块：配对码看着就是个 "XXX-XX-XXX" 字符串，
 * 但 Apple 的 HAP 规范里有一批**明令禁止**的码，用了的后果是
 * iOS 在输入配对码那一步直接拒绝、且不给任何有用的报错 ——
 * 现场只会看到"配对失败"，没人会想到是码本身不合法。
 * 这种"必须记在代码里"的规则，散落在各处迟早会被人改回去。
 *
 * 规范（HAP R2, Setup Code）的三条硬规则：
 *
 *   1) 第一位不能是 '0' 或 '1'。
 *   2) 八位数字全部相同的码一律禁用：000-00-000 ... 999-99-999。
 *   3) 两个"顺子"单独禁用：123-45-678 和 876-54-321。
 *
 * ★★ 本工程原来的默认码写的是 "123-45-678"，正好命中第 3 条 ★★
 *    也就是说它**永远配不上对**。现在改成首次启动时用 TRNG 随机生成
 *    一个合法码并落盘，Web 后台可以看到、也可以改 —— 排插没有屏幕，
 *    这是唯一能让人看到配对码的地方。
 *
 * 纯 C99，不碰 SDK，随机源由调用方注入（固件填 TRNG，测试填固定值）。
 */
#ifndef HK_CODE_H
#define HK_CODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* "XXX-XX-XXX" 的可见长度（不含 '\0'） */
#define HK_CODE_LEN 10
/* 与 hk_pair.h 的 HK_SETUP_CODE_MAX 对齐：连字符形式 + '\0' */
#define HK_CODE_MAX 12
/* 与 hk_store.h 的 HK_STORE_CODE_LEN 对齐，存进 flash 的宽度 */
#define HK_CODE_BUF 16

/*
 * 兜底码：只有 TRNG 挂了、随机生成失败时才用。
 * 必须自己也是个合法码（过 hk_code_valid）—— 早期版本写的
 * "123-45-678" 是被规范禁用的顺子，用它等于永远配不上对。
 */
#define HK_CODE_DEFAULT "482-19-573"

/*
 * 校验一个配对码是否为**规范允许**的值。
 *   code : 可以是 "12345678" 这种纯数字，也可以是 "123-45-678"
 *          （连字符位置不对也会被拒；想宽松点先调 hk_code_normalize）
 * 返回 1 = 合法可用，0 = 不合法。code 为 NULL 返回 0。
 */
int hk_code_valid(const char *code);

/*
 * 归一化：把用户输入的任意形式（"12345678"、"123 45 678"、"123-45-678"、
 * 甚至 "(123)45-678"）抽成前 8 个数字，按 "XXX-XX-XXX" 排版写入 out。
 * 数字不足 8 个返回 -1。成功返回 0。
 *
 * 注意：归一化**不做**合法性判断，排版完仍要过 hk_code_valid ——
 * 否则用户填 "00000000" 会被排成 "000-00-000" 然后被 iOS 拒。
 */
int hk_code_normalize(const char *in, char *out, size_t cap);

/*
 * 随机生成一个合法配对码。
 *   rand/ctx : 随机源（固件侧填 hk_rand，走硬件 TRNG）
 * 返回 0 成功；-1 参数错，-2 随机源不可用/连续多次都生成了禁用码。
 *
 * 首位取 2..9、其余位取 0..9，都用**拒绝采样**保证均匀
 * （直接对 256 取模会让小数字出现概率偏高 —— 密码学里这是要避的）。
 */
int hk_code_random(char *out, size_t cap,
				   void (*rand)(uint8_t *buf, size_t n, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* HK_CODE_H */

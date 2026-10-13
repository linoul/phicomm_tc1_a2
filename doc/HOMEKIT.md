# HomeKit（纯软件 HAP）实现说明

本分支在 TC1 A2 固件里内建 **HomeKit** 支持：不用买 MFi 认证芯片、不用改装硬件，
iPhone「家庭」App 与 Siri 直接控制 6 路插座。

- 采用的是 **纯软件 HAP**（HomeKit Accessory Protocol），做法与 Homebridge 一致 ——
  Apple 允许非商业用途在无 MFi 协处理器的情况下实现 HAP，只是不能拿去认证售卖。
- 配件在「家庭」App 里的形态：**6 个独立开关**（不做总控、不做自定义功率特征）。

> 本文档记录**分阶段推进的计划**与**各阶段的验证状态**。
> 已完成的是：
> - **第一阶段（密码学内核）** —— 299 项自测 + `fast-srp-hap` 交叉验证；
> - **第二阶段 a（TLV8 与配对状态机）** —— 80 项自测 + **Python 独立实现的交叉验证**；
> - **第二阶段 b（HAP 请求/响应层）** —— 125 项自测 + `hap_session_ref.py` 双向交叉验证；
> - **第三阶段（平台层）** —— 69 项自测 + 宿主语法自检 + SDK 符号核对；
> - **第四阶段（配对码进 Web + 事件推送）** —— 39 项自测；配对码首次启动随机生成、
>   可在设置页查看/修改/一键换码，继电器变化时主动推给已订阅的 iOS。
>
> 四个阶段的代码都写完了，但**整套从未在真机上跑过、也没和真实 iPhone 配过对**，
> 详见第 9 节。

---

## 1. 为什么可以不用 MFi 芯片

HAP 的两条认证路径：

| 路径 | 需要 MFi 协处理器 | 说明 |
|---|---|---|
| MFi 硬件认证 | 需要（i2c 上的专用芯片） | 商业产品必须走这条 |
| **软件认证 / 无认证** | **不需要** | Homebridge、HomeSpan 等都用这条；配对码照样有效 |

我们走第二条。密码学全部用固件自己实现，**不依赖任何 Apple 提供的密钥或芯片**。

---

## 2. 阶段划分与当前进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| **一** | **密码学内核**：SHA-512 / HMAC / HKDF、ChaCha20-Poly1305、X25519、Ed25519、SRP-6a 3072 位 | ✅ **已完成**（299 项自测 + `fast-srp-hap` 交叉验证） |
| **二 a** | **协议层内核**：TLV8 编解码、Pair Setup M1~M6、Pair Verify M1~M4 | ✅ **已完成**（80 项自测 + `hap_ref.py` 交叉验证） |
| 二 b | **HAP 请求/响应层**：会话帧加解密、HTTP、JSON、`/accessories`、`/characteristics`、6 路配件数据库 | ✅ **已完成**（125 项自测 + `hap_session_ref.py` 双向交叉验证） |
| **三** | **平台层**：lwIP TCP 监听 51826、mDNS 广播 `_hap._tcp`、配对信息持久化到 userbin、TRNG | ✅ **已完成**（69 项自测 + 宿主语法自检 + SDK 符号核对） |
| 四 | 事件推送（`ev`）与状态上报；接入 Web 后台 | ✅ **已完成**（配对码进了设置页 + 继电器变化主动推；见 6d） |

之所以把密码学放第一阶段单独做：它是**唯一能在没有硬件、没有交叉工具链的机器上
被完整验证**的部分。如果它错了，表现是"配对时 iOS 默默失败"，现场几乎无法排查。
先把它钉死，后面出问题时就能确定地排除掉整类原因。

---

## 3. 第一阶段：密码学内核

### 3.1 文件清单

全部位于 `src/homekit/`，**只依赖 C99 标准库，不引用任何 SDK 头文件**，
因此同一份源码既能编进固件，也能在本机用 gcc 编起来跑官方向量。

| 文件 | 内容 | 用途 |
|---|---|---|
| `hk_sha512.c/.h` | SHA-512（FIPS 180-4，含流式 update） | SRP 的哈希、HKDF、Ed25519 内部 |
| `hk_kdf.c/.h` | HMAC-SHA512（RFC 2104）、HKDF-SHA512（RFC 5869） | SRP 的 k/x、Pair Verify 的会话密钥派生 |
| `hk_chachapoly.c/.h` | ChaCha20、Poly1305、AEAD（RFC 8439） | 配对阶段的 TLV 加密、会话帧加密 |
| `hk_gf.c/.h` | GF(2^255-19) 域运算（16×16 位肢体） | X25519 与 Ed25519 **共用**一份，省 flash |
| `hk_curve25519.c/.h` | X25519（RFC 7748） | Pair Verify 的密钥协商 |
| `hk_ed25519.c/.h` | Ed25519 签名/验签（RFC 8032） | 配件证明自己的身份（LTSK/LTPK） |
| `hk_bignum.c/.h` | 3072 位大数 + Montgomery CIOS 模幂 | SRP-6a 的模幂 |
| `hk_srp.c/.h` | SRP-6a（3072 位群 + SHA-512） | Pair Setup 的核心 |
| `hk_srp_group.h` | 3072 位群参数（自动生成） | 见第 4 节 |

### 3.2 为什么不用 SDK 里现成的 mbedTLS

`libLPBX3Kernel.a` 里**确实打包了 mbedTLS**（`mbedtls_sha512_*`、`mbedtls_md_hmac_*`、
`mbedtls_mpi_exp_mod`、`ecp/ecdh`、硬件 TRNG 都有符号，见 `doc/WEB_BL0937.md` 第 11 节）。
但有一个致命障碍：

> **SDK 不提供 mbedTLS 的头文件，也不提供 mbedTLS 的编译配置。**
> `sdk/4.14/include/mbed_config.h` 里只有 `MBED_CONF_LWIP_*` / `MBED_CONF_RTOS_*`
> 这些 mbed-os 的项，**一个 `MBEDTLS_*` 都没有**。

看不到 `MBEDTLS_MPI_MAX_SIZE`、`MBEDTLS_ECP_MAX_BITS` 就等于不知道 `mbedtls_mpi`
结构体多大、`exp_mod` 支不支持 3072 位。硬调它是在赌内存布局 —— 赌错的后果是
随机崩溃，而且极难定位。

自己写一份还有额外好处：**能在 PC 上对着 Python 逐字节验证**。

### 3.3 ⚠️ 一个必须记住的差异：SHA-1 vs SHA-512

| 来源 | 哈希 |
|---|---|
| RFC 5054（SRP 的标准） | **SHA-1** |
| **HomeKit 规定** | **SHA-512** |

本实现用的是 **SHA-512**。也就是说 **RFC 5054 附录 B 的测试向量不能拿来对**
（那组向量是 SHA-1 + 1024 位群），只能用来核对**群参数**。这一点写在这里，
免得以后有人"顺手改成 RFC 5054 的向量"然后发现对不上。

### 3.4 ⚠️ M1 的 H(g) 用 g 的最短表示，k 里的 PAD(g) 才补零

两处对 g 的哈希**约定不同**，写错的表现是 iOS 配对时报 M1 校验失败：

| 用途 | 公式 | g 的表示 |
|---|---|---|
| `k = H(N \| PAD(g))` | `srp_k()` | **补零到 384 字节** |
| `H(N) XOR H(g)`（进 M1） | `hk_srp_m1()` | **最短大端表示**（g=5 即单字节 `0x05`） |

依据：`fast-srp-hap`（HAP-NodeJS 依赖）源码里 `getM1()` 用 `params.g.toBuffer(true)`
（jsbn 的 `toBuffer(true)` = 最短大端），而 `getk()` 用 `padToN(params.g)`。
三个独立实现（fast-srp-hap / HAP-NodeJS / esp-homekit）一致如此。

---

## 4. 向量是怎么保证可信的（方法论）

自研密码学最容易掉进的坑是：**实现在自洽地错**。如果测试向量也是自己边写边算的，
两边一起错，测出来还是一片绿。

所以 `tools/hk_vectors_gen.py` 的做法是**先自证预言机，再生成向量**：

```
第一步  用公开已知值校验预言机本身（Python hashlib/hmac，底层 OpenSSL）
        - NIST FIPS 180-4 的 SHA-512 向量           3 项
        - RFC 4231 的 HMAC-SHA512 向量（含超块长密钥） 3 项
        任何一项对不上 -> 直接退出，拒绝生成任何向量

第三步  自写 ChaCha20/Poly1305 参考实现后，先拿 RFC 8439 的公开向量钉住   5 项
第七步  Ed25519 参考实现先拿 RFC 8032 §7.1 的 TEST 1/2/3 钉住           9 项
第五步  X25519 参考实现先拿 RFC 7748 §5.2/§6.1/迭代向量钉住              7 项
第九步  SRP 群参数做**素性证明**：
        - N 恰好 3072 位
        - N 通过 Miller-Rabin 32 轮
        - (N-1)/2 也是素数（安全素数）
        - g=5 满足 g^(N-1) mod N == 1
第十步 SRP 端到端：断言客户端与服务端**独立算出的 S 完全一致**
```

也就是说：**预言机先被公开向量钉死，然后才被用来生成我们自己的向量**。
`hk_srp_group.h`（384 字节的素数）也是脚本生成的，我一个字都没有手抄 ——
抄错一个十六进制字符，后果是"配对永远失败"且没有任何提示。

重新生成：

```sh
python tools/hk_vectors_gen.py
```

---

## 5. 怎么跑自测

```sh
sh tools/build_hk_selftest.sh                 # 内部一致性（683 项）
PYTHON=python3 sh tools/build_pair_crosscheck.sh       # 配对层：C <-> Python
PYTHON=python3 sh tools/build_hap_crosscheck.sh        # 会话层：C <-> Python（双向）
```

- 不需要交叉工具链、不需要硬件、不需要 mbedTLS，只要一个宿主 gcc。
- 三个脚本退出码非 0 都表示有失败项，可以直接接进 CI。
- `SKIP_RUN=1 sh tools/build_hk_selftest.sh` 只编译不运行。
- 交叉验证脚本需要 Python 的 `cryptography`（`pip install cryptography`）。

> **顺序建议**：自测先跑（快），交叉验证后跑（慢，但只有它能证明"符合 HAP"）。
> 两者不是替代关系 —— 二 b 阶段的三个真 bug 都是自测先绿、交叉验证才抓出来的。

### 5.1 覆盖范围（683 项）

| 分组 | 项数 | 重点 |
|---|---|---|
| SHA-512 | 41 | NIST 向量；**111/112/113、127/128/129** 字节的补位边界；1/7/13 字节分块喂入必须与一次喂一致；零长度 update |
| HMAC-SHA512 | 18 | RFC 4231 六组（含密钥 > 块长）；**密钥 128/129 字节** 边界；空密钥；分块喂 |
| HKDF-SHA512 | 14 | salt=NULL 等价全零盐；**255 块上限**（16320 字节）实测；超限必须返回 -1 |
| ChaCha20 块函数 | 4 | RFC 8439 §2.3.2；ctr=0xffffffff |
| ChaCha20 流加密 | 14 | **0/1/63/64/65/200 字节**的块边界；加密后必须能还原 |
| Poly1305 | 40 | **0/1/15/16/17/31/32/33 字节**的长度边界；1/7/16 字节分块喂 |
| AEAD | 56 | RFC 8439 §2.8.2 官方向量；aad/pt 为空的组合；**原地加解密**；篡改密文/tag/aad/nonce/key/截断共 6 种必须全部被拒，且**拒绝时不得输出任何明文** |
| X25519 | 17 | RFC 7748 §5.2/§6.1；**迭代 1000 次**（放大进位误差）；小阶点；out 与 scalar/point 重叠 |
| Ed25519 | 46 | RFC 8032 TEST 1/2/3 的公钥+签名+验签；空消息与长消息；篡改 R/S/公钥/消息共 5 种拒绝；**S >= L 的畸形签名必须被拒**（否则签名可锻造） |
| 大数基础 | 12 | 大端往返；跨肢边界 2^32-1 / 2^32；固件群常量与向量一致 |
| Montgomery 模幂 | 10 | 对照 Python `pow()`：指数 0/1、底数 0/1、2^256、(N-1)^2、1024 位指数、**3072 位指数的费马小定理** |
| SRP-6a | 31 | x/v/A/B/u/S/K/M1/M2 全部对照 Python；**两端 S/K 逐字节相同**；配对码错误的 5 项识别；退化输入拒绝 |
| TLV8 | 50 | **384 字节公钥必然跨 2 片**，拼接后必须逐字节一致；254/255/256 边界；网络畸形数据（length 越界、残尾）必须拒绝；**缓冲不足时"一点都不写"**；200 轮随机组合往返 |
| Pair Setup / Verify | 30 | 控制器与配件都由本仓代码扮演，跑完 M1~M6 + Verify M1~M4；配件签名能被控制器验过；**两端会话密钥逐字节相同且两个方向不同**；配对码错/签名篡改/状态乱序/未配对 4 类负例 |
| HAP 会话层 | 125 | 帧：长度预算、**3000 字节 3 帧重组**、半包（消费 0 字节且不推进计数器）、改 AAD/密文/tag/用反方向密钥 4 类必须失败、**跳帧必须失败**（计数器生效）、输出缓冲不足时不写；HTTP：GET/PUT/query/小写头名/头不全/body 短于 Content-Length；JSON：逗号、转义、负数、**溢出后拒绝而非截断**、未知键跳过、条数超 max 截断；配件模型：iid 映射、只写特性不带 value、写只读回 -70402、未知 iid 回 -70407；端点：未验证 → 401、`/pair-setup` state=1 回 state=2、`/identify` → 204 且回调触发、会话建立后 `/accessories` → 200 |
| 持久化格式 | 71 | magic/版本/长度/CRC 任一不对都拒绝且**保持原结构不变**；CRC 覆盖含 magic 的整段；npeers 越界拒绝；ID 非 '\0' 结尾时截断而非越界；序列化→反序列化往返一致；新身份生成后 LTPK 与 LTSK 匹配；**配对码往返一致、且改它会被 CRC 抓住** |
| **网络层（三）** | **69** | 订阅表幂等/满/删后再加；mDNS TXT 的 `sf` 翻转、**不超 255**、缓冲不足返回 0 而非截断；**明文半包**（产出 0 字节且留在 rx）；**粘包**（产出长度必须**正好**是单段 2 倍）；**加密切换时序**（响应仍是明文、之后才 `encrypted=1`）；密文往返解回 `/accessories` JSON；**密文半包**（计数器不推进、补齐后只推进 1 次）；加密阶段垃圾数据 → FATAL 且 dead 后持续拒绝 |
| **配对码（四）** | **39** | 三条禁用规则各就各的测（首位 0/1、八位全同、两个顺子，且两种写法都堵）；格式（长度/非数字/分隔符错）；归一化把各种输入收敛成唯一排版，且**不替用户判合法**；随机生成后自身必须能过校验（200 个种子）、**恒定坏随机源下绝不产出非法码** |

### 5.2 特意设计的三类"上板才会暴露"的测试

向量测试测不出这些，但它们在真机上一定会遇到：

1. **流式喂入 vs 一次性喂入**
   网络层的帧是一段段收到的。如果只有"一次性调用"是对的，向量测试全绿、
   上了板子却莫名其妙失败。所以每个哈希/MAC 都额外测 1/5/7/13/16 字节分块喂入。

2. **原地加解密**
   嵌入式里为省 RAM 一定会原地做（`pt` 和 `ct` 是同一块内存）。
   AEAD 对全部 7 组向量都测了原地路径，并要求结果与分离缓冲逐字节一致。

3. **别名与重复调用**
   X25519 测了 `out` 与 `scalar` 重叠、`out` 与 `point` 重叠、以及重复调用结果稳定
   （用于暴露未初始化状态）。

### 5.3 外部参考实现交叉验证（终极检验）

向量全部生成之后，还用 **HomeKit 官方生态的参考实现 `fast-srp-hap`**
（HAP-NodeJS 实际依赖的包）把完整客户端流程跑了一遍：固定 salt 与私钥 a，
取它的 `computeA/computeK/computeM1/M2` 与 `hk_vectors.h` 里的向量逐字节比对：

```sh
# 先装参考实现（Node 任意版本）
cd build/refjs && npm install fast-srp-hap
# 跑比对
node tools/crosscheck_srp.js tools build/refjs/node_modules
```

实测输出：**A / K / M1 / M2 四项全部一致**。

踩过的两个坑（都写在脚本注释里）：

1. **必须用 `params['hap']`，不能用 `params['3072']`** —— 后者是 RFC 5054 原版
   （同一个 N、同一个 g=5，但 hash 是 **sha256**），只有 `hap` 组才是 sha512。
   用错组的现象很迷惑：A 一致（数学相同），u/K/M1 全部不一致（摘要不同）。
2. 参考实现会对测试私钥报 `client key length ... is less than the recommended
   256 bits` 警告 —— 我们向量里的 a 有效位不足 256 位，是提示不是错误。

---

---

## 6. 第二阶段 a：TLV8 与配对状态机

### 6.1 文件清单

| 文件 | 内容 | 用途 |
|---|---|---|
| `hk_tlv.c/.h` | TLV8 编解码：自动分片/合并、越界检查、原子写入 | 配对阶段所有报文的"信封" |
| `hk_pair.c/.h` | Pair Setup M1~M6、Pair Verify M1~M4 | HomeKit 的进门流程 |

两者同样只依赖 C99 标准库。**随机数由调用方注入**（`salt` / SRP 的 `b` / 临时 X25519 私钥
都是函数参数），因此能在 PC 上用固定值完整复现；固件侧填硬件 TRNG，配对记录的持久化
也交给调用方（本层只把结果留在结构里，不写 flash）。

### 6.2 依据：HAP-NodeJS 源码，不是 RFC

HAP 的配对流程没有公开的完整 RFC，能拿到的权威依据就是生态里跑得最多的实现。
本阶段逐行核对过 **HAP-NodeJS（Homebridge 的实现）**：

| 步骤 | 参考位置 |
|---|---|
| Pair Setup M1/M2（salt 16 字节、user="Pair-Setup"） | `HAPServer.ts` `handlePairSetupM1` |
| M3/M4（checkM1 / computeM2） | `handlePairSetupM3` |
| M5（解密 + 验控制器签名） | `handlePairSetupM5` / `handlePairSetupM5_2` |
| M6（配件签名 + 加密） | `handlePairSetupM5_3` |
| Verify M1/M2、M3/M4、会话密钥派生 | `handlePairVerifyM1` / `handlePairVerifyM3` |
| TLV 类型取值 | `internal-types.ts` `enum TLVValues` |

核对过程中**纠正了两处原本凭记忆写错的地方**（见 6.3、6.4），这两处如果错了，
C 侧自测照样全绿 —— 因为控制器和配件都是本仓代码写的，会错得一样。

### 6.3 ⚠️ 两个"证明"类型：0x04 和 0x0A

HAP 里有两个都叫 "proof" 的类型，含义完全不同：

| 类型 | 值 | 放什么 | 出现在哪 |
|---|---|---|---|
| `PASSWORD_PROOF` | **0x04** | SRP 的 M1/M2（64 字节哈希） | 仅 Pair Setup 的 M3 / M4 |
| `SIGNATURE`（也叫 PROOF） | **0x0A** | Ed25519 签名（64 字节） | M5 / M6、Verify 的 M2 / M3 |

> **这个 bug 就是本次实际踩到的**：第一版 `hk_pair.c` 在 M5/M6 和 Verify 里统一用了 0x04，
> 379 项自测**全部通过**（因为测试里的控制器也用 0x04），是 `tools/hap_ref.py` 的
> 独立实现把它抓出来的 —— Python 侧按规范用 0x0A，解不开 C 侧的报文才暴露。
> 真机上的表现会是"iOS 配对走到最后一步静默失败"。

### 6.4 ⚠️ 签名内容：M5/M6 带 HKDF 派生值，Verify 不带

最容易记错的一处。签名对象不是"ID || 公钥"，而是：

| 步骤 | 签名者 | 被签名的内容（按顺序拼接） |
|---|---|---|
| Setup M5 | 控制器（配件验） | `HKDF(K, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", 32)` ‖ iOS 配对ID ‖ iOS LTPK |
| Setup M6 | 配件 | `HKDF(K, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", 32)` ‖ 配件ID ‖ 配件LTPK |
| Verify M2 | 配件 | 配件临时公钥 ‖ 配件ID ‖ 控制器临时公钥 —— **没有 HKDF 前缀** |
| Verify M3 | 控制器（配件验） | 控制器临时公钥 ‖ iOS 配对ID ‖ 配件临时公钥 —— **没有 HKDF 前缀** |

漏掉那个 32 字节派生值（或反过来多拼了）都会让对方验签失败。

### 6.5 加密参数一览

| 用途 | HKDF 的 salt / info（ikm 见下） | nonce（4 零 + 8 ASCII） |
|---|---|---|
| Setup 报文加密 | `Pair-Setup-Encrypt-Salt` / `Pair-Setup-Encrypt-Info` | `PS-Msg05` / `PS-Msg06` |
| Verify 报文加密 | `Pair-Verify-Encrypt-Salt` / `Pair-Verify-Encrypt-Info` | `PV-Msg02` / `PV-Msg03` |
| 会话密钥（配件→控制器） | `Control-Salt` / `Control-Read-Encryption-Key` | — |
| 会话密钥（控制器→配件） | `Control-Salt` / `Control-Write-Encryption-Key` | — |

- Setup 的 ikm 是 **K = H(S)**（64 字节），不是 S；Verify 的 ikm 是 X25519 共享秘密（32 字节）。
- AEAD 的 **aad 为空**。
- `encryptedData` 的格式是 **密文 ‖ 16 字节 tag**（tag 直接拼在后面）。
- 会话密钥的 info 是站在**控制器**视角命名的，所以"Read"那把是**配件发送**用的
  （已在 6.2 的参考实现里确认，别按字面反过来）。

### 6.6 交叉验证：tools/hap_ref.py

C 侧自测是"自己对自己"，所以额外准备了一份**完全独立**的 Python 实现：
用 `hashlib`/`hmac` 自己拼 HKDF，用 `cryptography`（OpenSSL）做 ChaCha20-Poly1305 /
X25519 / Ed25519，配件侧重算一遍，与 C 的输出逐字节比对。

```sh
sh tools/build_pair_crosscheck.sh          # 编 C 侧转储 -> 跑 Python 比对
PYTHON=/path/to/python sh tools/build_pair_crosscheck.sh
```

流程：`tools/hk_pair_dump.c` 把 C 侧的关键值（B / K / M2 / M6 密文 / Verify 公钥与密文 /
两个方向的会话密钥）转储成 JSON，`hap_ref.py` 独立复算并比对。当前 **10 项全部一致**。

它只共享两样非算法的东西，而且都是**解析源文件得到**而非手抄：
固定输入向量（`tools/hk_pair_vec.h`）与 SRP 群参数 N（`hk_srp_group.h`）。

### 6.7 这一层的内存账

| 结构 | RAM | 说明 |
|---|---|---|
| `HK_SETUP` | ≈ 1.3 KB | `v`/`B`/`S` 各 384 字节 + K 64 + salt 16 + b 32 |
| `HK_VERIFY` | ≈ 200 B | 临时密钥对、共享秘密、两把会话密钥 |

`B` 和 `S` 没合并成一个缓冲是有原因的：M3 要先算 `u=H(A|B)`，再算 `S=(A·v^u)^b` 得到
`K=H(S)`，**然后**算 M1 —— 而 M1 里还要用 `B`。两者的生命周期是重叠的。

---

## 6b. 第二阶段 b：HAP 会话层（帧 / HTTP / JSON / 配件模型）

### 6b.1 文件清单

| 文件 | 职责 |
|---|---|
| `src/homekit/hk_hap.c/.h` | 会话帧加解密、极简 HTTP 解析与响应构造、端点分发 |
| `src/homekit/hk_json.c/.h` | 极简 JSON 生成器 + **受限**扫描器（只认 aid/iid/value/ev） |
| `src/homekit/hk_acc.c/.h` | 6 路排插的配件数据库 + `/accessories`、`/characteristics` 渲染 |
| `tools/hk_test_hap.c` | 主机自测（125 项） |
| `tools/hk_hap_dump.c` | 交叉验证用的转储/反向校验程序 |
| `tools/hap_session_ref.py` | Python + OpenSSL 独立实现的分帧加解密 |
| `tools/build_hap_crosscheck.sh` | 一键跑完双向交叉验证 |

### 6b.2 ⚠️ 帧加密的三条硬规则（都对 HAP-NodeJS `hapCrypto.ts` 核对过）

| 项 | 值 | 记错会怎样 |
|---|---|---|
| **AAD** | 那 2 字节的长度头（小端），**不是空** | 配对阶段的 aad 才是空，混用 = 第一帧就认证失败 |
| **nonce** | 4 个零字节 + 8 字节**小端计数器**，每帧 +1，**两个方向各数各的** | 计数器错位 = 第一帧之后全废 |
| **分片** | 单帧明文上限 `0x400` = 1024，超了切多帧，每帧自带长度头+密文+tag | `/accessories` 响应 1766 字节必然是 2 帧 |

一帧的布局：`[2B 明文长度][密文][16B tag]`。

### 6b.3 配件数据库与 iid 分配

iid 用**固定规律**生成，不随机、不随重启变化 —— iOS 会缓存实体，iid 变了就对不上。

| iid | 含义 |
|---|---|
| 1 | AccessoryInformation 服务（`0x3E`） |
| 2 | Identify（`0x14`，只写，**不带 value**） |
| 3/4/5/6/7 | Manufacturer / Model / Name / SerialNumber / FirmwareRevision |
| `10+10n` | 第 n 路 Outlet 服务（`0x47`），n=0..5 |
| `11+10n` | 第 n 路的 Name（`0x23`，只读） |
| `12+10n` | 第 n 路的 **On**（`0x25`，pr+pw+ev，bool） |

On 的 iid 依次是 **12 / 22 / 32 / 42 / 52 / 62**。第一个 Outlet 标了 `primary`
（HAP 要求每个配件恰好有一个主服务）。

动态值（On）走 `HK_ACC_IO` 回调，本层不直接碰 GPIO —— 第三阶段接 `user_relay` 时不用改这里。

### 6b.4 端点

| 端点 | 加密 | 说明 |
|---|---|---|
| `POST /pair-setup` | 否 | TLV8，M1~M6 |
| `POST /pair-verify` | 否 | TLV8，M1~M4 |
| `POST /pairings` | 是 | 未实现，回 `[state=2][error=0x06 unavailable]` |
| `POST /identify` | 否 | 触发 `io->identify()`，回 204 |
| `GET /accessories` | 是 | 配件数据库 JSON |
| `GET /characteristics?id=1.12,...` | 是 | 读 |
| `PUT /characteristics` | 是 | 写 / 事件订阅（`ev`） |

没有会话时访问加密端点 → `401` + `{"status":-70401}`。

### 6b.5 这一层踩到的四个坑（自测全绿但其实是错的）

1. **JSON 生成器的值函数忘了补逗号** —— `hk_json_str/int/bool` 漏了 `jpre()`。
   对象里的逗号是靠 `hk_json_key()` 补的，所以 `{"a":1,"b":2}` 看着正常，
   **只有数组才会露馅**：`["pr""pw"]`（少了逗号）。
2. **受限 JSON 解析器把目标数组吞了** —— 遇到不认识的键时按通用解析器的做法
   把整个值跳过，结果 `{"characteristics":[...]}` 的 `"characteristics"` 正好是
   "不认识的键"，整个数组被吞掉 → 解析出 0 条。
   修法：**只在值是标量时才跳过**，遇到 `{` 或 `[` 留给主循环处理。
3. **`/accessories` 的 value 缺键名** —— 输出成了 `"format":"string","PHICOMM"`。
   修法是让 `put_char_value()` 连键一起写，并且**取不到值时一个字节都不写**
   （否则会留下半个 `"value":` 变成非法 JSON）。
4. **交叉验证两侧的"计数器"没对齐** —— C 侧一个会话连续加密 5 个样本，
   nonce 里的计数器一直累加；Python 侧每个样本都从 0 开始复算。
   表现是**只有第一个样本对得上**，很容易被误判成"分片有 bug"。

> 1~3 都是 C 自测**先绿后红**的典型：自测只验证了"形状像"，
> 真正判它们的是 `hap_session_ref.py` 与 Python 的 `json.loads`。

### 6b.6 交叉验证怎么做的（`sh tools/build_hap_crosscheck.sh`）

| 方向 | 做法 | 能证明什么 |
|---|---|---|
| C → Python | Python 独立实现解 C 的帧，比对明文 | C 的帧格式被外部认可 |
| Python → C（逐字节） | Python 用同一密钥重新加密，与 C 的帧**逐字节相同** | nonce / aad / 分片只要差一点，密文就完全不同 —— 这是最硬的一条 |
| Python → C | Python 用 c2a 密钥加密，C 的 `hk_frame_decrypt` 解 | 反向也通 |
| 负例 | 改长度头 / 跳帧 | AAD 与计数器确实在起作用 |
| 结构 | Python `json.loads` 校验 `/accessories` | JSON 合法且字段齐全 |

**33 项全绿**。

---

## 6c. 第三阶段：平台层（TCP 51826 / mDNS / 持久化 / TRNG）

### 6c.1 文件清单

| 文件 | 职责 |
|---|---|
| `src/homekit/hk_net.h` | 接口。用 `HK_NET_PLATFORM` 把"纯逻辑"与"平台胶水"分开 |
| `src/homekit/hk_net.c` | 实现。前半不 include 任何 SDK 头（主机可测），后半才是 lwIP / mDNS / userbin |
| `tools/hk_test_net.c` | 主机自测 69 项 |
| `src/Makefile`、`projects/LPT230_gcc.uvprojx` | 已加入构建（15 个 .c） |
| `src/app_main.c` | 加了 `#ifdef HK_ENABLE` 的 `hk_net_start()` 调用点 |

**为什么必须分成两半**：这一层要碰的东西（TCP 粘包、加密切换时序、半包）恰恰是
真机上最难查的 —— 表现全是"iOS 默默失败"，没有任何日志。把它们做成不认识
socket 的纯函数，就能在 PC 上钉死。

### 6c.2 SDK 能力核对结果（都是查过库符号的，不是猜的）

| 能力 | API | 核对方式 |
|---|---|---|
| TCP 监听 | `lwip_socket/bind/listen/accept/recv/send/close` | `nm` 查 `libLPBX3Kernel.a`，全部 `T` |
| 字节序 | `lwip_htons/lwip_htonl` | **`lwip/def.h` 里是宏**，不是函数，不用链接 |
| mDNS | `mdns_start`、`mdns_announce_service`、`mdns_set_txt_rec` | `nm` 全部 `T` |
| userbin | `hffile_userbin_read/write` | `nm` 是 **`W`（weak）**，库里有定义，可链接 |
| 真随机数 | `rda_trng_get_bytes`（优先）、`hfsys_random`（退化） | `nm` 全部 `T` |
| 线程 | `hfthread_create`、`hf_thread_delay` | `nm` 全部 `T` |
| MAC | `hfnet_get_mac_address` | `nm` 为 `T` |

两个**与早期文档记录不符**、必须记住的事实：

1. **`MEMP_NUM_TCP_PCB` 是 4，不是 12。** `lwipopts.h:127` 把它写成
   `MBED_CONF_LWIP_TCP_SOCKET_MAX`，而 opt.h 的默认 5 也被覆盖。
   一个 listening PCB + 一个连接就占掉 2 个，httpd 还要用 ——
   所以本层刻意只支持**一条**并发 HAP 连接，多了直接拒。
2. **`LWIP_COMPAT_SOCKETS` 默认是 1**（`lwip/opt.h:1756`），所以 `socket()` 这类
   简名其实能用。但代码里一律写 `lwip_*` 显式名，免得被别的宏污染。

### 6c.3 ⚠️ 五个坑（都写进 hk_net.h 开头了）

| # | 坑 | 写错的表现 |
|---|---|---|
| 1 | **加密切换点是 Pair Verify M4 的响应发出之后**，不是 M2 之后 | 早切一帧 → iOS 发来的 M3 被当密文解 → 直接断连 |
| 2 | **TCP 是字节流**：一次 recv 可能半个请求，也可能两个请求 | 丢请求 / 卡死等 body |
| 3 | **半包不能推进计数器**：`hk_frame_decrypt` 没消费的字节必须原样留着 | nonce 错位 → 之后每一帧都认证失败，"连上就掉" |
| 4 | **PCB 只有 4 个** | 多开连接会拖垮 httpd |
| 5 | **LTSK 丢了就彻底连不上**，且 iOS 侧的报错毫无提示 | 必须"存不下就不启动"，而不是带病运行 |

判据是 `HK_SESSION.up` —— 它由 `hk_session_begin()` 置 1，而 `hk_hap.c` 只在
处理完 `/pair-verify` 的 `state=3` 时才调它。

### 6c.4 持久化放在哪

userbin 共 4KB，已被占用的情况：

| 区间 | 内容 |
|---|---|
| 0 ~ 1300 | MQTT 配置 |
| 1316 / 1356 起 | BL0937 校准与电量 |
| 2048 ~ 2048+`sizeof(PLUG_CONFIG)` | 排插配置 |
| **紧接着的 424 字节** → | **HomeKit（本阶段新增）** |
| **2864 之后** | **SDK 的 MQTT 备份区，绝对不能写** |

`2864` 不是拍脑袋：`hfflashlayout.h` 里
`USER_BIN_FILE_MQTT_OFFSET - USER_BIN_FILE_OFFSET = 0xB30 = 2864`。

代码用表达式而不是常量，并加了编译期护栏：

```c
#define HK_STORE_USERBIN_ADDR (PLUG_CONFIG_USERBIN_ADDR + sizeof(PLUG_CONFIG) + 8)
typedef char hk_net_layout_ok[(HK_STORE_USERBIN_ADDR + HK_STORE_BLOB_MAX <= HK_USERBIN_SDK_LIMIT) ? 1 : -1];
```

用 `sizeof(PLUG_CONFIG)` 是因为它在 10/1（188B）和 10/2（192B）两个版本里不一样，
写死就会踩到别人。

### 6c.5 自测 69 项（`sh tools/build_hk_selftest.sh`）

| 分组 | 项数 | 重点 |
|---|---|---|
| 订阅表 | 11 | 幂等新增、表满、删除后再加 |
| mDNS TXT | 12 | `sf` 随配对状态翻转、**不超过 255**、缓冲不足返回 0 而非截断 |
| 明文阶段 | 8 | `identify` 回调被触发、响应确实是明文 |
| 半包（明文） | 6 | 半个请求产出 0 字节且留在 rx |
| 粘包 | 6 | **产出长度必须正好是单段的 2 倍**（少=丢请求，多=重复处理） |
| 加密切换时序 | 6 | 响应仍是明文、**之后**才 `encrypted=1` |
| 加密阶段 | 8 | 解回响应确认真的是 `/accessories` JSON |
| 半包（密文） | 8 | **计数器不推进**、补齐后只推进 1 次 |
| 错误处理 | 4 | 加密阶段垃圾数据 → FATAL，且 dead 后持续拒绝 |

> 全仓现在是 **683 项**（密码学 299 + 协议 205 + 持久化 71 + 网络 69 + 配对码 39）。

### 6c.6 怎么启用

`HK_ENABLE` 必须**全局**给，不能只给 `app_main.c`（它同时影响三个文件，见下）：

```sh
make HK_FLAGS=-DHK_ENABLE      # src/Makefile 里已留好这个变量
```

| 文件 | 受 HK_ENABLE 影响的地方 |
|---|---|
| `app_main.c` | 是否调 `hk_net_start()` |
| `user_web.c` | 设置页是否显示/可改配对码 |
| `user_gpio.c` | 继电器变化时是否调 `hk_net_notify()` |

只给 `app_main.c` 的后果是：服务起了，但 Web 页显示"未启用"、按键改状态也不推送。
`app_main.c` 的调用点在 `hfnet_wifi_is_active()` **之后** —— `mdns.h` 明确要求
`mdns_start()` 只能在网络栈起来之后调用。

---

## 6d. 第四阶段：配对码进 Web + 事件推送

### 6d.1 为什么配对码必须能在网页上看到

排插**没有屏幕**，HomeKit 的配对码是"配对时唯一要人输入的东西"，
没有它整套功能等于不可用。所以这一阶段先解决码的问题，再解决推送。

顺带修掉一个**会让设备永远配不上对**的 bug：原来的默认码写的是
`HK_SETUP_CODE "123-45-678"`，而这正是 HAP 规范**明令禁用**的两个顺子之一
（另一个是 `876-54-321`）。用了它的表现是 iOS 在输入配对码那一步直接拒绝，
且不给任何有用的报错 —— 现场只会看到"配对失败"。

规则抽成了独立模块 `hk_code.c`，三条硬规则：

1. 第一位不能是 `0` 或 `1`；
2. 八位数字全部相同的码一律禁用（`000-00-000` … `999-99-999`）；
3. 两个顺子单独禁用（`123-45-678`、`876-54-321`）。

现在的行为是 **首次启动用硬件 TRNG 现生成一个合法码并落盘**，之后可在 Web 后台改。
生成用拒绝采样保证均匀（直接对 256 取模会让小数字概率偏高）。

### 6d.2 ★★ 改配对码 = 恢复出厂，这个语义不能搞错

HAP 的 SRP 里配对码参与派生 salt/verifier，但**一旦配对成功，之后的连接走
Pair Verify（长期公钥），不再校验配对码**。所以：

> 只改码、不清理已配对列表 ⇒ 旧 iPhone 照样能连，码却变了。
> 用户以为"改了码别人就进不来"，实际没有。

所以 `hk_net_set_code()` 的实现是**整个 store 重置**（含 LTSK/LTPK），
语义等价于恢复出厂。这条写在 `hk_net.h` 的声明处，别在后续改动中把它"优化"掉。

另外有个容易踩的实现细节：Web 的"保存"按钮会把当前页面上的配对码原样回传，
**必须和当前值比较、真的变了才调 `hk_net_set_code()`** ——
否则用户每保存一次 MQTT 设置，HomeKit 就被恢复出厂一次。

### 6d.3 事件推送怎么接的

```c
/* user_gpio.c: user_relay_set() 里 */
hk_net_notify(x, y ? 1 : 0);   /* 只置位掩码，立即返回 */
```

`hk_net_notify()` **不碰网络**，这是硬要求：它会被按键中断回调、MQTT 回调、
httpd 回调共同调用，而 httpd 只有一个约 1KB 的缓冲和单线程，
在里面阻塞等网络会把整个 Web 后台拖死。真正的发送由 HAP 线程轮询完成。

| 设计点 | 原因 |
|---|---|
| 用**位掩码**而不是队列 | 同一路连续扳两次只推最后一次。HAP 事件本就是"通知变化"，中间态丢了没关系，最终一致即可 —— 连缓冲都不用开 |
| 一次只推一个 characteristic | 6 路的 On 是 iid 12/22/…/62，一次只可能一路变。HAP 允许多条一起推，但只要其中一条 iid 写错**整批**都会被 iOS 丢弃 |
| 只推**已订阅**的 iid | 没订阅就推，iOS 会当成未知特性 |
| 事件体**不带 `status` 字段** | 那是 `/characteristics` **响应**用的。带上它 iOS 会拿去匹配请求号，表现为"开关状态不刷新" |
| `recv` 用 `MSG_DONTWAIT` 轮询 | 这个 SDK 的 lwIP **没有暴露 errno/EAGAIN**（`sockets.h` 里压根没有 `EAGAIN` 宏），没法区分"超时"和"真出错"。改成非阻塞 + 主动 delay，既绕开 errno，又能周期性回头看有没有事件要推 |

### 6d.4 Web 侧改动与体积

设置页加了 HomeKit 一栏（配对码输入框 + 状态），`/api` 加了：

| 接口 | 用途 |
|---|---|
| `act=cg` | 回传新增的 `hk`（当前配对码）与 `hkst`（已配对/未配对/未启用） |
| `act=cfg&hc=` | 保存配对码（只在真的变了才落盘） |
| `act=hkg` | 重新生成一个随机配对码 |

⚠️ **体积是这里最紧的约束**：httpd 的响应缓冲约 1KB，工程惯例是单片 <950 字节，
而 UTF-8 下一个汉字占 **3 字节**，长提示句非常贵。加了 HomeKit 一栏后
`/u.js` 一度冲到 1058 字节，处理办法是：

1. 把 HomeKit 的 JS 拆成独立分片 **`/h.js`**（跟校准页 `/c.js` 一个套路），
   `go(3)` 里 `ld('u.js');ld('h.js')`；
2. `encodeURIComponent` 起别名 `var E=...`（下面有 6 处要用）；
3. 确认弹窗文案压短，完整解释挪进本文档。

改完各分片：`/t3` 935、`/t.js` 920、`/u.js` 820、`/h.js` 315（最大 935，余量 89）。
**改任何页面都要重跑 `python tools/check_web_size.py`。**

> `/h.js` 与 `/u.js` 是并发加载的，**谁先到不确定**，所以 `/h.js` 里不能引用
> `/u.js` 定义的任何东西（连 `E` 都不行），只能用 `/t.js` 的 `$ / ajax / msg`。

---

## 7. 体积账（这是本项目最紧的约束）

应用分区只有 588K（`LPBX3.ld`：`ORIGIN = 0x1800C000, LENGTH = 588K`），
而上游 release 的裸 bin 已经 529128 字节、上次刷入的约 544768 字节
→ **余量只有约 56KB**。

密码学内核 + 协议层实测（宿主 `gcc -Os` + `size`，x86-64；ARM Thumb-2 通常略小）：

| 模块 | .text（含常量） | .bss |
|---|---|---|
| `hk_sha512` | 1952 | 0 |
| `hk_kdf` | 900 | 0 |
| `hk_chachapoly` | 3544 | 0 |
| `hk_gf` | 2360 | 0 |
| `hk_curve25519` | 932 | 0 |
| `hk_ed25519` | 3016 | 0 |
| `hk_bignum` | 2080 | 0 |
| `hk_srp` | 3484 | 1216 |
| `hk_tlv` | 1316 | 0 |
| `hk_pair` | 5024 | 0 |
| `hk_json` | 3644 | 0 |
| `hk_acc` | 3032 | 0 |
| `hk_hap` | 5012 | 0 |
| `hk_store` | 1776 | 0 |
| `hk_net` | 2400 | 0（见下方 RAM 账） |
| **合计（一 + 二 a + 二 b + 三）** | **40472** | **1216** |

**≈ 39.5KB flash**。第四阶段（事件推送）估计 2~5KB，总计约 **42~45KB** ——
还能塞进 56KB 余量，**但已经不宽裕了**，每加一个功能都该重新量一次。

> `hk_net` 那 2400 是**纯逻辑**部分（不定义 `HK_NET_PLATFORM` 时量的）。
> 平台胶水（socket / mDNS / userbin 调用）在固件里会再加几百字节，
> 但 `hk_net.o` 在本机编不出来 —— SDK 头文件里有 ARM 内联汇编
> （宿主 gcc 报 `no such instruction: rev16`）。它的正确性是用
> `-fsyntax-only` + `nm` 查库符号来保证的，见 6c.2。

### 7.1 RAM 账（比 flash 更值得盯）

`HK_CONN` 把所有缓冲都内嵌了，实测 **`sizeof(HK_CONN) = 10952` ≈ 10.7KB**：

| 成员 | 字节 |
|---|---:|
| `HK_SETUP`（SRP 的 v/B/S 各 384） | 1344 |
| `HK_VERIFY` | 240 |
| `HK_SESSION` | 88 |
| `rx` + `frag` + `plain` | 4400 |
| `resp` + `tx` | 4700 |

它是 `hk_net.c` 里的静态全局 `g_conn`，**常驻 RAM**，再加上监听线程栈 4096。
DRAM 是 96K，装得下，但 SRP 模幂本身还要吃栈 —— 真机上如果发现配对时
重启，第一件事就是把线程栈从 4096 往上加。

内嵌而不是用栈，是刻意的：这样主机自测和固件用的是**同一份布局**，
不会出现"PC 上好好的、上板栈溢出"。

后续若真的溢出，可选的裁剪顺序：
1. 把 web 页面里的静态说明文字移到外部网页包；
2. `hk_chachapoly` 只保留 AEAD 入口（去掉单独导出的块函数）；
3. SRP 的模幂换成 4 位窗口（省时间不省空间，但可以换来更小的超时余量）；
4. 实在不行就砍掉电压/电流（BL0937 的 CF1 一路）与它的标定页。

> **已经加进构建了**，但默认不进固件：`src/Makefile` 把 `homekit/*.c` 打进
> `userapps.a`，而 `.a` 是静态库 —— 链接器**只提取被引用的 .o**。
> 所以只要 `app_main.c` 不去调 `hk_net_start()`，这 39.5KB 一个字节都不会进固件。
> 启用方式：`-DHK_ENABLE`（`app_main.c` 里的调用点已就位）。
> Keil 工程 `projects/LPT230_gcc.uvprojx` 也加了 `homekit` 分组，两条构建路都通。

---

## 8. 后续阶段要碰的平台能力（已确认可用）

这几个是本分支能不能做成的前提，已经查证过，不需要再翻 SDK：

| 能力 | API / 证据 | 状态 |
|---|---|---|
| 广播 `_hap._tcp` 与 TXT 记录 | `mdns_announce_service()`、`mdns_set_txt_rec()`（`sdk/4.14/include/mdns/mdns.h:510,752`） | ✅ 已用 |
| 监听 TCP 51826 | `lwip_socket/bind/listen/accept/recv/send/close`（`nm` 查库全为 `T`） | ✅ 已用 |
| 真随机数（HAP 强依赖） | 硬件 TRNG `rda_trng_get_bytes()`，失败退化到 `hfsys_random()` | ✅ 已用 |
| 持久化配对信息 | userbin，紧跟 `PLUG_CONFIG` 之后（见 6c.4） | ✅ 已用 |

> ⚠️ **早期这里记的 `MBED_CONF_LWIP_TCP_SOCKET_MAX=12` 是错的**，实测
> `lwipopts.h:127` 把它定成 **4**。这直接决定了本层只支持一条并发连接。

配对信息的存储预算：LTPK 32 + LTSK 32 + 每条配对记录 36(id) + 32(LTPK) 字节。
按 5 条配对记录算 = 64 + 5×68 = 404 字节，加上 16 字节头和 4 字节 CRC = **424**，
即 `HK_STORE_BLOB_MAX`。

---

## 9. 还没做 / 未验证

- **全部代码没有在真机上跑过**（本机没有 `arm-none-eabi`，Keil 在虚拟机里）；
- **没有和真实的 iPhone 配过对**。目前最强背书是：**683 项主机自测** +
  三个外部参考实现的交叉验证（`fast-srp-hap` 验密码学、`hap_ref.py` 验配对流程、
  `hap_session_ref.py` 验会话帧与 JSON）。但"参考实现一致"不等于"iOS 认可"，
  第一次真机配对仍是最关键的一道关；
- **未实现 Add / Remove / List Pairings**（method 0x03/0x04/0x05）。
  这是可选项：不做的话 iOS 仍能配对和使用，只是不能在家庭 App 里管理多个控制器；
- **事件推送的并发未加锁**（已知、未处理）：`g_evt_pending`/`g_evt_val` 是
  `volatile` 位掩码，由 HAP 线程读、其它线程写。单字节写在本平台上不会撕裂，
  但"读-改-写"不是原子的 —— 两个线程同时扳不同路时理论上可能丢一次推送。
  工程里 `user_config` 那套也是这个用法，先沿用；真机出问题再上 RTX 互斥；
- **事件推送只在已建立的连接上推**：没有连接（iOS 不在前台、或刚开机还没连）
  时事件会被丢弃。这不违反 HAP —— iOS 连上时会拉全量 `/accessories`；
- **并发**：现在是**故意只支持一条**连接（`MEMP_NUM_TCP_PCB` 只有 4，见 6c.2）。
  第二条连接会被 accept 后立刻服务完关闭，不会互相踩 —— 但如果两个 iPhone
  同时按配对，后一个要等前一个断开。**没有**做过多连接压力测试；
- **3072 位模幂在 160MHz M4 上的实际耗时不详**。按 CIOS 的运算量估算约 1 秒量级，
  需要在真机上实测。若超过 iOS 的配对超时，可换成 4 位窗口法或把
  `v = g^x mod N` 预先算好存起来（它只依赖固定的配对码，本来就不必每次重算）。
  ⚠️ 若实测很慢，注意它会**阻塞监听线程**——配对期间 HAP 服务是停摆的；
- 配对码的展现方式已定：首次启动随机生成、Web 后台可查看/可改/可一键换码
  （见 6d）。剩下的只是"要不要也印在机身标签上"这个产品问题；
- **mDNS 的 `mdns_start()` 返回值没检查后续冲突**：mdns.h 说 hostname 冲突时
  会自动改名（`foo` → `foo-2`），而我们的配件 ID 用的是 MAC、不依赖 hostname，
  所以暂不处理。但 `keyvals` 会被 mDNS 模块改写这一点已在代码里规避
  （每次 announce 前重建 TXT）。

---

## 10. 参考

- RFC 8032（Ed25519）、RFC 7748（X25519）、RFC 8439（ChaCha20-Poly1305）
- RFC 5869（HKDF）、RFC 4231（HMAC 测试向量）、RFC 5054（SRP，注意它用的是 SHA-1）
- FIPS 180-4（SHA-2）
- `fast-srp-hap`（HAP-NodeJS 的 SRP 依赖；其 `params['hap']` 组 = 3072 位群 + SHA-512，
  本项目 SRP 实现最终以它为外部交叉验证基准，脚本 `tools/crosscheck_srp.js`）
- **HAP-NodeJS**（Homebridge 的实现）：第二阶段的每一步都对着它核对，
  重点文件 `src/lib/HAPServer.ts`、`src/internal-types.ts`（`enum TLVValues`）、
  `src/lib/util/hapCrypto.ts`（nonce 补零规则、**帧的 aad 与分片**）
- 项目整体说明见 [WEB_BL0937.md](WEB_BL0937.md)

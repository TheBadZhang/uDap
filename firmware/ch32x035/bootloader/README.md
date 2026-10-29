# uDap UF2 bootloader（CH32X035F8U6）

上电时按住 PB3 超过 1.2 s 进入，主机出现一个 U 盘；把应用固件
（`firmware.uf2`）拖进去即可升级，不用再接线烧录。

## Flash 布局

| 区域 | 地址 | 大小 |
| --- | --- | --- |
| bootloader（本工程） | `0x00000000..0x00004FFF` | 20 KiB（实际约 16 KiB） |
| 应用固件 | `0x00005000..0x0000F6FF` | 41.75 KiB |
| NV 配置（预留） | `0x0000F700..0x0000F7FF` | 256 B |

应用侧必须用 `../application/ld/Link_highcode_nv_256B_uf2.ld` 链接（`--bootloader=y`，默认）。
**双模式固件（`--wchlink=y`）约 57 KiB，放不下 bootloader**，那种构建退回应用
从 `0x0` 起的老布局，不支持拖拽升级。

## 启动判定（`src/main.c`）

| 条件 | 结果 |
| --- | --- |
| RAM 握手区有 `REQUEST_UF2`（应用发的 vendor command） | 留在 UF2 |
| 上电时**一直按住** PB3 > 1.2 s | 留在 UF2 |
| 应用区无效（入口页是擦除态 / 向量越界） | 留在 UF2 |
| 其余（含「按住但提前松开」） | 直接跳应用（最多花 20 ms 去抖） |

这里**没有**「上电短按 = 切调试器模式」这种手势：调试器模式改由应用里**双击 PB3**
切换（双模式固件把串口固定成单串口，双击手势正好空出来，见 `../application/main.c`）。
bootloader 因此只判断「是否一直按到超时」，逻辑最简单，也不会和长按抢按键。

握手区在 **RAM 起始 16 字节**（`../application/boot_info.h`）：软复位不清 SRAM，而选项字
一擦就是整页（还带着读保护位）。记录带 magic + 反码校验，上电时的随机 RAM 不会被
误认；方向是单向的（应用写、bootloader 读走即清）。

## UF2 接收与落盘（`src/boot_uf2.c`）

虚拟 FAT16（DBR / FAT / 目录 / INFO_UF2.TXT）复用 CherryDAP 子模块里那份实现：

```
third_party/cherrydap/projects/HSLink-Pro/bootloader/bootuf2/bootuf2.c
```

但**写路径是本工程自己的**（`src/boot_uf2.c`）。原因：

* 上游的接收状态是静态的、`bootuf2_init()` 不清位图 —— 一轮拷贝之后再也回不到初始态，
  「拖失败了直接再拖一次」永远成功不了；
* 上游把 `CONFIG_BOOTUF2_FLASHMAX`（字节数）当块号上限用（`BlockIndex < block_max`），
  而位图只有 `BLOCKSMAX/8+1` 字节，畸形块会越界写 `.bss`。

本工程的实现：

| 机制 | 作用 |
| --- | --- |
| 逐扇区块校验（magic / family / PayloadSize==256 / 256 B 对齐 / 地址在应用区 / 块号在范围内） | 目录与 FAT 扇区自然被忽略；畸形块不会落盘 |
| 块号位图去重 | 主机重发 `WRITE` 命令不会重复擦写 |
| 每页「擦 + 写 + 回读校验」（`src/boot_flash.c`） | 写失败立刻知道，不会假设成功；FPEC **必须用 `0x0800_0000` 别名地址**（见下） |
| 页写是**原子且不可重入**的 | 搬数据 + 擦 + 写 + 校验整段关中断（约 3–5 ms）。否则 USB 中断会在擦写的几毫秒里改掉源缓冲/重入进来，表现为随机的「校验失败 @0x…」（实测在 0x6100 踩过） |
| 每页失败重试一次 | 电源抖动/时序偶发失败不至于让整份升级作废 |
| **入口页最后提交**：开始拷贝时先擦掉 `0x5000`，整份文件落盘后才写它 | 中途断开时设备仍停在 bootloader，不会跳进半截固件 |
| 出错后主机安静 2 s 自动复位会话 | 可以直接再拖一次，不必拔线/断电 |
| 同页内容冲突检测 | 畸形 UF2 不会互相覆盖 |

落盘地址只接受 `0x5000..0xF6FF`（256 B 对齐），越界块直接丢弃。

> ⚠️ **地址别名**：代码/数据按 `0x0000_0000` 起链接，但 CH32X035 的 FPEC
> （`FLASH_ROM_ERASE` / `FLASH_ROM_WRITE`）**只接受 `0x0800_0000` 别名区**的地址
> —— SDK 里 `ValidAddrStart = FLASH_BASE = 0x08000000`，传 `0x5100` 会直接返回
> `FLASH_ADR_RANGE_ERROR`（0xFF），表现为「擦除失败 @0x…」而 Flash 一点没动。
> WCH 自己的 IAP 例程同样用别名：`iap.h` 里 `#define FLASH_Base 0x08005000`。
> `src/boot_flash.c` 的 `flash_alias()` 负责换算，链接脚本不用改。

U 盘里的文件：

| 文件 | 说明 |
| --- | --- |
| `INFO_UF2.TXT` | 产品名 / 板名 / 进入 bootloader 的原因 |
| `75446170ID` | family ID 标记（空文件，UF2 惯例） |
| `INDEX   HTM` / `JOIN    HTM` | 指向项目主页的小网页（可选） |

> 拖拽路径不依赖文件名：写入的每个扇区都会被当作 UF2 候选块解析，
> 所以主机把文件写进任何空闲簇都会生效（虚拟盘是 64 MB FAT16）。

### LED 指示

| 现象 | 含义 |
| --- | --- |
| 2 Hz 慢闪 | 等主机拖文件 |
| 常亮 | 正在写 Flash |
| 快闪 N 次 + 长停 | 出错，N = 错误码：`2` 擦写/校验失败、`3` 同页内容冲突、`4` 提交入口页失败 |
| 常亮 300 ms 后重启 | 升级成功 |

### 串口日志（推荐用来排查）

灯在板子上很难看清，所以 bootloader 会同时把状态打到串口：

| 项目 | 值 |
| --- | --- |
| 引脚 | **PB0 = TX**（就是应用里桥接串口 1 的 TX），GND 共地 |
| 参数 | **115200 8N1**，无流控 |
| 内容 | 启动判定（为什么进/不进 UF2）、USB 枚举事件、每 8 块的进度、每条错误（带 Flash 地址）、完成 |

> 日志是 UTF-8 编码的中文 + ASCII 关键字。终端按 UTF-8 打开最清楚；用 GBK 终端时
> 中文会乱码，但 `[boot]` / `[uf2]` / `[flash]`、块计数和 `0x…` 地址这些 ASCII
> 部分仍然可读。

典型输出：

```
[boot] uDap UF2 bootloader 1.2.0
[boot] 上电时按住按键 >1200 ms
[uf2] MSC 模式已启动，等待主机拖 firmware.uf2
[usb] 总线复位
[usb] 已配置（主机认到 U 盘）
[uf2] RX 8/107 块
...
[uf2] RX 104/107 块
[uf2] DONE 107/107 块，复位进应用
[boot] uDap UF2 bootloader 1.2.0
[boot] 上电时没有按键
[boot] 应用有效，跳转到 0x00005000
```

失败时会具体到原因，例如：

```
[flash] 回读校验失败 @0x0000a300
[uf2] ERROR 2（Flash 擦写/校验失败），安静 2 s 后自动复位会话，可直接重拖
```

> PB0 与应用里桥接串口 1 的 TX 是同一根脚：上电瞬间目标板会收到这几行日志字节。
> 这属于正常现象（目标板一般不会因为收到 ASCII 就出问题）；要避免就把 bootloader
> 的 `boot_log_init()` 调用去掉后重编。

### 没有调试器时怎么烧（手动烧录）

`..\build.ps1` 会额外生成 **`build\<mode>\firmware_full.bin`**：bootloader（`0x0`）
+ 应用（`0x5000`）的合并镜像，从 **`0x00000000`** 整片烧进去就行，不需要先有
bootloader、也不需要调试器。

```powershell
cd ..
.\build.ps1            # 生成 firmware_full.bin
.\flash_isp.ps1        # 走芯片自带的 USB ISP（wchisp / WCHISPTool）
```

* 芯片自带 3 KB 的系统 BOOT 区（厂家固化的 ISP）。**用户区为空/无效时**芯片会从
  那里启动，枚举成一个 USB ISP 设备（通常 `4348:55e0`），于是能在完全没有固件的
  情况下烧第一版固件。
* 命令行：装 [wchisp](https://github.com/ch32-rs/wchisp)（已支持 CH32X035；预编译在
  `releases/tag/nightly`，或 `cargo install wchisp`）→ `.\flash_isp.ps1`
  （内部就是 `wchisp flash firmware_full.bin`；Windows 报 `CH375DLL64.dll` 缺失时
  把 dll 放到 exe 旁边，或用 Zadig 换 WinUSB 驱动）。
* 图形界面：WCHISPTool → 芯片选 CH32X035、接口 USB、文件选 `firmware_full.bin`、
  起始地址 `0x00000000`。
* 有别的 SWD/SDI 调试器也行：把 `firmware_full.bin` 烧到 `0x0` 即可
  （或者继续用 `..\flash.ps1` 分两次写 bootloader + 应用）。
* 看不到 ISP 设备：换一根能传数据的 USB 线；板上有 BOOT 按键/跳线就按住再上电；
  片上还有能跑的固件时，按住模式按键上电进 UF2、从 U 盘升级。

## 目录

```
bootloader/                 # 构建入口是上一级的 ../xmake.lua（target: bootloader）
  ld/Link_bootloader.ld   FLASH = 20 KiB @0x0；RAM 前 16 B 留给启动握手区
  src/main.c              启动判定、按键、LED、主循环、串口日志
  src/boot_config.h       Flash 布局 / family ID / USB VID:PID / 按键超时
  src/boot_time.h         SysTick 时间基准（不做除法）
  src/boot_log.c          日志串口（USART4 PB0，115200 8N1）+ 极简 printf
  src/boot_system.c       最小 SystemInit（48 MHz，省掉 GPIO_IPD_Unused）
  src/boot_alloc.c        极简 bump 分配器（替掉 newlib malloc，省约 1 KiB）
  src/boot_flash.c        Flash 页擦写（在 RAM 执行）+ 回读校验 + 应用有效性判断
  src/boot_uf2.c          UF2 块校验 / 去重 / 入口页最后提交 / 完成判定
  src/boot_usb.c          MSC 描述符 / 字符串 / 扇区回调
  src/bootuf2_config.h    bootuf2 的编译期配置
  test/                   主机端仿真测试（不需要硬件）
```

## 构建 / 测试

应用和 bootloader 在**同一份 `../xmake.lua`** 里（两个 target），一次构建两个：

```powershell
# 一键（应用 + bootloader + firmware.uf2 + firmware_full.bin）
cd ..
.\build.ps1          # 或 build.bat

# 只编 bootloader（-P . 不能省，见下）
xmake -P . -r bootloader

# 主机端仿真：把 firmware.uf2 逐扇区喂给固件里真正在用的那份逻辑，
# 覆盖正常/错 family/越界块号/重复下发/中断拷贝五个用例
cd test
.\run.ps1
```

产物：`build/<mode>/bootloader.elf` / `.bin` / `.map`（`after_build` 会检查不超过 20 KiB）。
上层的 `..\build.ps1` 还会生成 `..\build\<mode>\firmware_full.bin`（bootloader +
应用的合并镜像），用于「没有调试器」时整片烧录。

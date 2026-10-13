### 18.7 历史变更：快路径阈值 12 MHz → 6 MHz

#### 2026-09-26：快路径阈值 12 MHz → 6 MHz

上面的基线是「12 MHz 才起跳」。这个阈值其实**不是硬编码在代码里的常量**，而是
CherryDAP 的一个可配宏算出来的（`DAP/Source/DAP.c` 的 `Set_Clock_Delay()`）：

```c
#define MAX_SWJ_CLOCK(delay_cycles)  ((CPU_CLOCK/2U) / (IO_PORT_WRITE_CYCLES + delay_cycles))
if (clock >= MAX_SWJ_CLOCK(DELAY_FAST_CYCLES)) { fast_clock = 1; ... }
```

所以把 `DAP_config.h` 里的 `DELAY_FAST_CYCLES` 从默认 `0` 改成 `2`：

$$\text{阈值}=\frac{48\,\text{MHz}/2}{2+2}=6\,\text{MHz}$$

**为什么改**：快路径的实际 SWCLK 由固定指令数决定（约 6 MHz），**与请求值无关** ——
请求 6 MHz 和请求 12 MHz 拿到同一个速率。阈值留在 12 MHz 的唯一后果，是让主机填
6 ~ 11.5 MHz 时白跑慢路径。实测确认（16 KiB 块，5 轮平均）：

| SWCLK 请求 | 写 KiB/s | 读 KiB/s | 路径 |
| ---: | ---: | ---: | :--- |
| 5000 kHz | 53.3 | 45.1 | 慢（饱和） |
| 5750 kHz | 54.1 | 45.5 | 慢（饱和，最后一档） |
| **6000 kHz** | **125.4** | **121.0** | **快（拐点）** |
| 12000 kHz | 126.3 | 119.0 | 快（与 6 MHz 同速） |

反汇编交叉验证（LTO 把 `Set_Clock_Delay` 内联进了 `DAP_ProcessCommand`）：

```asm
20002460:  lui  a5,0x5b9
20002464:  addi a5,a5,-641      ; a5 = 5 999 999
20002468:  bltu a5,a4, <快路径>  ; clock > 5999999 → 快路径
```

即阈值正好是 6 000 000 Hz；慢路径的延时算法（`(d-2+7)/8`）逐字未变。

**`DELAY_FAST_CYCLES` 的第二个用途**（决定 `PIN_DELAY_FAST()` 补几个 NOP）在本工程
不受影响：`src/dap/sw_dp.c` 已把 `PIN_DELAY_FAST` 重定义为固定一个 `__NOP()`，
且上游唯二使用它的 `SW_DP.c` / `JTAG_DP.c` **未参与编译**（`xmake.lua` 只编
`DAP.c` + `DAP_vendor.c`）。所以该宏在本工程里**纯粹是阈值旋钮**，零时序副作用。

**`SWD_FAST_DELAY_NOPS=1` 的要求仍然成立**（快路径余量）—— 这一点没有变。

**顺带修的一个坑**：`tools/speed.ps1` 与 `tools/openocd_speed_sweep.ps1` 原先把
「≥12000 kHz 是快路径」**硬编码**在打印逻辑里，改阈值后它们会打出错误的路径标注。
现已改成参数 `-FastMinKhz`（默认 6000），与该宏同步。

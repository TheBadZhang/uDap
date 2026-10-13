### 18.5 RAM / Flash 写入速度（pyocd，分阶段）

纯数据搬运，**瓶颈完全在 DAP 侧的 SWD 链路**，是衡量链路性能的干净指标。

命令：

```powershell
python tools/pyocd_ram_speed.py -t stm32f411ce --sizes 64K `
    --frequencies 1000k,2000k,3000k,4000k,8000k,11500k,12000k,16000k,20000k,40000k `
    --repeats 5
```

64 KiB @ `0x20000000`，5 轮取中位数，`program` 模式（FileProgrammer）：

| SWD 请求 | 耗时 | 写入速度 | 路径 |
| --- | --- | --- | --- |
| 1 MHz | 2361.5 ms | 27.1 KiB/s | 慢 |
| 2 MHz | 1752.0 ms | 36.5 KiB/s | 慢 |
| 3 MHz | 983.6 ms | **65.1 KiB/s** | 慢（已饱和） |
| 4 MHz | 983.5 ms | 65.1 KiB/s | 慢（饱和） |
| 8 MHz | 983.5 ms | 65.1 KiB/s | 慢（饱和） |
| 11.5 MHz | 983.7 ms | 65.1 KiB/s | 慢（饱和） |
| **12 MHz** | 303.4 ms | **210.9 KiB/s** | **快（拐点）** |
| 16 MHz | 305.4 ms | 209.5 KiB/s | 快 |
| 20 MHz | 311.1 ms | 205.7 KiB/s | 快 |
| 40 MHz | 330.5 ms | 193.7 KiB/s | 快 |

补充：`--mode raw`（直调 `write_memory_block8`）在 12 MHz 下为 **215.1 KiB/s**，
比 `program` 模式快约 17%。原因是 `RamBuilder._MAX_WRITE_SIZE = 4096`，
`program` 模式按 4 KiB 分包，每包都有一次往返开销。**想测链路上限用 `--mode raw`。**

#### 观察

- 慢路径从 3 MHz 起完全饱和：3/4/8/11.5 MHz 耗时差 < 0.2 ms。因为 `clock_delay`
  被钳到最小值，请求更高频率不会有任何变化。
- 快路径 12~40 MHz 全落在 194~211 KiB/s。**快路径实际 SWCLK 是固定的 ~6 MHz**，
  由指令执行时间决定，与请求频率无关，所以填 12 还是 40 MHz 结果相同；
  差别只是余量厚度。
- 12 → 40 MHz 反而略慢（210.9 → 193.7），说明请求越高余量越紧，宁可填 12。

命令（分离擦除/写入/校验三个阶段，500 KiB 随机数据）：

```powershell
openocd -s <openocd scripts> -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg `
  -c "set speed_khz 12000; set image build/testdata/rand500k.bin; set flash_addr 0x08000000" `
  -c "source tools/openocd_flash_speed_test.tcl"
```

512000 bytes @ `0x08000000`，扇区 0~7 全擦：

| SWD 请求 | 擦除 | 写入 | 校验 | 总计 | 写入速度 | 整体速度 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 MHz | 8203 ms | 22244 ms | 3769 ms | 34216 ms | 22.5 KiB/s | 14.6 KiB/s |
| 2 MHz | 8103 ms | 17366 ms | 3753 ms | 29222 ms | 28.8 KiB/s | 17.1 KiB/s |
| 4 MHz | 8189 ms | 11360 ms | 3740 ms | 23289 ms | 44.0 KiB/s | 21.5 KiB/s |
| 8 MHz | 8309 ms | 11051 ms | 3736 ms | 23096 ms | 45.2 KiB/s | 21.6 KiB/s |
| 11.5 MHz | 8046 ms | 10701 ms | 3735 ms | 22482 ms | 46.7 KiB/s | 22.2 KiB/s |
| **12 MHz** | 8039 ms | **4377 ms** | 3717 ms | **16133 ms** | **114.2 KiB/s** | **31.0 KiB/s** |
| 16 MHz | 8162 ms | 4383 ms | 3714 ms | 16259 ms | 114.1 KiB/s | 30.7 KiB/s |
| 20 MHz | 8138 ms | 4400 ms | 3715 ms | 16253 ms | 113.6 KiB/s | 30.8 KiB/s |

写入速度曲线：

```
 1 MHz  ▏█████▎                                    22.5 KiB/s
 2 MHz  ▏██████▊                                   28.8
 4 MHz  ▏██████████▍                               44.0
 8 MHz  ▏██████████▋                               45.2   ┐ 慢路径饱和
11.5MHz ▏███████████                               46.7   ┘
12 MHz  ▏██████████████████████████▋              114.2   ← 拐点，2.4x
16 MHz  ▏██████████████████████████▋              114.1
20 MHz  ▏██████████████████████████▍              113.6
```

#### 观察

- **擦除恒定 ~8.1 s**（8039~8309 ms，抖动 < 3%），与 SWD 时钟完全无关。
  这是 F411 片上 Flash 控制器的固定成本，DAP 再快也压不动。
- **写入在 12 MHz 跳变 2.4 倍**（46.7 → 114.2 KiB/s），拐点位置与 RAM 完全一致。
- **校验 ~3.7 s / 133 KiB/s**，同样不随 SWD 时钟变化 —— 校验是读操作，
  受 DAP 读带宽限制（~134 KiB/s），而非目标。
- 12 → 20 MHz 写入只差 0.5%，**没有理由往上写**。

| | 写入速度 | 瓶颈 |
| --- | --- | --- |
| **RAM** | 210.9 KiB/s | DAP 侧 SWD 链路 |
| **Flash** | 114.2 KiB/s | 目标 Flash 编程时间 |
| 比值 | **1.85×** | |

Flash 写入被目标每页编程后的等待拖住，所以比纯搬运的 RAM 慢近一倍。
**要评估 DAP 链路本身，应看 RAM 数据；Flash 数据还混了目标特性。**

# 第四部分：附录

## 20. 测试脚本一览

| 脚本 | 用途 |
| --- | --- |
| `firmware/tools/openocd_speed_test.tcl` | 单频率 SWD 内存读写测速（会被 `speed.ps1` / 扫描脚本调用） |
| `firmware/tools/speed.ps1` | 单个 SWCLK 频率的 SWD 测速 |
| `firmware/tools/openocd_speed_sweep.ps1` | **频率扫描**，一次跑一串频率并汇总成表（见「SWD 调试吞吐」） |
| `firmware/tools/com_loopback_speed.py` | **COM 口环回吞吐 / 利用率**测试，持续满载模式（见「COM 口环回吞吐与利用率」与「持续满载的可靠性边界」） |
| `firmware/tools/com_loopback_paced.py` | **分块 / 间隔发送**测试，含 BRR 与实际波特率推算（见「持续满载的可靠性边界」与「撤除波特率上限之后」） |
| `firmware/tools/com_loopback_matrix.py` | **波特率 × 块大小矩阵**扫描，找最小可靠发送粒度（见「撤除波特率上限之后」） |
| `firmware/tools/uart_*.py` | USART4 桥的连通性 / 全双工 / 流控 / 压力测试 |
| `firmware/tools/dap_probe_check.py` | 探针基本信息检查 |
| `firmware/tools/pyocd_speed.py` | **pyocd 测速**：与 `speed.ps1` 同地址 / 同块大小 / 同 pattern / 同回读校验，数字可直接横向比较；最接近固件真实天花板（实测 262 写 / 232 读 KiB/s） |
| `firmware/tools/cycle.ps1` | **一条命令跑完「编译 → 烧录 → 测速」**：用 RVLink + WCH OpenOCD 刷 uDap，再轮询 USB 重新枚举，最后调 `speed.ps1` |
| `firmware/tools/probe_swd_sweep.py` | **对照探针扫描**：对第三方 CMSIS-DAP 探针做「1~15 MHz × 1/4/16/64 KiB × 重复次数」的读写吞吐扫描，pyocd 与 OpenOCD 两个工具都支持，输出 CSV + 汇总表（见「对照探针：测试方法与横向对比」） |
| `firmware/tools/openocd.cfg` | 独立的 100 × 64 KiB 读写压测配置（`adapter speed 40000`） |

复现本文数据：

```powershell
# 6.1 SWD 频率扫描（需接好目标板）
.\firmware\tools\openocd_speed_sweep.ps1 -Rounds 5 -Warmup 1

# 6.2 / 6.3 COM 环回，持续满载（需 PB0-PB1 短接）
python .\firmware\tools\com_loopback_speed.py --seconds 2 --repeats 3

# 6.3 / 6.4 分块发送（可用 --chunk / --gap-ms 控制发送粒度）
python .\firmware\tools\com_loopback_paced.py --baud 2000000 --chunk 4096 --total 262144 --repeats 3
python .\firmware\tools\com_loopback_paced.py --baud 6000000 --chunk 192 --total 32768

# 6.4 波特率 × 块大小矩阵
python .\firmware\tools\com_loopback_matrix.py `
    --baud 2000000,2500000,3000000,4000000,6000000 --chunk 192,512,4096 `
    --total 32768 --repeats 2 --csv firmware\build\com_loopback_highbaud.csv
```

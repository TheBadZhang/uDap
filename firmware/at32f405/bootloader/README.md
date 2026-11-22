# uDap AT32F405 bootloader（DFU + WinUSB 双通道升级）

本目录是 AT32F405KBU7_4（128 KiB Flash / 70 KiB RAM）的 bootloader 源码。
上电先跑这里的固件（0x08000000，16 KiB），由它决定「跳应用」还是「停在升级模式」。

## 1. Flash 布局

| 区域 | 起始地址 | 大小 | 说明 |
| --- | --- | --- | --- |
| bootloader | 0x08000000 | 16 KiB（0x4000） | 本目录，`bootloader_16k.ld` |
| 应用 | 0x08004000 | 112 KiB（0x1C000） | 应用链接脚本 `startup/linker/at32f405_flash_128k_70k.ld` |
| 用户数据 | 0x1FFFF800 | 2 KiB | 厂商 ESIG/用户区，bootloader 不碰 |

- 应用的 `SCB->VTOR` 由 `application/bsp/bsp_vector.c` 的 `__attribute__((constructor(101)))`
  在 `SystemInit()` 之后、`main()` 之前设成 `0x08004000`（不改 `sdk/` 里的厂商源码）。
- 两个固件的 `.noinit` 段都固定在同一地址 `0x20010000`，因此
  `g_udap_boot_magic`（`volatile uint32_t`，`0x55444642` = 'UDFB'）跨复位保持有效。

## 2. 上电流程

1. `system_clock_config()`（12 MHz HEXT → 216 MHz）→ `bsp_delay_init()`；
2. LED 自检：PB2/PB4 低电平点亮，闪两下（60 ms），随后保持灭；
3. 读 `g_udap_boot_magic`：等于 `0x55444642` → 清 magic、留在升级模式（USB 0205）；
4. 否则检查 `0x08004000` 处的初始 MSP（`0x20000000..0x20011800`）与 Reset 向量
   （`0x08004000..0x08020000`，且 bit0=1）：合法 → 跳应用，不合法 → 留在升级模式；
5. 跳应用：清 magic → `__disable_irq()` → `SysTick->CTRL = 0` →
   `SCB->VTOR = 0x08004000` → `__set_MSP(*(uint32_t *)0x08004000)` → `__enable_irq()` →
   跳 `*(uint32_t *)0x08004004`。

## 3. USB 身份

- `VID 0x0D28 / PID 0x0205`；序列号 = 芯片 UID（0x1FFFF7E8 低 32 位 + 0x1FFFF7EC 高 32 位，
  各 8 个十六进制字符，全 0/全 F 时回退 `DEADBEEF`）。
- 接口 0：标准 DFU 1.1（class 0xFE / subclass 1 / protocol 2），`wTransferSize = 1024`，
  alt 0 直接对应应用区 `0x08004000`；DFU 下载按块号寻址（`0x08004000 + block * 1024`），
  写哪页擦哪页，`wLength = 0` 的 DNLOAD 结束后校验并复位跳应用。
- 接口 1：WinUSB（MS OS 2.0 描述符，`bVendorCode = 0x20`），批量端点 OUT `0x06` / IN `0x85`，
  承载 `docs/bootloader-protocol.md` 第 4 节的私有协议
  （GET_INFO / ERASE_APP / WRITE / READ / FINISH / REBOOT）。
- 注意：`application/dap/dap_main.c` 里的应用固件也声明了 MS OS 2.0 的接口 0，
  两者同时在线时 Windows 可能沿用缓存；本 bootloader 把接口 0（DFU）和接口 1（vendor）
  都加了 WINUSB CompatibleId，并用同一 GUID `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`。

## 4. 构建

在工程目录里带 `-P .` 构建（默认会同时产出 firmware 与 bootloader）：

```powershell
cd 'D:\repo\USB转\uDap\firmware\at32f405'
xmake -P . -j 8
```

产物：

- `build\bin\bootloader\bootloader.{elf,hex,bin}`
- `build\bin\firmware\firmware.{elf,hex,bin}`

bootloader 的 `after_build` 会检查 `.bin` 是否超过 16 KiB（0x4000），超了直接报错并中止，
不会生成超尺寸产物。

## 5. 烧录（外部 DAPLink，UID F0BFABCD5AF5BE69）

pyocd 的 `load` 默认整片擦除，如果分两次烧（先 bootloader 再应用）第二次会把 bootloader 擦掉。
正确做法是**一次会话里先整片擦，再一次 load 两个 hex**：

```powershell
cd 'D:\repo\USB转\uDap\firmware\at32f405'
$pack = 'misc\ArteryTek.AT32F402_405_DFP.2.1.6.pack'
pyocd erase -c -u F0BFABCD5AF5BE69 -t at32f405kbu7_4 -f 1000000 --pack $pack
pyocd load -u F0BFABCD5AF5BE69 -t at32f405kbu7_4 -f 1000000 --pack $pack -e sector `
    build\bin\bootloader\bootloader.hex build\bin\firmware\firmware.hex
pyocd reset -u F0BFABCD5AF5BE69 -t at32f405kbu7_4 -f 1000000 --pack $pack
```

`-e sector` 让 `load` 只按需擦扇区；两个 hex 在同一个 `load` 会话里按地址合并，不会互相擦掉。

## 6. 关键实现注意点

- **Flash 扇区粒度是 1 KiB**：AT32F405KBU7_4 属于 xB（128 KiB）档，SDK 例程
  `project/at_start_f402/examples/flash/flash_write_read/src/flash.c:33-37` 写的是
  `#if defined (AT32F402_405xC) ... 2048 #else 1024`——只有 xC（256 KiB）才是 2 KiB。
  因此 `boot_config.h` 里 `BOOT_SECTOR_SIZE = 0x400`（`BOOT_APP_SECTOR_COUNT = 112`），
  擦除/懒擦除位图都按 1 KiB 处理；而 `GET_INFO` 上报的 `page_size` 保持规范第 4 节的字面值
  `2048`（协议约定值，主机侧按它算进度，不参与本机擦除粒度决策）。
  早期按 2 KiB 擦除时，写第 3 个 504 字节块（从 0x080043F0 起）会跨进未擦的 0x08004400
  扇区而返回 `status = 0x02`，已修复并回归。
- **越界保护**：所有 Flash 读写都先换算成「应用区内偏移」，越界一律返回 `status = 0x01`，
  绝不擦写 bootloader 自身 16 KiB（`tools/bootloader_flash_test.py bounds` 有回归用例）。
- **CRC32**：IEEE 多项式 0xEDB88320，初值/末尾异或 0xFFFFFFFF，与 `zlib.crc32` 一致；
  自实现，不依赖 libc。
- `usb_config.h` 里 `CONFIG_USBDEV_REQUEST_BUFFER_LEN = 1024`：DFU 的 `wTransferSize`
  直接取这个值，同时保证 EP0 OUT 能收满 1024 字节。
- bootloader 只驱动 PB2/PB4（LED）和片内 USB HS PHY；PA2/PA3/PA4/PA5/PA6/PA7/PB0/PB1
  一律不驱动，避免拉低板上目标板的 SWD/串口/RESET。

## 7. 回归测试脚本

`tools/bootloader_flash_test.py`（pyusb + libusb_package，无需 pyserial）：

```powershell
cd 'D:\repo\USB转\uDap\firmware\at32f405'
python tools\bootloader_flash_test.py --serial 5059EC86 list
python tools\bootloader_flash_test.py --serial 5059EC86 enter-boot
python tools\bootloader_flash_test.py --serial 5059EC86 flash   build\bin\firmware\firmware.bin
python tools\bootloader_flash_test.py --serial 5059EC86 badcrc  build\bin\firmware\firmware.bin
python tools\bootloader_flash_test.py --serial 5059EC86 dfu
python tools\bootloader_flash_test.py --serial 5059EC86 dfu-flash build\bin\firmware\firmware.bin
python tools\bootloader_flash_test.py --serial 5059EC86 readback  build\bin\firmware\firmware.bin
python tools\bootloader_flash_test.py --serial 5059EC86 bounds
python tools\bootloader_flash_test.py --serial 5059EC86 reboot
```

**务必用 `--serial` 过滤自己的板子**：外部 DAPLink（F0BFABCD5AF5BE69）跑的是同一份
uDAP 应用固件，也枚举成 `0d28:0204`，盲取第一台设备会对它发 ENTER_BOOT
（它会进入 UF2 模式，变成 `1209:7544` 的 U 盘）。

## 8. 已知限制

- 没有 DfuSe 的 `DFU_SPECIAL_CMD_SET_ADDRESS_POINTER(0x21)` / `ERASE(0x41)` 扩展命令：
  DFU 只支持按块号顺序下载到 0x08004000 起的应用区，不能任意指定地址。
- `dfu-util` 未安装的机器上用脚本的 `dfu-flash` 子命令代替（标准 DFU 1.1 控制传输，
  效果等价）；`dfu` 子命令只验证描述符与 GETSTATUS。
- 应用区末尾未写满的部分保持旧内容；FINISH 只按 `total_len` 计算 CRC 并校验这两个字段。

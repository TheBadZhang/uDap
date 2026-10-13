## 21. 第三方依赖

```sh
git submodule update --init
```

| submodule | commit | 版本 |
| --- | --- | --- |
| `firmware/third_party/cherryusb` | `0e40349` | v1.6.1-82 |
| `firmware/third_party/cherryrb` | `19ea7c6` | v1.0.0 |
| `firmware/third_party/cherrydap` | `c40bb5a` | HSLinkPro-2.4.2-41 |

均在各自的开源许可下使用；本工程对子模块**不做修改**，所有适配放在 `firmware/src/`。

其中 **CherryDAP** 是 CMSIS-DAP 命令解析层的来源（`third_party/cherrydap/README.md`）——
一个基于 [CherryUSB](https://github.com/sakumisu/CherryUSB) 与
[ARMmbed DAPLink](https://github.com/ARMmbed/DAPLink) 的 DAPLink 模板。它的特性：

- CMSIS-DAP version 2.1，只支持 WinUSB
- 支持 SWD + JTAG
- 支持 USB2UART、WebUSB、自定义 HID

本工程只编入它的 `DAP/Source/DAP.c` 与 `DAP_vendor.c`（命令解析），传输后端换成自己的
`src/dap/sw_dp.c`（见「模式一：CMSIS-DAP 数据通路」）；上游的 `SW_DP.c` / `JTAG_DP.c`
**不参与编译**。它的 `projects/` 下有若干参考板（`bl616` / `esp32s3` /
`hpm5301evklite` / `HSLink-Pro`）。

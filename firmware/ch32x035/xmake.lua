-- 工程与全局策略
set_project("ch32x035f8u-dap")
set_version("0.1.0")
set_xmakever("2.9.8")
set_policy("build.release.strip", false)

-- 构建模式与工具链
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})
includes("toolchains/wch-riscv/xmake.lua")

local arch_flags = {"-march=rv32imacxw", "-mabi=ilp32", "-msmall-data-limit=8", "-mno-save-restore"}
local startup_file = "sdk/Startup/startup_ch32x035_highcode.S"

-- 构建后统计输出用的辅助函数
local function parse_berkeley_size(size_output)
    for line in size_output:gmatch("[^\r\n]+") do
        local text, data, bss = line:match("^%s*(%d+)%s+(%d+)%s+(%d+)%s+%d+%s+%x+%s+")
        if text and data and bss then
            return {
                text = tonumber(text),
                data = tonumber(data),
                bss = tonumber(bss)
            }
        end
    end
    raise("failed to parse berkeley size output")
end

local function parse_sysv_section_sizes(size_output)
    local sections = {}
    for line in size_output:gmatch("[^\r\n]+") do
        local name, size = line:match("^%s*(%S+)%s+(%d+)%s+0x%x+%s*$")
        if name and size then
            sections[name] = tonumber(size)
        end
    end
    return sections
end

local function sum_section_sizes(sections, names)
    local total = 0
    for _, name in ipairs(names) do
        local size = sections[name]
        if size == nil then
            raise("missing section in sysv size output: " .. name)
        end
        total = total + size
    end
    return total
end

local function format_size(bytes)
    if bytes >= 1024 then
        return string.format("%.2f KiB (%d bytes)", bytes / 1024.0, bytes)
    end
    return string.format("%d bytes", bytes)
end

local function print_memory_summary(size_info)
    print(string.format("Flash used: %s", format_size(size_info.text + size_info.data)))
    print(string.format("RAM   used: %s", format_size(size_info.ram)))
end

-- 调试器功能：CMSIS-DAP（CherryDAP + src/dap、src/drv glue）**固定编入**；
-- WCH-Link（RVSWD 目标调试 / Flash 烧录，见 src/wchlink）**可选**。
--
-- 默认只编 CMSIS-DAP：WCH-Link 平时用不到，但它的代码占掉数 KiB Flash、
-- 数据占掉约 6 KiB RAM（overlay 段），把这两样省下来才能放得下双串口
-- （第二个 CDC + USART2 + 两路 FIFO）。
--
-- 传 --wchlink=y 得到「双模式」固件：两套调试器同编一份，上电时按住 PB3
-- 切到另一种（见 src/main.c），两套私有数据共用同一段 RAM（overlay，见
-- src/mode_overlay.h 与 sdk/Ld/Link_highcode_nv_256B_dual.ld）。
-- USB 本身不可关：本芯片必然用到 USB。

-- 用芯片出厂 UID（ESIG 区，96 位、每颗唯一）作为 CMSIS-DAP 的 USB 序列号。
-- **默认开启**：只有每颗芯片唯一，多块板子同时插在一台机器上才能区分。
-- 传 --esig_sn=n 退回固定的 "DEADBEEF"（所有板子相同，无法区分）。
-- 寄存器定义见 CH32X035 应用手册第 19 章「电子签名（ESIG）」。
-- 序列号在 src/dap/dap_main.c 里设置。
option("esig_sn")
    set_default(true)
    set_showmenu(true)
    set_description("Use the chip ESIG UID (96-bit, unique per chip) as the USB serial number")
option_end()

-- 是否把 WCH-Link（RVSWD）调试器一起编进固件
option("wchlink")
    set_default(false)
    set_showmenu(true)
    set_description("Also build the WCH-Link (RVSWD) debugger mode (dual-mode firmware, costs ~6 KiB RAM)")
option_end()

target("firmware")
    -- 目标属性
    set_plat("cross")
    set_arch("riscv")
    set_kind("binary")
    set_languages("gnu11")
    set_toolchains("wch-riscv")
    set_warnings("all")
    set_targetdir("build/$(mode)")
    set_filename("firmware.elf")

    local esig_sn_enabled = has_config("esig_sn")
    local wchlink_enabled = has_config("wchlink")

    -- 本工程应用代码
    -- src/*.c        系统初始化、中断、main（调试器模式：CMSIS-DAP [+ WCH-Link]）
    -- src/bsp/*.c    延时/打印/系统/USB 硬件初始化/UID
    -- src/port/*.c   CherryUSB 的 CH32X035 设备端口 glue
    add_files("src/*.c", "src/bsp/*.c", "src/port/*.c")
    add_includedirs("src", "src/port")
    add_defines("ESIG_SN=" .. (esig_sn_enabled and 1 or 0))
    add_defines("WCHLINK_ENABLE=" .. (wchlink_enabled and 1 or 0))

    -- 串口驱动（全部串口相关代码都在这一对文件里，见 src/drv/drv_uart.h 的说明）
    --   COM1  <-> USART4 (PB0/PB1)          两模式都可用
    --   COM2  <-> USART2 (PA2/PA3)          仅双串口模式
    --   PA2/PA3 在单串口模式下是 DTR / RTS（低有效）
    --   两路 RX 都走 DMA（USART4=Ch8、USART2=Ch6），TX 用 TXE 中断
    --   还包含：串口模式切换 + PB12 指示灯 + 选项字持久化 + CDC 行编码回调
    -- （usbd_cdc_acm_* 回调由两种调试器模式**共用**；不共用的话双模式固件会撞成
    --   重复符号，因为两个模式各定义一份。）
    add_files("src/drv/drv_uart.c")
    add_sysincludedirs("../third_party/cherryrb")
    add_files("../third_party/cherryrb/chry_ringbuffer.c")

    -- 模式按键（上电按住切调试器模式 / 双击切串口模式）+ 把模式持久化到用户选择字
    add_files("src/drv/drv_mode_button.c", "src/drv/drv_mode_store.c")

    -- CMSIS-DAP 专有的应用 glue
    add_files("src/dap/*.c")
    add_includedirs("src/dap")

    -- 链接脚本：
    --   --wchlink=y  双模式，带 OVERLAY 段（两套私有数据共用同一段 RAM 地址）
    --   默认只有 CMSIS-DAP，用不带 overlay 的脚本（不需要 _xxx_overlay_bss 符号）
    local linker_script = wchlink_enabled and "sdk/Ld/Link_highcode_nv_256B_dual.ld"
        or "sdk/Ld/Link_highcode_nv_256B.ld"

    -- WCH-Link 专有的应用 glue（含无 LED / 无电源开关的空实现）
    if wchlink_enabled then
        add_files("src/wchlink/**/*.c", "src/wchlink/**/*.S",
                  "src/status/*.c", "src/drv/drv_power_switch.c")
    end

    -- CH32X035 SDK
    -- 注意：不编译 sdk/Debug/debug.c —— 它无条件定义了 _write 与 _sbrk，
    -- 会与 src/bsp/bsp_print.c 的 retarget 实现重复定义（参考工程同样排除它）。
    -- sdk/Debug 仍需作为头文件搜索路径：src/ch32x035_it.h 依赖 debug.h。
    add_files(startup_file,
              "sdk/Core/*.c",
              "sdk/Peripheral/src/*.c")
    add_includedirs("sdk/Core", "sdk/Peripheral/inc", "sdk/Debug")

    -- ===== USB：必选，不做任何条件裁剪 =====
    -- 本芯片必然使用 USB，因此 CherryUSB 核心、CH32X035 设备端口 glue、
    -- 应用侧初始化配置（src/usb_config.h）与芯片级 USB 硬件初始化
    -- （src/bsp/bsp_usb.c）缺一不可。缺文件由下面的 before_build 报错，
    -- 避免默默编出一个没有 USB 的固件。
    add_files("../third_party/cherryusb/core/usbd_core.c",
              "../third_party/cherryusb/class/cdc/usbd_cdc_acm.c")
    add_sysincludedirs("../third_party/cherryusb/core",
                       "../third_party/cherryusb/common",
                       "../third_party/cherryusb/class/cdc",
                       "../third_party/cherryusb/class/msc",
                       "../third_party/cherryusb/class/hid")

    -- ===== CMSIS-DAP =====
    -- CherryDAP（上游 CMSIS-DAP 实现）
    add_files("../third_party/cherrydap/DAP/Source/DAP.c",
              "../third_party/cherrydap/DAP/Source/DAP_vendor.c")
    add_sysincludedirs("../third_party/cherrydap",
                       "../third_party/cherrydap/DAP/Include")

    -- 编译、汇编与链接选项
    add_cxflags(table.join(arch_flags, {
                   "-D__PACKED=__attribute__((packed))",
                   "-fmessage-length=0",
                   "-fsigned-char",
                   "-ffunction-sections",
                   "-fdata-sections",
                   "-fno-common",
                   "-Wno-comment",
                   "-Wno-unused-parameter",
                   "-Wno-missing-prototypes"}), {force = true})
    add_asflags(table.join(arch_flags, {
                   "-ffunction-sections",
                   "-fdata-sections"}), {force = true})
    add_ldflags(table.join(arch_flags, {
                   "-ffunction-sections",
                   "-fdata-sections",
                   "--specs=nano.specs",
                   "--specs=nosys.specs",
                   "-nostartfiles",
                   "-Wl,-T" .. linker_script,
                   "-Wl,--gc-sections"}), {force = true})

    -- 按模式追加优化与符号设置
    --
    -- 两种模式（CMSIS-DAP / WCH-Link）的代码必须同时装进 62 KiB Flash，
    -- 所以 debug 与 release 都用 -Os：
    --   * release：-O3 + LTO 会把整条 USB 调用链内联进 .highcode 的中断处理函数，
    --     使其膨胀到 18 KiB，同时吃光 62 KiB Flash 与 20 KiB RAM（参考工程
    --     ch32x035-usb-wchlink-main 同样使用 smallest）。
    --   * debug：-Og / -O0 下两套代码 + overlay 装不进 Flash（`.data` 直接报
    --     region overflow），所以也改用 -Os + LTO，只保留调试符号。
    if is_mode("debug") then
        set_symbols("debug")
    else
        set_symbols("hidden")
    end
    set_optimize("smallest")
    add_cxflags("-flto", {force = true})
    add_ldflags("-flto", {force = true})

    -- 构建钩子，按触发顺序排列

    -- 打印当前模式、工具链与 SDK 目录
    before_build(function (target)
        -- 必需文件校验放在这里，而不是 xmake.lua 顶层：
        -- xmake 的工程描述作用域是精简沙箱，顶层没有 raise（顶层调用会报
        -- "attempt to call a nil value (global 'raise')"），而 before_build 是
        -- 脚本作用域，raise 可用，且它在任何编译动作之前执行。
        --
        -- CMSIS-DAP 侧的文件全部必需；WCH-Link 的文件只在 --wchlink=y 时必需。
        -- 少任何一个都直接报错，而不是默默编出一个少了一半功能的固件。
        local required = {"src/board_pin_map.h",
                          "src/swd_pin_map.h",
                          "src/usb_config.h",
                          "src/port/usb_ch32x035_dc_usbfs.c",
                          "src/bsp/bsp_usb.c",
                          "src/drv/drv_uart.c",
                          "src/drv/drv_uart.h",
                          "src/drv/drv_mode_button.c",
                          "src/drv/drv_mode_store.c",
                          "src/dap/DAP_config.h",
                          "src/dap/dap_main.c",
                          "src/dap/sw_dp.c",
                          "src/dap/swd_gpio.h",
                          "src/dap/swd_gpio_x035.c",
                          "src/bsp/bsp_uid.c"}
        if wchlink_enabled then
            table.insert(required, "src/wchlink/usb/wchlink_usb.c")
            table.insert(required, "src/wchlink/transport/rvswd_phy_gpio.c")
            table.insert(required, "src/wchlink/target/rvswd_target_registry.c")
            table.insert(required, "src/status/status_led.c")
            table.insert(required, "src/drv/drv_power_switch.c")
        end
        for _, req in ipairs(required) do
            if not os.isfile(req) then
                raise("missing required source: " .. req)
            end
        end

        local toolchain = assert(target:toolchain("wch-riscv"))
        local toolchain_root = toolchain:get("toolchain_root")
        cprint("${cyan}Using mode:${clear} %s", get_config("mode") or "debug")
        cprint("${cyan}Using toolchain:${clear} %s", toolchain_root)
        cprint("${cyan}Using SDK:${clear} sdk/")
        cprint("${cyan}Debugger:${clear} %s", wchlink_enabled
            and "CMSIS-DAP + WCH-Link (dual mode, hold button at power-up to switch)"
            or "CMSIS-DAP only (--wchlink=y to add WCH-Link)")
        cprint("${cyan}Serial #:${clear} %s", has_config("esig_sn")
            and "chip ESIG UID (unique)" or "fixed DEADBEEF")
    end)

    -- 指定 map 文件，与 elf 同目录
    before_link(function (target)
        local mapfile = path.join(target:targetdir(), target:basename() .. ".map")
        target:add("ldflags", "-Wl,-Map=" .. mapfile, {force = true})
    end)

    -- 生成 bin 并统计 Flash 与 RAM 占用
    after_build(function (target)
        local toolchain = assert(target:toolchain("wch-riscv"))
        local prefix = toolchain:get("gcc_prefix")
        local targetfile = target:targetfile()
        local bindir = target:targetdir()

        local binfile = path.join(bindir, "firmware.bin")
        os.execv(prefix .. "objcopy", {"-O", "binary", targetfile, binfile})

        -- OVERLAY 段的 LMA 由 ld 单独排布，**不受 FLASH 区域长度约束**，超了也不报错，
        -- 只会静默生成一个装不进芯片的 .bin。所以按 .bin 实际长度兜底检查。
        local flash_limit = 63232  -- 62K - 256（末尾 256 B 留给 NV 配置，见链接脚本）
        local binsize = os.filesize(binfile)
        if binsize > flash_limit then
            raise(string.format("firmware.bin is %d B, exceeds the %d B FLASH limit " ..
                                "(OVERLAY LMAs are not bounded by the FLASH region)",
                                binsize, flash_limit))
        end

        local sections_output = os.iorunv(prefix .. "size", {"--format=sysv", targetfile})
        local size_info = parse_berkeley_size(os.iorunv(prefix .. "size", {"--format=berkeley", targetfile}))
        local sections = parse_sysv_section_sizes(sections_output)

        print(sections_output)
        -- .highcode 在 SRAM 中执行；.stack 是 NOBITS，不应依赖 Berkeley 分类推断 RAM 占用
        size_info.ram = sum_section_sizes(sections, {".highcode", ".data", ".bss", ".stack"})
        -- 双模式的 overlay 段（dap_overlay / wchlink_overlay）被链接到**同一段地址**，
        -- 不能相加，只能取较大的那个作为这段窗口的占用（见 sdk/Ld/Link_highcode_nv_256B_dual.ld）。
        size_info.overlay = 0
        for _, name in ipairs({"dap_overlay", "wchlink_overlay"}) do
            local size = sections[name]
            if size ~= nil and size > size_info.overlay then
                size_info.overlay = size
            end
        end
        size_info.ram = size_info.ram + size_info.overlay
        print_memory_summary(size_info)
    end)

    -- 清理生成的 bin 与 map
    after_clean(function (target)
        local bindir = target:targetdir()
        os.rm(path.join(bindir, "firmware.bin"))
        os.rm(path.join(bindir, target:basename() .. ".map"))
    end)

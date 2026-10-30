-- uDap CH32X035F8U6：**应用固件 + UF2 bootloader 的统一构建入口**
--
-- 两个 target 放在同一个工程里（不再有 bootloader/xmake.lua 那种「自成一家」）：
--
--   target("bootloader")  Flash 0x0000..0x3FFF（16 KiB），只做 UF2 拖拽升级
--   target("firmware")    应用；带 bootloader 布局时链接到 0x4000
--
-- 两者共用同一套工具链/编译选项/SDK，只有链接脚本、需要的 CherryUSB 类
-- （bootloader 只要 MSC）和入口代码不同，所以放在一个文件里更好对照。
--
-- 用法:
--   xmake f -P . -m release --esig_sn=y --wchlink=n --swd_pins=pa5_pa6 --bootloader=y -y
--   xmake -P . -r                  # 两个 target 都编（bootloader 在前）
--   xmake -P . -r firmware         # 只编应用
--   xmake -P . -r bootloader       # 只编 bootloader（默认布局下才存在）
--
-- 注意 `-P .` 不能省：仓库根目录也有一个只含 task 的 xmake.lua，不给 -P 时
-- xmake 会认成那个工程（报 not a valid target name）。
--
-- 一般直接用上层脚本（它还会把 bootloader + 应用合并成 firmware_full.bin 并生成
-- firmware.uf2）：
--   .\build.ps1     或     build.bat
--
-- 产物（都在 build\<mode>\ 下）：
--   firmware.elf / .bin / .map / .uf2
--   bootloader.elf / .bin / .map

set_project("ch32x035f8u-dap")
set_version("1.3.0")
set_xmakever("2.9.8")
set_policy("build.release.strip", false)

-- 构建模式与工具链
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})
includes("toolchains/wch-riscv/xmake.lua")

local arch_flags = {"-march=rv32imacxw", "-mabi=ilp32", "-msmall-data-limit=8", "-mno-save-restore"}

local app_src = "application"
local sdk = "sdk"
local third_party = "../third_party"

-- UF2 格式头文件来自 third_party/uf2（microsoft/uf2）；虚拟 U 盘与写路径都由
-- bootloader/src 自己实现（省掉上游的 malloc/INFO_UF2 逻辑，见 boot_disk.c）
local uf2_include = third_party .. "/uf2"

local esig_sn_enabled = has_config("esig_sn")
local wchlink_enabled = has_config("wchlink")
-- 双模式固件（约 57 KiB）放不下 16 KiB 的 bootloader，只能退回应用独占 Flash
local use_bootloader = has_config("bootloader") and not wchlink_enabled

local function format_size(bytes)
    if bytes >= 1024 then
        return string.format("%.2f KiB (%d bytes)", bytes / 1024.0, bytes)
    end
    return string.format("%d bytes", bytes)
end

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
    os.raise("failed to parse berkeley size output")
end

local function parse_sysv_section_sizes(size_output)
    local sections = {}
    for line in size_output:gmatch("[^\r\n]+") do
        -- size 的 addr 列在 GCC12 里是 0x...，GCC15 改成十进制；这里只关心名字和大小
        local name, size = line:match("^%s*(%S+)%s+(%d+)%s+%S+%s*$")
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
            os.raise("missing section in sysv size output: " .. name)
        end
        total = total + size
    end
    return total
end

local function print_memory_summary(size_info)
    print(string.format("Flash used: %s", format_size(size_info.text + size_info.data)))
    print(string.format("RAM   used: %s", format_size(size_info.ram)))
end

-- 两个 target 共用的编译/汇编选项
local function add_shared_flags(extra_cxflags)
    local cxflags = {
        "-D__PACKED=__attribute__((packed))",
        "-fmessage-length=0",
        "-fsigned-char",
        "-ffunction-sections",
        "-fdata-sections",
        "-fno-common",
        "-Wno-comment",
        "-Wno-unused-parameter",
        "-Wno-missing-prototypes"
    }
    for _, flag in ipairs(extra_cxflags or {}) do
        table.insert(cxflags, flag)
    end

    add_cxflags(table.join(arch_flags, cxflags), {force = true})
    add_asflags(table.join(arch_flags, {
                   "-ffunction-sections",
                   "-fdata-sections"}), {force = true})

    -- 空间都很紧：debug/release 都用 -Os + LTO
    --   * release：-O3 会把整条 USB 调用链内联进 .highcode，Flash/RAM 都装不下；
    --   * debug：-Og/-O0 装不进 Flash，所以也只加调试符号。
    if is_mode("debug") then
        set_symbols("debug")
    else
        set_symbols("hidden")
    end
    set_optimize("smallest")
    add_cxflags("-flto", {force = true})
end

local function add_map_file_hook()
    before_link(function (target)
        local mapfile = path.join(target:targetdir(), target:basename() .. ".map")
        target:add("ldflags", "-Wl,-Map=" .. mapfile, {force = true})
    end)
end

-- ===========================================================================
-- 调试器功能：CMSIS-DAP（固定编入）+ WCH-Link（可选）
-- ===========================================================================
-- 默认只编 CMSIS-DAP：WCH-Link 的代码占数 KiB Flash、数据占约 6 KiB RAM（overlay
-- 段），把这两样省下来才能放得下双串口（第二个 CDC + USART2 + 两路 FIFO）。
--
-- 传 --wchlink=y 得到「双模式」固件：两套调试器同编一份，运行中双击 PB3 切换
-- （见 src/main.c）。这种构建放不下 bootloader，自动退回应用 @0x0。

-- 用芯片出厂 UID（ESIG 区，64 位、每颗唯一）作为 CMSIS-DAP 的 USB 序列号。
option("esig_sn")
    set_default(true)
    set_showmenu(true)
    set_description("Use the chip ESIG UID (64-bit, unique per chip) as the USB serial number")
option_end()

-- SWD 引脚方案（对应 src/swd_pin_map.h 的 SWD_PIN_VARIANT）
option("swd_pins")
    set_default("pa5_pa6")
    set_values({"pa5_pa7", "pa5_pa6", "pa2_pa3"})
    set_showmenu(true)
    set_description("SWD pin variant: pa5_pa6 (default), pa5_pa7 or pa2_pa3 (legacy)")
option_end()

-- 是否把 WCH-Link（RVSWD）调试器一起编进固件
option("wchlink")
    set_default(false)
    set_showmenu(true)
    set_description("Also build the WCH-Link (RVSWD) debugger mode (dual-mode firmware, ~6 KiB RAM)")
option_end()

-- 是否按「UF2 bootloader 布局」构建（应用从 0x4000 起 + 同时构建 bootloader）
option("bootloader")
    set_default(true)
    set_showmenu(true)
    set_description("Build the UF2 bootloader and put the app at 0x4000 (auto-disabled with --wchlink=y)")
option_end()

-- ===========================================================================
-- target: bootloader（0x0000..0x3FFF）
-- ===========================================================================
if use_bootloader then
    target("bootloader")
        set_plat("cross")
        set_arch("riscv")
        set_kind("binary")
        set_languages("gnu11")
        set_toolchains("wch-riscv")
        set_warnings("all")
        set_targetdir("build/$(mode)")
        set_filename("bootloader.elf")

        -- bootloader 自己的源码（含虚拟 U 盘 boot_disk.c）
        add_files("bootloader/src/*.c")

        -- 与应用共用的启动握手区（src/boot_info.h）在这里必须真的读写那块 RAM；
        -- 不定义这个宏时头文件里的访问会编译成空操作。
        add_defines("UDAP_UF2_BOOTLOADER=1")

        -- 复用的应用侧公共源码。
        -- 不取 src/system_ch32x035.c：bootloader 用自己的最小 SystemInit
        -- （bootloader/src/boot_system.c），省掉 400 B 的 GPIO_IPD_Unused。
        add_files(app_src .. "/ch32x035_it.c",
                  app_src .. "/bsp/bsp_delay.c",
                  app_src .. "/bsp/bsp_print.c",
                  app_src .. "/bsp/bsp_uid.c",
                  app_src .. "/bsp/bsp_usb.c",
                  app_src .. "/port/usb_ch32x035_dc_usbfs.c")

        -- CH32X035 SDK（与应用一样排除 sdk/Debug/debug.c，它自带 _write/_sbrk）
        add_files(sdk .. "/Startup/startup_ch32x035_highcode.S",
                  sdk .. "/Core/*.c",
                  sdk .. "/Peripheral/src/*.c")

        -- CherryUSB：核心 + MSC 类（bootloader 只有 U 盘）
        add_files(third_party .. "/cherryusb/core/usbd_core.c",
                  third_party .. "/cherryusb/class/msc/usbd_msc.c")

        -- UF2 解析 + 虚拟 FAT16 全在 bootloader/src 里：
        --   boot_uf2.c  写路径（块校验/去重/入口页最后提交）
        --   boot_disk.c 只读 FAT16（主机枚举/列目录，只有 index.htm）

        -- 头文件搜索路径。**顺序有意义**：bootloader/src 必须排在最前，
        -- 它提供自己的 usb_config.h。
        add_includedirs("bootloader/src", app_src, {public = true})
        add_includedirs(uf2_include, {public = true})
        add_sysincludedirs(sdk .. "/Core", sdk .. "/Peripheral/inc", sdk .. "/Debug",
                           third_party .. "/cherryusb/core",
                           third_party .. "/cherryusb/common",
                           third_party .. "/cherryusb/class/msc")

        add_shared_flags({"-Wno-discarded-qualifiers"})

        -- bootloader 必须留在**机器模式**：它要跳进应用的 _start，而应用入口会
        -- 重跑 `csrw 0xbc0`（机器模式专用 CSR）。若 bootloader 的 startup 已按
        -- WCH 默认 (mstatus=0x88) 掉进用户模式，跳转后那条指令就是非法指令
        -- （实测 mcause=2 @handle_reset+0x80 → 无限复位）。见 startup 里的注释。
        add_asflags("-DSTART_KEEP_MACHINE_MODE", {force = true})
        add_ldflags(table.join(arch_flags, {
                       "-ffunction-sections",
                       "-fdata-sections",
                       "--specs=nano.specs",
                       "--specs=nosys.specs",
                       "-nostartfiles",
                       "-Wl,-T" .. path.join("bootloader", "ld", "Link_bootloader.ld"),
                       "-Wl,--gc-sections"}), {force = true})
        add_ldflags("-flto", {force = true})

        add_map_file_hook()

        before_build(function (target)
            local required = {"bootloader/src/main.c",
                              "bootloader/src/boot_config.h",
                              "bootloader/src/boot_disk.c",
                              "bootloader/src/boot_flash.c",
                              "bootloader/src/boot_uf2.c",
                              "bootloader/src/boot_usb.c",
                              "bootloader/ld/Link_bootloader.ld"}
            for _, req in ipairs(required) do
                if not os.isfile(req) then
                    os.raise("missing required source: " .. req)
                end
            end
            cprint("${cyan}bootloader:${clear} 16 KiB @0x0000 (UF2 drag-and-drop)")
        end)

        after_build(function (target)
            local toolchain = assert(target:toolchain("wch-riscv"))
            local prefix = toolchain:get("gcc_prefix")
            local targetfile = target:targetfile()
            local bindir = target:targetdir()
            local binfile = path.join(bindir, "bootloader.bin")

            os.execv(prefix .. "objcopy", {"-O", "binary", targetfile, binfile})

            -- 链接脚本里 FLASH 只有 16 KiB，超了 ld 会直接报错；这里再按 .bin
            -- 实际长度兜底检查一次（.bin 更接近真实占用）。
            local binsize = os.filesize(binfile)
            if binsize > 0x4000 then
                os.raise(string.format("bootloader.bin is %d B, exceeds the 16 KiB bootloader region",
                                    binsize))
            end
            print(string.format("Bootloader size: %d B / 16384 B (%.1f%% free)",
                                binsize, 100.0 * (16384 - binsize) / 16384.0))
        end)

        after_clean(function (target)
            local bindir = target:targetdir()
            os.rm(path.join(bindir, "bootloader.bin"))
            os.rm(path.join(bindir, target:basename() .. ".map"))
        end)
end

-- ===========================================================================
-- target: firmware（应用）
-- ===========================================================================
target("firmware")
    set_plat("cross")
    set_arch("riscv")
    set_kind("binary")
    set_languages("gnu11")
    set_toolchains("wch-riscv")
    set_warnings("all")
    set_targetdir("build/$(mode)")
    set_filename("firmware.elf")

    -- 先编 bootloader：上层脚本要在 build 结束后把两者合并成 firmware_full.bin
    -- （order = true 只保证顺序，不做链接依赖）
    if use_bootloader then
        add_deps("bootloader", {order = true})
    end

    -- 应用代码（源码都在 application/ 下，链接脚本在 application/ld/）
    -- application/*.c        系统初始化、中断、main（调试器模式：CMSIS-DAP [+ WCH-Link]）
    -- application/bsp/*.c    延时/打印/系统/USB 硬件初始化/UID
    -- application/port/*.c   CherryUSB 的 CH32X035 设备端口 glue
    add_files(app_src .. "/*.c", app_src .. "/bsp/*.c", app_src .. "/port/*.c")
    add_includedirs(app_src, app_src .. "/port")
    add_defines("ESIG_SN=" .. (esig_sn_enabled and 1 or 0))
    add_defines("WCHLINK_ENABLE=" .. (wchlink_enabled and 1 or 0))
    add_defines("UDAP_UF2_BOOTLOADER=" .. (use_bootloader and 1 or 0))

    -- SWD 引脚方案 -> SWD_PIN_VARIANT
    local swd_pins = get_config("swd_pins") or "pa5_pa6"
    add_defines("SWD_PIN_VARIANT=SWD_PIN_VARIANT_" .. string.upper(swd_pins))

    -- 串口驱动（全部串口相关代码都在这一对文件里，见 application/drv/drv_uart.h 的说明）
    --   COM1 <-> USART4 (PB0/PB1)     两模式都可用
    --   COM2 <-> USART2 (PA2/PA3)     仅双串口模式
    --   PA2/PA3 在单串口模式下是 DTR / RTS（低有效）
    --   两路 RX 都走 DMA（USART4=Ch8、USART2=Ch6），TX 用 TXE 中断
    --   （usbd_cdc_acm_* 回调由两种调试器模式共用，所以放在这里而不是各模式目录）
    add_files(app_src .. "/drv/drv_uart.c")
    add_sysincludedirs("../third_party/cherryrb")
    add_files("../third_party/cherryrb/chry_ringbuffer.c")

    -- 状态灯（PB11/PB12 的用途由 LED 模式决定，见 application/drv/drv_led.h）
    add_files(app_src .. "/drv/drv_led.c")

    -- 应用侧控制（vendor command 的延迟动作：切串口模式后重新枚举、进 UF2、复位）
    add_files(app_src .. "/drv/drv_control.c")

    -- 模式按键（双击切模式）+ 把模式持久化到用户选择字
    add_files(app_src .. "/drv/drv_mode_button.c", app_src .. "/drv/drv_mode_store.c")

    -- CMSIS-DAP 专有的应用 glue
    add_files(app_src .. "/dap/*.c")
    add_includedirs(app_src .. "/dap")

    -- 链接脚本（application/ld/）：
    --   双模式                    Link_highcode_nv_256B_dual.ld（应用 0x0 起，带 OVERLAY）
    --   单模式 + UF2 bootloader    Link_highcode_nv_256B_uf2.ld（应用 0x4000 起，
    --                             RAM 前 16 B 留给启动握手区）
    --   单模式、不要 bootloader    Link_highcode_nv_256B.ld（应用 0x0 起）
    local ld_dir = app_src .. "/ld"
    local linker_script
    if wchlink_enabled then
        linker_script = ld_dir .. "/Link_highcode_nv_256B_dual.ld"
    elseif use_bootloader then
        linker_script = ld_dir .. "/Link_highcode_nv_256B_uf2.ld"
    else
        linker_script = ld_dir .. "/Link_highcode_nv_256B.ld"
    end

    -- WCH-Link 专有的应用 glue（含无 LED / 无电源开关的空实现）
    if wchlink_enabled then
        add_files(app_src .. "/wchlink/**/*.c", app_src .. "/wchlink/**/*.S",
                  app_src .. "/status/*.c", app_src .. "/drv/drv_power_switch.c")
    end

    -- CH32X035 SDK
    -- 不编译 sdk/Debug/debug.c（它无条件定义 _write / _sbrk，会与
    -- application/bsp/bsp_print.c 的 retarget 重复定义），但保留 sdk/Debug 作为头文件路径。
    add_files(sdk .. "/Startup/startup_ch32x035_highcode.S", "sdk/Core/*.c", "sdk/Peripheral/src/*.c")
    add_includedirs("sdk/Core", "sdk/Peripheral/inc", "sdk/Debug")

    -- ===== USB：必选，不做任何条件裁剪 =====
    add_files("../third_party/cherryusb/core/usbd_core.c",
              "../third_party/cherryusb/class/cdc/usbd_cdc_acm.c")
    add_sysincludedirs("../third_party/cherryusb/core",
                       "../third_party/cherryusb/common",
                       "../third_party/cherryusb/class/cdc",
                       "../third_party/cherryusb/class/msc",
                       "../third_party/cherryusb/class/hid")

    -- ===== CMSIS-DAP =====
    -- 注意**不编译** DAP/Source/DAP_vendor.c：上游那份是空模板，本工程的 vendor
    -- 命令在 application/dap/udap_vendor.c（DAP.c 里的实现是 __WEAK，由我们覆盖）。
    add_files("../third_party/cherrydap/DAP/Source/DAP.c")
    add_sysincludedirs("../third_party/cherrydap",
                       "../third_party/cherrydap/DAP/Include")

    add_shared_flags()
    add_ldflags(table.join(arch_flags, {
                   "-ffunction-sections",
                   "-fdata-sections",
                   "--specs=nano.specs",
                   "--specs=nosys.specs",
                   "-nostartfiles",
                   "-Wl,-T" .. linker_script,
                   "-Wl,--gc-sections"}), {force = true})
    add_ldflags("-flto", {force = true})

    -- 打印当前模式、工具链与布局（before_build 是脚本作用域，可以用 raise）
    before_build(function (target)
        local required = {app_src .. "/board_pin_map.h",
                          app_src .. "/swd_pin_map.h",
                          app_src .. "/usb_config.h",
                          app_src .. "/boot_info.h",
                          app_src .. "/port/usb_ch32x035_dc_usbfs.c",
                          app_src .. "/bsp/bsp_usb.c",
                          app_src .. "/drv/drv_uart.c",
                          app_src .. "/drv/drv_uart.h",
                          app_src .. "/drv/drv_led.c",
                          app_src .. "/drv/drv_control.c",
                          app_src .. "/drv/drv_mode_button.c",
                          app_src .. "/drv/drv_mode_store.c",
                          app_src .. "/dap/DAP_config.h",
                          app_src .. "/dap/dap_main.c",
                          app_src .. "/dap/udap_vendor.c",
                          app_src .. "/dap/sw_dp.c",
                          app_src .. "/dap/swd_gpio.h",
                          app_src .. "/dap/swd_gpio_x035.c",
                          app_src .. "/bsp/bsp_uid.c"}
        if wchlink_enabled then
            table.insert(required, app_src .. "/wchlink/usb/wchlink_usb.c")
            table.insert(required, app_src .. "/wchlink/transport/rvswd_phy_gpio.c")
            table.insert(required, app_src .. "/wchlink/target/rvswd_target_registry.c")
            table.insert(required, app_src .. "/status/status_led.c")
            table.insert(required, app_src .. "/drv/drv_power_switch.c")
        end
        for _, req in ipairs(required) do
            if not os.isfile(req) then
                os.raise("missing required source: " .. req)
            end
        end

        local toolchain = assert(target:toolchain("wch-riscv"))
        cprint("${cyan}Using mode:${clear} %s", get_config("mode") or "debug")
        cprint("${cyan}Using toolchain:${clear} %s", toolchain:get("toolchain_root"))
        cprint("${cyan}Using SDK:${clear} sdk/")
        cprint("${cyan}SWD pins:${clear} %s", swd_pins)
        cprint("${cyan}Debugger:${clear} %s", wchlink_enabled
            and "CMSIS-DAP + WCH-Link (dual mode, double-click the button at runtime to switch)"
            or "CMSIS-DAP only (--wchlink=y to add WCH-Link)")
        cprint("${cyan}Layout:${clear} %s", use_bootloader
            and "UF2 bootloader + app at 0x4000 (drag firmware.uf2 to upgrade)"
            or (wchlink_enabled
                and "app at 0x0000 (dual-mode firmware does not fit behind the bootloader)"
                or "app at 0x0000 (no bootloader, SWD flashing only)"))
        cprint("${cyan}Serial #:${clear} %s", has_config("esig_sn")
            and "chip ESIG UID (unique)" or "fixed DEADBEEF")
    end)

    add_map_file_hook()

    -- 生成 bin、UF2 并统计 Flash 与 RAM 占用
    after_build(function (target)
        import("lib.detect.find_program")

        local toolchain = assert(target:toolchain("wch-riscv"))
        local prefix = toolchain:get("gcc_prefix")
        local targetfile = target:targetfile()
        local bindir = target:targetdir()

        local binfile = path.join(bindir, "firmware.bin")
        os.execv(prefix .. "objcopy", {"-O", "binary", targetfile, binfile})

        -- OVERLAY 段的 LMA 不受 FLASH 区域长度约束，超了 ld 也不报错，只会静默
        -- 生成一个装不进芯片的 .bin，所以按 .bin 实际长度兜底检查。
        local flash_limit = use_bootloader and (63232 - 16384) or 63232
        local binsize = os.filesize(binfile)
        if binsize > flash_limit then
            os.raise(string.format("firmware.bin is %d B, exceeds the %d B application region " ..
                                "(OVERLAY LMAs are not bounded by the FLASH region)",
                                binsize, flash_limit))
        end

        if use_bootloader then
            -- family ID 与起始地址必须和 bootloader/src/boot_config.h 一致
            local uf2_family = "0x75446170"
            local uf2_base = "0x4000"
            local uf2conv = path.join(os.scriptdir(), "..", "third_party", "uf2", "utils", "uf2conv.py")
            local uf2file = path.join(bindir, "firmware.uf2")
            local python = find_program("python") or find_program("py") or find_program("python3")
            if python and os.isfile(uf2conv) then
                if type(python) == "table" then
                    python = python.program
                end
                os.execv(python, {uf2conv, "-c", "-f", uf2_family, "-b", uf2_base,
                                  "-o", uf2file, binfile})
                cprint("${green}UF2:${clear} %s", uf2file)
            else
                cprint("${yellow}skip UF2:${clear} 找不到 python 或 third_party/uf2（先 git submodule update --init）")
            end
        else
            -- 非 bootloader 布局：删掉上次留下的 firmware.uf2（地址按 0x4000 算，拖进去没用）
            os.rm(path.join(bindir, "firmware.uf2"))
        end

        local sections_output = os.iorunv(prefix .. "size", {"--format=sysv", targetfile})
        local size_info = parse_berkeley_size(os.iorunv(prefix .. "size", {"--format=berkeley", targetfile}))
        local sections = parse_sysv_section_sizes(sections_output)

        print(sections_output)
        -- .highcode 在 SRAM 中执行；.stack 是 NOBITS
        size_info.ram = sum_section_sizes(sections, {".highcode", ".data", ".bss", ".stack"})
        -- 双模式的 overlay 段链接到**同一段地址**，只能取较大的那个
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

    after_clean(function (target)
        local bindir = target:targetdir()
        os.rm(path.join(bindir, "firmware.bin"))
        os.rm(path.join(bindir, "firmware.uf2"))
        os.rm(path.join(bindir, target:basename() .. ".map"))
        if use_bootloader then
            os.rm(path.join(bindir, "bootloader.bin"))
            os.rm(path.join(bindir, "bootloader.map"))
        end
    end)

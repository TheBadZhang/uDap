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
local linker_script = "sdk/Ld/Link_highcode_nv_256B.ld"

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

-- CMSIS-DAP（CherryDAP + src/dap、src/drv glue）是可选的：
--   xmake f --dap=n   # 只编 USB 骨架，不含调试器功能
-- USB 本身不可关：本芯片必然用到 USB。
option("dap")
    set_default(true)
    set_showmenu(true)
    set_description("Enable CMSIS-DAP (CherryDAP + src/dap + src/drv glue)")
option_end()

-- 用芯片出厂 UID（ESIG 区，96 位、每颗唯一）作为 USB 序列号。
-- 关闭时用固定的 "DEADBEEF" —— 多块板子同时插在一台机器上时无法区分。
-- 寄存器定义见 CH32X035 应用手册第 19 章「电子签名（ESIG）」。
-- 序列号在 src/dap/dap_main.c 里设置，所以依赖 CMSIS-DAP glue（--dap=y）。
option("esig_sn")
    set_default(false)
    set_showmenu(true)
    set_description("Use the chip ESIG UID (96-bit, unique per chip) as the USB serial number")
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

    local dap_enabled = has_config("dap")
    local esig_sn_enabled = has_config("esig_sn")

    -- 序列号在 src/dap/dap_main.c 中设置，而该文件只在启用 DAP 时才编译。
    -- 这个组合下选项会静默失效，所以直接报错而不是默默编出一个用不上它的固件。
    if esig_sn_enabled and not dap_enabled then
        raise("esig_sn requires dap: the USB serial number is set in src/dap/dap_main.c, " ..
              "which is only compiled with CMSIS-DAP " ..
              "(use `xmake f --dap=y` or `--esig_sn=n`)")
    end

    -- 本工程应用代码
    -- src/*.c        系统初始化、中断、main（main 内用 ENABLE_DAP 区分两条路径）
    -- src/bsp/*.c    延时/打印/系统/USB 硬件初始化
    -- src/port/*.c   CherryUSB 的 CH32X035 设备端口 glue
    add_files("src/*.c", "src/bsp/*.c", "src/port/*.c")
    add_includedirs("src")
    add_defines("ENABLE_DAP=" .. (dap_enabled and 1 or 0))
    add_defines("ESIG_SN=" .. (esig_sn_enabled and 1 or 0))

    -- CMSIS-DAP 专有的应用 glue，仅在启用 DAP 时参与编译
    if dap_enabled then
        add_files("src/dap/*.c", "src/drv/*.c")
        add_includedirs("src/dap")
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
    -- （src/bsp/bsp_usb.c）缺一不可。
    -- 缺文件时直接 fail，避免默默编出一个没有 USB 的固件。
    for _, req in ipairs({"src/usb_config.h",
                          "src/port/usb_ch32x035_dc_usbfs.c",
                          "src/bsp/bsp_usb.c"}) do
        if not os.isfile(req) then
            raise("USB is mandatory: missing " .. req)
        end
    end

    -- CherryUSB：上游核心 + CDC ACM 类，端口 glue 在 src/port/
    add_files("third_party/cherryusb/core/usbd_core.c",
              "third_party/cherryusb/class/cdc/usbd_cdc_acm.c")
    add_sysincludedirs("third_party/cherryusb/core",
                       "third_party/cherryusb/common",
                       "third_party/cherryusb/class/cdc",
                       "third_party/cherryusb/class/msc",
                       "third_party/cherryusb/class/hid")

    -- ===== CMSIS-DAP：可选（xmake f --dap=n 关闭） =====
    -- 注意：不要在这个分支里打印启用/禁用状态。`xmake f` 会把 xmake.lua 求值两次，
    -- 配置阶段读到的 dap 值可能还是旧值，会打出与实际相反的结论。
    -- 权威状态统一在下面的 before_build 里输出。
    if dap_enabled then
        for _, req in ipairs({"src/dap/DAP_config.h",
                              "src/dap/dap_main.c",
                              "src/dap/sw_dp.c",
                              "src/drv/drv_uart.c"}) do
            if not os.isfile(req) then
                raise("DAP is enabled but missing " .. req ..
                      " (run `xmake f --dap=n` to disable CMSIS-DAP)")
            end
        end

        -- CherryDAP（上游 CMSIS-DAP 实现）
        add_files("third_party/cherrydap/DAP/Source/DAP.c",
                  "third_party/cherrydap/DAP/Source/DAP_vendor.c")
        add_sysincludedirs("third_party/cherrydap",
                           "third_party/cherrydap/DAP/Include")

        -- CherryRB（CDC <-> USART 桥的收发 FIFO，只有 DAP glue 用到）
        add_files("third_party/cherryrb/chry_ringbuffer.c")
        add_sysincludedirs("third_party/cherryrb")
    end

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
    if is_mode("debug") then
        set_symbols("debug")
        set_optimize("none")
        add_cxflags("-Og", {force = true})
    else
        set_symbols("hidden")
        set_optimize("fastest")
        add_cxflags("-flto", {force = true})
        add_ldflags("-flto", {force = true})
    end

    -- 构建钩子，按触发顺序排列

    -- 打印当前模式、工具链与 SDK 目录
    before_build(function (target)
        local toolchain = assert(target:toolchain("wch-riscv"))
        local toolchain_root = toolchain:get("toolchain_root")
        cprint("${cyan}Using mode:${clear} %s", get_config("mode") or "debug")
        cprint("${cyan}Using toolchain:${clear} %s", toolchain_root)
        cprint("${cyan}Using SDK:${clear} sdk/")
        cprint("${cyan}CMSIS-DAP:${clear} %s", has_config("dap") and "enabled" or "disabled")
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

        os.execv(prefix .. "objcopy", {"-O", "binary", targetfile, path.join(bindir, "firmware.bin")})

        local sections_output = os.iorunv(prefix .. "size", {"--format=sysv", targetfile})
        local size_info = parse_berkeley_size(os.iorunv(prefix .. "size", {"--format=berkeley", targetfile}))
        local sections = parse_sysv_section_sizes(sections_output)

        print(sections_output)
        -- .highcode 在 SRAM 中执行；.stack 是 NOBITS，不应依赖 Berkeley 分类推断 RAM 占用
        size_info.ram = sum_section_sizes(sections, {".highcode", ".data", ".bss", ".stack"})
        print_memory_summary(size_info)
    end)

    -- 清理生成的 bin 与 map
    after_clean(function (target)
        local bindir = target:targetdir()
        os.rm(path.join(bindir, "firmware.bin"))
        os.rm(path.join(bindir, target:basename() .. ".map"))
    end)

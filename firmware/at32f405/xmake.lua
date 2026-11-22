--!A cross-platform build utility based on Lua
--
--  uDap —— AT32F405KBU7 版（CherryUSB + CherryRB + CherryDAP）
--
--  常用命令：
--    xmake                          构建（默认 target = firmware）
--    xmake run firmware             构建 + 烧录 + 运行（外部 DAPLink + pyocd）
--    xmake flash -t firmware        同上（任务形式）
--    xmake erase                    整片擦除
--    xmake reset                    复位目标
--    xmake f --flasher=none         只编译不烧录
--    xmake f --swd_phy=spi          切换 SWD 物理层实现（gpio / spi）
--
--  工具链：GNU Arm Embedded Toolchain。若 arm-none-eabi-gcc 在 PATH 里则无需配置，
--  否则 xmake f --sdk=<gcc 根目录> --cross=arm-none-eabi- -y。
--
set_project("udap-at32f405")
set_version("1.0.0")
set_xmakever("3.0.0")

set_plat("cross")
set_arch("arm")

set_defaultplat("cross")
set_defaultarchs("arm")

set_policy("build.linker.output", true)

-- ============================================================================
-- 工具链自动探测（与 AT32_SDK 里的模板一致）
-- ============================================================================
local function _find_in_path(toolname)
    local exts = is_host("windows") and {".exe", ""} or {""}
    local sep = is_host("windows") and ";" or ":"
    local pathenv = os.getenv("PATH") or ""
    for dir in pathenv:gmatch("[^" .. sep .. "]+") do
        for _, ext in ipairs(exts) do
            local filepath = path.join(dir, toolname .. ext)
            if os.isfile(filepath) then
                return filepath
            end
        end
    end
end

local function _detect_gcc_arm_root()
    for _, envname in ipairs({"GCC_ARM_HOME", "ARMGCC_DIR"}) do
        local dir = os.getenv(envname)
        if dir and os.isdir(dir) then
            return dir
        end
    end
    local gcc = _find_in_path("arm-none-eabi-gcc")
    if gcc then
        return path.directory(path.directory(gcc))
    end
end

if not get_config("cross") then
    set_config("cross", "arm-none-eabi-")
end

if not get_config("sdk") then
    local gccroot = _detect_gcc_arm_root()
    if gccroot then
        set_config("sdk", gccroot)
    else
        os.raise("未找到 arm-none-eabi-gcc，请加入 PATH 或 xmake f --sdk=<gcc 根目录> --cross=arm-none-eabi- -y")
    end
end

-- ============================================================================
-- 可配置项
-- ============================================================================
option("device")
    set_default("AT32F405KBU7_4")
    set_showmenu(true)
    set_description("目标芯片型号（AT32F405KBU7_4 = 128K Flash / 70K RAM）")

option("swd_phy")
    set_default("gpio")
    set_values({"gpio", "gpio_fast", "spi"})
    set_showmenu(true)
    set_description("SWD 物理层：gpio（位翻转，27 MHz 档自动切 RAM 汇编内核）| spi（SPI1 硬件移位）；gpio_fast 是 gpio 的等价别名")

option("opt")
    set_default("size")
    set_values({"size", "speed"})
    set_showmenu(true)
    set_description("优化目标：size（-Os，默认，128K Flash 只有 70K RAM 的公司里最稳）| speed（-O2）")

option("flasher")
    set_default("pyocd")
    set_values({"pyocd", "jlink", "none"})
    set_showmenu(true)
    set_description("烧录工具：pyocd（外部 DAPLink）| jlink | none")

option("pyocdexe")
    set_default("pyocd")
    set_showmenu(true)
    set_description("pyocd 可执行文件")

option("pyocdtarget")
    set_default("at32f405kbu7_4")
    set_showmenu(true)
    set_description("pyocd target 名")

option("packfile")
    set_default("misc/ArteryTek.AT32F402_405_DFP.2.1.6.pack")
    set_showmenu(true)
    set_description("CMSIS pack（提供 pyocd 的 flash 算法与 target 定义）")

-- ============================================================================
-- 路径与公共参数
-- ============================================================================
local projectdir = os.projectdir()
local third_party = path.join(projectdir, "..", "third_party")

local device = get_config("device")
if not device then
    device = "AT32F405KBU7_4"
end

-- B = 128K/70K，C = 256K/102K（AT32F402/405 的容量档位在型号第 10 个字符）
local cap = device:sub(10, 10)
local linker_script
if cap == "B" then
    linker_script = path.join("startup", "linker", "at32f405_flash_128k_70k.ld")
elseif cap == "C" then
    linker_script = path.join("startup", "linker", "at32f405_flash_256k_102k.ld")
else
    os.raise("无法从型号 " .. device .. " 推断 Flash 容量（第 10 个字符应为 B 或 C）")
end

-- 优化级别交给 xmake 的 set_optimize()（见各 target），这里只放跨实现必须一致的
-- 机器相关/分段参数。LTO 打开后，热点函数会被跨文件内联，体积会大一点点、速度
-- 会快一点；fat objects 不开（省编译时间），预编译的 libc/libgcc 由链接插件处理。
local lto_flags = {"-flto"}

local gcc_flags = {
    "-mcpu=cortex-m4",
    "-mthumb",
    "-mfloat-abi=soft",
    "-g",
    "-ffunction-sections",
    "-fdata-sections",
    "-fno-common",
}

local warn_flags = {
    "-Wall",
    "-Wextra",
    "--specs=nano.specs",
}

local ld_flags = {
    "-mcpu=cortex-m4",
    "-mthumb",
    "-mfloat-abi=soft",
    "-T", linker_script,
    "-Wl,--gc-sections",
    "-Wl,--print-memory-usage",
    "--specs=nano.specs",
    "--specs=nosys.specs",
    "-nostartfiles",
    "-Wl,--start-group",
    "-lc", "-lm", "-lgcc",
    "-Wl,--end-group",
}

-- ============================================================================
-- 共享的文件/头文件/宏
-- ============================================================================
local sdk_includes = {
    path.join(projectdir, "sdk", "cmsis", "core"),
    path.join(projectdir, "sdk", "cmsis", "device", "include"),
    path.join(projectdir, "sdk", "drivers", "inc"),
    path.join(projectdir, "sdk", "include"),
    path.join(projectdir, "sdk", "bsp"),
}

local app_includes = {
    path.join(projectdir, "application"),
    path.join(projectdir, "application", "bsp"),
    path.join(projectdir, "application", "drv"),
    path.join(projectdir, "application", "dap"),
}

local usb_includes = {
    path.join(third_party, "cherryusb", "core"),
    path.join(third_party, "cherryusb", "common"),
    path.join(third_party, "cherryusb", "class", "cdc"),
    path.join(third_party, "cherryusb", "class", "hid"),
    path.join(third_party, "cherryusb", "class", "hub"),
    path.join(third_party, "cherryusb", "class", "msc"),
    path.join(third_party, "cherryusb", "class", "vendor"),
    path.join(third_party, "cherryusb", "port", "dwc2"),
    path.join(third_party, "cherrydap"),
    path.join(third_party, "cherrydap", "DAP", "Include"),
    path.join(third_party, "cherryrb"),
}

-- ============================================================================
-- target: firmware
-- ============================================================================
target("firmware")
    set_kind("binary")
    set_optimize((get_config("opt") == "speed") and "fastest" or "smallest")
    set_targetdir(path.join("$(builddir)", "bin", "firmware"))
    set_basename("firmware.elf")
    set_filename("firmware.elf")

    add_files(path.join(projectdir, "startup", "startup_at32f405.s"))
    add_files(path.join(projectdir, "sdk", "cmsis", "device", "source", "system_at32f402_405.c"))
    add_files(path.join(projectdir, "sdk", "drivers", "src", "*.c"))
    add_files(path.join(projectdir, "sdk", "bsp", "at32f405_clock.c"))
    add_files(path.join(projectdir, "sdk", "bsp", "bsp_delay.c"))
    add_files(path.join(projectdir, "sdk", "bsp", "syscalls.c"))

    add_files(path.join(projectdir, "application", "*.c"))
    add_files(path.join(projectdir, "application", "bsp", "*.c"))
    add_files(path.join(projectdir, "application", "drv", "*.c"))
    add_files(path.join(projectdir, "application", "dap", "*.c"))
    -- SWD 位翻转的汇编内核（.ramcode 段，运行时在 RAM 里执行）
    add_files(path.join(projectdir, "application", "dap", "swd_fast_gpio.S"))

    add_files(path.join(third_party, "cherryusb", "core", "usbd_core.c"))
    add_files(path.join(third_party, "cherryusb", "class", "cdc", "usbd_cdc_acm.c"))
    add_files(path.join(third_party, "cherryusb", "port", "dwc2", "usb_dc_dwc2.c"))
    add_files(path.join(third_party, "cherryusb", "port", "dwc2", "usb_glue_at.c"))
    add_files(path.join(third_party, "cherrydap", "DAP", "Source", "DAP.c"))
    add_files(path.join(third_party, "cherrydap", "DAP", "Source", "JTAG_DP.c"))
    add_files(path.join(third_party, "cherryrb", "chry_ringbuffer.c"))

    -- SWD 物理层三选一：gpio 用 cherrydap 的原版位翻转实现，gpio_fast 用
    -- 我们自己的汇编内核 + RAM 常驻（application/dap/swd_gpio_fast.c，
    -- 内核在 swd_fast_gpio.S），spi 用 application/dap/swd_spi.c。
    -- 三者都提供 SWJ_Sequence/SWD_Sequence/SWD_Transfer，同时编会符号冲突，
    -- 所以这里必须互斥（后两者已经在 application/dap/*.c 通配里，靠宏自己关掉）。
    -- gpio 与 gpio_fast 已合并成一个实现（application/dap/swd_gpio_fast.c）：
    -- 快路径（请求到 27 MHz 档，DAP_Data.fast_clock=1）走 swd_fast_gpio.S 的
    -- RAM 常驻汇编；其余档位转发给 cherrydap 原版 SW_DP.c（用 -D 改名为 *Ref，
    -- 这样两份实现可以同时链接，行为与上游逐字一致）。
    if get_config("swd_phy") ~= "spi" then
        add_files(path.join(third_party, "cherrydap", "DAP", "Source", "SW_DP.c"), {
            defines = {
                "SWD_Transfer=SWD_TransferRef",
                "SWJ_Sequence=SWJ_SequenceRef",
                "SWD_Sequence=SWD_SequenceRef",
            },
        })
    end

    add_includedirs(sdk_includes, {public = true})
    add_includedirs(app_includes)
    add_includedirs(usb_includes, {public = true})

    add_defines(device)
    add_defines("AT32_CLOCK_USE_HEXT=1")
    add_defines("UART_ENABLE=1")
    add_defines("ESIG_SN=1")
    add_defines("UDAP_UF2_BOOTLOADER=1")
    if get_config("swd_phy") == "spi" then
        add_defines("SWD_PHY_SPI=1")
    else
        add_defines("SWD_PHY_GPIO_FAST=1")
    end

    add_cflags(gcc_flags, {force = true})
    add_cflags(warn_flags, {force = true})
    add_cflags(lto_flags, {force = true})
    add_asflags({"-mcpu=cortex-m4", "-mthumb"}, {force = true})
    add_ldflags(ld_flags, {force = true})
    add_ldflags(lto_flags, {force = true})
    add_ldflags({"-Wl,-Map=" .. path.join("$(builddir)", "bin", "firmware", "firmware.map")}, {force = true})

    -- 生成 .bin / .hex / .lst
    after_build(function(target)
        local function run(cmd, args, retries)
            retries = retries or 3
            for i = 1, retries do
                local ok = try {
                    function()
                        os.execv(cmd, args)
                        return true
                    end,
                    catch {
                        function(errors)
                            return false
                        end
                    }
                }
                if ok then
                    return true
                end
                os.sleep(200)
            end
            return false
        end

        local elf = target:targetfile()
        local dir = path.directory(elf)
        local base = "firmware"
        local bin = path.join(dir, base .. ".bin")
        local hex = path.join(dir, base .. ".hex")
        local lst = path.join(dir, base .. ".lst")

        if not run("arm-none-eabi-objcopy", {"-O", "binary", elf, bin}) then
            raise("生成 .bin 失败")
        end
        if not run("arm-none-eabi-objcopy", {"-O", "ihex", elf, hex}) then
            raise("生成 .hex 失败")
        end

        local out = os.iorunv("arm-none-eabi-objdump", {"-d", "-S", "-l", "--no-show-raw-insn", elf})
        if out then
            io.writefile(lst, out)
        end

        local size = os.iorunv("arm-none-eabi-size", {elf})
        if size then
            print(size)
        end
    end)

    -- 烧录（外部 DAPLink + pyocd，或 J-Link）
    on_run(function(target)
        local flasher = get_config("flasher") or "pyocd"
        local dir = path.directory(target:targetfile())
        local hex = path.join(dir, "firmware.hex")

        if flasher == "none" then
            print("flasher=none，跳过烧录")
            return
        end

        if flasher == "jlink" then
            local jlinkexe = get_config("jlinkexe") or "JLink.exe"
            local script = path.join(os.projectdir(), "build", "flash.jlink")
            io.writefile(script, "erase\nloadfile " .. hex .. "\nr\ng\nexit\n")
            os.execv(jlinkexe, {"-device", device, "-if", "SWD", "-speed", "4000", "-CommanderScript", script})
            return
        end

        local pyocd = get_config("pyocdexe") or "pyocd"
        local pack = path.absolute(get_config("packfile") or "misc/ArteryTek.AT32F402_405_DFP.2.1.6.pack")
        local pyocd_target = get_config("pyocdtarget") or "at32f405kbu7_4"

        os.execv(pyocd, {"erase", "-c", "-t", pyocd_target, "-f", "2000000", "--pack", pack}, {try = true})
        os.execv(pyocd, {"load", "-t", pyocd_target, "-f", "2000000", "--pack", pack, hex})
        print("[ok] " .. hex .. " 已烧录（pyocd target=" .. pyocd_target .. "）")
    end)

-- ============================================================================
-- target: bootloader（0x08000000 起 16 KiB，应用从 0x08004000 开始）
-- ============================================================================
local boot_linker_script = path.join("bootloader", "bootloader_16k.ld")

local boot_ld_flags = {
    "-mcpu=cortex-m4",
    "-mthumb",
    "-mfloat-abi=soft",
    "-T", boot_linker_script,
    "-Wl,--gc-sections",
    "-Wl,--print-memory-usage",
    "--specs=nano.specs",
    "--specs=nosys.specs",
    "-nostartfiles",
    "-Wl,--start-group",
    "-lc", "-lm", "-lgcc",
    "-Wl,--end-group",
}

target("bootloader")
    set_kind("binary")
    -- 固定 -Os：--opt=speed（-O2）会让 bootloader 涨到 18.8 KiB，超过 16 KiB 分区
    set_optimize("smallest")
    set_targetdir(path.join("$(builddir)", "bin", "bootloader"))
    set_basename("bootloader.elf")
    set_filename("bootloader.elf")

    add_files(path.join(projectdir, "startup", "startup_at32f405.s"))
    add_files(path.join(projectdir, "sdk", "cmsis", "device", "source", "system_at32f402_405.c"))
    add_files(path.join(projectdir, "sdk", "drivers", "src", "*.c"))
    add_files(path.join(projectdir, "sdk", "bsp", "at32f405_clock.c"))
    add_files(path.join(projectdir, "sdk", "bsp", "bsp_delay.c"))
    add_files(path.join(projectdir, "sdk", "bsp", "syscalls.c"))

    add_files(path.join(projectdir, "bootloader", "*.c"))

    add_files(path.join(third_party, "cherryusb", "core", "usbd_core.c"))
    add_files(path.join(third_party, "cherryusb", "class", "dfu", "usbd_dfu.c"))
    add_files(path.join(third_party, "cherryusb", "port", "dwc2", "usb_dc_dwc2.c"))
    add_files(path.join(third_party, "cherryusb", "port", "dwc2", "usb_glue_at.c"))

    -- bootloader 目录在最前：bootloader 自带 usb_config.h（EP0 缓冲 1024）
    add_includedirs(path.join(projectdir, "bootloader"))
    add_includedirs(sdk_includes, {public = true})
    add_includedirs(usb_includes, {public = true})
    add_includedirs(path.join(third_party, "cherryusb", "class", "dfu"), {public = true})

    add_defines(device)
    add_defines("AT32_CLOCK_USE_HEXT=1")

    add_cflags(gcc_flags, {force = true})
    add_cflags(warn_flags, {force = true})
    add_cflags(lto_flags, {force = true})
    add_asflags({"-mcpu=cortex-m4", "-mthumb"}, {force = true})
    add_ldflags(boot_ld_flags, {force = true})
    add_ldflags(lto_flags, {force = true})
    add_ldflags({"-Wl,-Map=" .. path.join("$(builddir)", "bin", "bootloader", "bootloader.map")}, {force = true})

    -- 生成 .bin / .hex，并检查 16 KiB 上限（超了立刻报错，别等烧录才发现撞应用）
    after_build(function(target)
        local elf = target:targetfile()
        local dir = path.directory(elf)
        local bin = path.join(dir, "bootloader.bin")
        local hex = path.join(dir, "bootloader.hex")

        local ok = try {
            function()
                os.execv("arm-none-eabi-objcopy", {"-O", "binary", elf, bin})
                os.execv("arm-none-eabi-objcopy", {"-O", "ihex", elf, hex})
                return true
            end,
            catch {
                function(errors)
                    return false
                end
            }
        }
        if not ok then
            raise("生成 bootloader.bin / bootloader.hex 失败")
        end

        local size = os.iorunv("arm-none-eabi-size", {elf})
        if size then
            print(size)
        end

        local fsize = os.filesize(bin)
        if fsize and fsize > 0x4000 then
            raise(string.format("bootloader.bin = %d 字节，超过 16 KiB（0x4000）上限！", fsize))
        end
        print(string.format("[ok] %s（%d / 16384 字节）", bin, fsize or 0))
    end)

    -- 烧录 bootloader：整片擦除 + 写 bootloader.hex（之后还要单独烧应用，见 README）
    on_run(function(target)
        local flasher = get_config("flasher") or "pyocd"
        local dir = path.directory(target:targetfile())
        local hex = path.join(dir, "bootloader.hex")

        if flasher == "none" then
            print("flasher=none，跳过烧录")
            return
        end

        local pyocd = get_config("pyocdexe") or "pyocd"
        local pack = path.absolute(get_config("packfile") or "misc/ArteryTek.AT32F402_405_DFP.2.1.6.pack")
        local pyocd_target = get_config("pyocdtarget") or "at32f405kbu7_4"

        os.execv(pyocd, {"erase", "-c", "-t", pyocd_target, "-f", "2000000", "--pack", pack}, {try = true})
        os.execv(pyocd, {"load", "-t", pyocd_target, "-f", "2000000", "--pack", pack, hex})
        print("[ok] " .. hex .. " 已烧录（注意：整片擦除会清掉应用，需再烧 firmware）")
    end)

-- ============================================================================
-- 任务
-- ============================================================================
task("flash")
    set_category("plugin")
    set_menu({
        usage = "xmake flash [options]",
        description = "构建并烧录 firmware",
        options = {
            {"t", "target", "kv", "firmware", "要烧录的 target 名"},
        }
    })
    on_run(function()
        local name = option.get("target") or "firmware"
        os.exec("xmake build " .. name)
        os.exec("xmake run " .. name)
    end)

task("erase")
    set_category("plugin")
    set_menu({
        usage = "xmake erase",
        description = "整片擦除（pyocd）",
        options = {}
    })
    on_run(function()
        local pyocd = get_config("pyocdexe") or "pyocd"
        local pack = path.absolute(get_config("packfile") or "misc/ArteryTek.AT32F402_405_DFP.2.1.6.pack")
        local pyocd_target = get_config("pyocdtarget") or "at32f405kbu7_4"
        os.execv(pyocd, {"erase", "-c", "-t", pyocd_target, "-f", "2000000", "--pack", pack})
    end)

task("reset")
    set_category("plugin")
    set_menu({
        usage = "xmake reset",
        description = "复位目标（pyocd）",
        options = {}
    })
    on_run(function()
        local pyocd = get_config("pyocdexe") or "pyocd"
        local pack = path.absolute(get_config("packfile") or "misc/ArteryTek.AT32F402_405_DFP.2.1.6.pack")
        local pyocd_target = get_config("pyocdtarget") or "at32f405kbu7_4"
        os.execv(pyocd, {"reset", "-t", pyocd_target, "-f", "2000000", "--pack", pack})
    end)

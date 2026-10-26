# OpenOCD SWD memory throughput test.
#
# Run after loading an interface/target configuration, for example:
#   openocd -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg \
#     -c "set speed_khz 30000; set test_addr 0x20000000; set test_bytes 32768; set rounds 10" \
#     -c "source [find tools/openocd_speed_test.tcl]"
#
# The test area must be writable target RAM. It is backed up, temporarily
# overwritten during the test, then restored before the target is resumed.
#
# ---------------------------------------------------------------------------
# 为什么读用 dump_image 而不是 read_memory
# ---------------------------------------------------------------------------
# `read_memory ... <count>` 会把读到的每个 word 拼成一个 TCL 列表返回，构造成本
# **计在计时窗口内**，而且大块时是超线性的：实测定制 OpenOCD（含异步 libusb 补丁）
# 读 64 KiB 用 read_memory 要 3.18 s（≈20 KiB/s），同一次读改用 dump_image 只要
# 0.39 s（≈166 KiB/s，含进程启动）—— 也就是说 20 KiB/s 里绝大部分是 TCL 列表
# 开销，不是 SWD/USB 的吞吐。
#
# 所以计时窗口里读用 dump_image（写文件，不构造列表）；写仍用 write_memory
# （列表只解析一次，实测写吞吐与 pyOCD 一致）。校验改为「文件大小 + 抽样前 4 个
# word」，避免把逐字比较的 TCL 开销又算进读时间。

if {![info exists speed_khz]}  { set speed_khz 6000 }
if {![info exists test_addr]}  { set test_addr 0x20000000 }
if {![info exists test_bytes]} { set test_bytes 32768 }
if {![info exists rounds]}     { set rounds 10 }
if {![info exists warmup_rounds]} { set warmup_rounds 1 }

if {$test_bytes < 4 || ($test_bytes % 4) != 0} {
    echo "test_bytes must be a positive multiple of 4"
    shutdown error
}
if {$rounds < 1} {
    echo "rounds must be greater than zero"
    shutdown error
}
if {$warmup_rounds < 0} {
    echo "warmup_rounds cannot be negative"
    shutdown error
}

adapter speed $speed_khz
init
halt

# 临时文件放到系统临时目录（都没有就退回当前目录）
if {[info exists ::env(TEMP)]} {
    set _tmpdir $::env(TEMP)
} elseif {[info exists ::env(TMP)]} {
    set _tmpdir $::env(TMP)
} else {
    set _tmpdir "."
}
set _dump_file [file join $_tmpdir "ocd_speed_read.bin"]
set _backup_file [file join $_tmpdir "ocd_speed_backup.bin"]

set words [expr {$test_bytes / 4}]
set pattern {}
for {set i 0} {$i < $words} {incr i} {
    # A changing pattern catches address/data-line and burst-order mistakes.
    lappend pattern [format "0x%08x" [expr {0xa5a50000 | ($i & 0xffff)}]]
}

# Preserve the selected RAM range so the test does not change the paused program.
if {[catch {dump_image $_backup_file $test_addr $test_bytes} err]} {
    echo "cannot back up test RAM: $err"
    resume
    shutdown error
}

set passed 0
set write_samples 0
set read_samples 0
set write_total_us 0
set read_total_us 0

# Bring the CMSIS-DAP command queue and target AP into steady state.  The
# warmup transfer is deliberately outside the reported timing window.
for {set r 0} {$r < $warmup_rounds} {incr r} {
    if {[catch {write_memory $test_addr 32 $pattern} err]} {
        echo "warmup write failed: $err"
        catch {load_image $_backup_file $test_addr bin}
        resume
        shutdown error
    }
    if {[catch {dump_image $_dump_file $test_addr $test_bytes} err]} {
        echo "warmup read failed: $err"
        catch {load_image $_backup_file $test_addr bin}
        resume
        shutdown error
    }
}

for {set r 0} {$r < $rounds} {incr r} {
    set t0 [clock microseconds]
    if {[catch {write_memory $test_addr 32 $pattern} err]} {
        echo "write round [expr {$r + 1}] failed: $err"
        continue
    }
    incr write_samples
    set write_total_us [expr {$write_total_us + [clock microseconds] - $t0}]

    set t0 [clock microseconds]
    if {[catch {dump_image $_dump_file $test_addr $test_bytes} err]} {
        echo "read round [expr {$r + 1}] failed: $err"
        continue
    }
    incr read_samples
    set read_total_us [expr {$read_total_us + [clock microseconds] - $t0}]

    # 抽样校验：文件大小 + 前 4 个 word
    set ok 0
    if {[file size $_dump_file] == $test_bytes} {
        set ok 1
        set got [read_memory $test_addr 32 4]
        for {set i 0} {$i < 4 && $i < $words} {incr i} {
            if {[expr {[lindex $got $i] != [lindex $pattern $i]}]} {
                set ok 0
                break
            }
        }
    }
    if {$ok} {
        incr passed
    } else {
        echo "data mismatch in round [expr {$r + 1}]"
    }
}

set write_kib 0.0
set read_kib 0.0
if {$write_total_us > 0} {
    set write_kib [expr {($test_bytes * $write_samples * 1000000.0) / ($write_total_us * 1024.0)}]
}
if {$read_total_us > 0} {
    set read_kib [expr {($test_bytes * $read_samples * 1000000.0) / ($read_total_us * 1024.0)}]
}

if {[catch {load_image $_backup_file $test_addr bin} err]} {
    echo "failed to restore test RAM: $err"
    shutdown error
}

catch {file delete $_dump_file}
catch {file delete $_backup_file}

echo "================ Speed Test Summary ================"
echo [format "Adapter Request Speed: %d kHz" $speed_khz]
echo [format "Test Block           : %d KiB" [expr {$test_bytes / 1024}]]
echo [format "Passed Rounds        : %d / %d" $passed $rounds]
echo [format "Write Samples        : %d" $write_samples]
echo [format "Write Avg Speed      : %.3f KiB/s" $write_kib]
echo [format "Read Samples         : %d" $read_samples]
echo [format "Read Avg Speed       : %.3f KiB/s" $read_kib]
echo "======================================================"
resume
shutdown

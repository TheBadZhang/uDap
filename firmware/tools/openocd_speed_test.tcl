# OpenOCD SWD memory throughput test.
#
# Run after loading an interface/target configuration, for example:
#   openocd -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg \
#     -c "set speed_khz 30000; set test_addr 0x20000000; set test_bytes 32768; set rounds 10" \
#     -c "source [find tools/openocd_speed_test.tcl]"
#
# The test area must be writable target RAM. It is backed up, temporarily
# overwritten during the test, then restored before the target is resumed.

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

set words [expr {$test_bytes / 4}]
set pattern {}
for {set i 0} {$i < $words} {incr i} {
    # A changing pattern catches address/data-line and burst-order mistakes.
    lappend pattern [format "0x%08x" [expr {0xa5a50000 | ($i & 0xffff)}]]
}

# Preserve the selected RAM range so the test does not change the paused program.
if {[catch {set saved [read_memory $test_addr 32 $words]} err]} {
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
        catch {write_memory $test_addr 32 $saved}
        resume
        shutdown error
    }
    if {[catch {read_memory $test_addr 32 $words} err]} {
        echo "warmup read failed: $err"
        catch {write_memory $test_addr 32 $saved}
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
    if {[catch {set got [read_memory $test_addr 32 $words]} err]} {
        echo "read round [expr {$r + 1}] failed: $err"
        continue
    }
    incr read_samples
    set read_total_us [expr {$read_total_us + [clock microseconds] - $t0}]

    set ok [expr {[llength $got] == $words}]
    if {$ok} {
        for {set i 0} {$i < $words} {incr i} {
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

if {[catch {write_memory $test_addr 32 $saved} err]} {
    echo "failed to restore test RAM: $err"
    shutdown error
}

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

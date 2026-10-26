# 测 STM32F4 的 flash 烧录速度：erase / program / verify 三段分别计时。
#
# 用法（外部先设好变量）：
#   openocd -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg \
#           -c "adapter speed 6000" \
#           -c "set BINFILE {C:/tmp/x.bin}" -c "set BINADDR 0x08000000" \
#           -c "set LASTSEC 4" \
#           -f flash_speed_test.tcl
#
# 计时用 OpenOCD 自带的 ms（自 openocd 启动起的毫秒数，1 ms 分辨率）。
# 128 KiB 的擦除/写在几十 ms ~ 几秒量级，1 ms 足够。

if {![info exists BINADDR]} { set BINADDR 0x08000000 }
if {![info exists LASTSEC]} { set LASTSEC 4 }

init
halt
flash probe 0

echo "===== 开始 ====="

set t0 [ms]
flash erase_sector 0 0 $LASTSEC
set t1 [ms]

flash write_image $BINFILE $BINADDR bin
set t2 [ms]

verify_image $BINFILE $BINADDR bin
set t3 [ms]

echo [format "RESULT erase_ms=%d program_ms=%d verify_ms=%d total_ms=%d" \
      [expr {$t1 - $t0}] [expr {$t2 - $t1}] [expr {$t3 - $t2}] [expr {$t3 - $t0}]]

shutdown

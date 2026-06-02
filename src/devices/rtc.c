/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * Goldfish RTC 设备模型。
 *
 * Linux 可通过该 MMIO 设备读取宿主当前时间，并设置闹钟触发中断。实现参考
 * Linux goldfish RTC 驱动寄存器布局，只覆盖系统模拟所需的寄存器语义。
 */

#if !RV32_HAS(GOLDFISH_RTC)
#error \
    "只有启用 GOLDFISH RTC 支持时才能构建此文件。"
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rtc.h"

static uint64_t now_nsec;

uint64_t rtc_get_now_nsec(rtc_t *rtc)
{
    /* TODO:
     * - 检测时区，并使用正确的 UTC 偏移。
     * - 可在 main.c 新增 CLI 选项，让用户选择 [UTC] 或
     *   [UTC + offset]（本地时间），例如 -x rtc:utc 或 -x rtc:localtime。
     */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t) (ts.tv_sec * 1e9) + ts.tv_nsec + rtc->clock_offset;
}

uint32_t rtc_read(rtc_t *rtc, uint32_t addr)
{
    uint32_t rtc_read_val = 0;

    /*
     * 读取时间时，内核必须先执行 IO_READ(TIME_LOW)，得到低 32 位无符号值；
     * 再执行 IO_READ(TIME_HIGH)，得到完整 64 位时间的高 32 位有符号值。[1]
     *
     * [1]
     * https://android.googlesource.com/platform/external/qemu/+/refs/heads/emu-2.0-release/docs/GOLDFISH-VIRTUAL-HARDWARE.TXT
     */
    switch (addr) {
    case RTC_TIME_LOW:
        now_nsec = rtc_get_now_nsec(rtc);
        rtc->time_low = (uint32_t) (now_nsec & MASK(32));
        rtc_read_val = rtc->time_low;
        break;
    case RTC_TIME_HIGH:
        /* 复用读取 RTC_TIME_LOW 时捕获的 now_nsec，保证高低位一致。 */
        rtc->time_high = (uint32_t) (now_nsec >> 32);
        rtc_read_val = rtc->time_high;
        break;
    case RTC_ALARM_LOW:
        rtc_read_val = rtc->alarm_low;
        break;
    case RTC_ALARM_HIGH:
        rtc_read_val = rtc->alarm_high;
        break;
    case RTC_ALARM_STATUS:
        rtc_read_val = rtc->alarm_status;
        break;
    default:
        rv_log_error("不支持的 RTC 读取操作：0x%x", addr);
        break;
    }

    return rtc_read_val;
}

void rtc_write(rtc_t *rtc, uint32_t addr, uint32_t value)
{
    switch (addr) {
    case RTC_TIME_LOW:
        now_nsec = rtc_get_now_nsec(rtc);
        rtc->clock_offset += (uint64_t) (value) - (now_nsec & MASK(32));
        break;
    case RTC_TIME_HIGH:
        /* 复用写入 RTC_TIME_LOW 时捕获的 now_nsec，保证高低位一致。 */
        rtc->clock_offset += ((uint64_t) (value) << 32) -
                             (now_nsec & ((uint64_t) (MASK(32)) << 32));
        break;
    case RTC_ALARM_LOW:
        rtc->alarm_low = value;
        break;
    case RTC_ALARM_HIGH:
        rtc->alarm_high = value;
        break;
    case RTC_IRQ_ENABLED:
        rtc->irq_enabled = value;
        break;
    case RTC_CLEAR_ALARM:
        rtc->alarm_status = 0;
        break;
    case RTC_CLEAR_INTERRUPT:
        rtc->interrupt_status = 0;
        break;
    default:
        rv_log_error("不支持的 RTC 写入操作：0x%x", addr);
        break;
    }
    return;
}

rtc_t *rtc_new()
{
    rtc_t *rtc = calloc(1, sizeof(rtc_t));
    assert(rtc);

    /*
     * rtc->time_low/high 可通过 RTC_SET_TIME ioctl 更新，因此初始化时先让它们
     * 与宿主 OS 时间保持一致。
     */
    now_nsec = rtc_get_now_nsec(rtc);
    rtc->time_low = (uint32_t) (now_nsec & MASK(32));
    rtc->time_high = (uint32_t) (now_nsec >> 32);

    return rtc;
}

void rtc_delete(rtc_t *rtc)
{
    free(rtc);
}

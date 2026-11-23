/*
 * Y2K38 runtime compatibility shim for legacy 32-bit time_t prebuilts
 * (e.g. libnscube.so, libbabile.so) running under glibc >= 2.34 + time64 kernel.
 * Preloaded via LD_PRELOAD, intercepts 32-bit clock_gettime/gettimeofday/time
 * and sem_timedwait via raw __NR_clock_gettime64 (403) and glibc __sem_timedwait64.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_clock_gettime64
#define __NR_clock_gettime64 403
#endif

#ifndef CLOCK_REALTIME_ALARM
#define CLOCK_REALTIME_ALARM 8
#endif

#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE 5
#endif

/* Anchor post-2038 wall-clock deltas onto 2024-01-01T00:00:00Z inside int32 range. */
#define SHIM_EPOCH_BASE 1704067200LL
#define SHIM_TIME32_MAX 2147483647LL

struct kernel_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* Matches glibc armhf struct __timespec64 (sizeof=16, offsetof(tv_nsec)=8). */
struct glibc_timespec64 {
    int64_t tv_sec;
    int32_t tv_nsec;
    int32_t pad;
};

extern int __sem_timedwait64(sem_t *sem, const struct glibc_timespec64 *abstime);

static int64_t g_shim_base_real_sec = 0;

static int shim_ptr_null(const void *ptr)
{
    volatile uintptr_t addr = (uintptr_t)ptr;
    return addr == 0;
}

static int is_realtime_clock(clockid_t clk_id)
{
    return clk_id == CLOCK_REALTIME ||
           clk_id == CLOCK_REALTIME_COARSE ||
           clk_id == CLOCK_REALTIME_ALARM;
}

static int32_t shim_map_realtime_sec(int64_t real_sec)
{
    int64_t base;
    int64_t expected;
    int64_t synth;

    if (real_sec <= SHIM_TIME32_MAX) {
        return (int32_t)real_sec;
    }

    base = __atomic_load_n(&g_shim_base_real_sec, __ATOMIC_ACQUIRE);
    if (base == 0) {
        expected = 0;
        (void)__atomic_compare_exchange_n(&g_shim_base_real_sec, &expected, real_sec,
                                          0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        base = __atomic_load_n(&g_shim_base_real_sec, __ATOMIC_ACQUIRE);
    }

    synth = SHIM_EPOCH_BASE + (real_sec - base);
    if (synth < 0) {
        return 0;
    }
    if (synth > SHIM_TIME32_MAX) {
        return (int32_t)SHIM_TIME32_MAX;
    }
    return (int32_t)synth;
}

static int shim_read_clock64(clockid_t clk_id, struct kernel_timespec64 *kts)
{
    if (kts == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (syscall(__NR_clock_gettime64, clk_id, kts) != 0) {
        return -1;
    }
    return 0;
}

int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    struct kernel_timespec64 kts;

    if (shim_ptr_null(tp)) {
        errno = EFAULT;
        return -1;
    }
    if (shim_read_clock64(clk_id, &kts) != 0) {
        return -1;
    }

    if (is_realtime_clock(clk_id)) {
        tp->tv_sec = (time_t)shim_map_realtime_sec(kts.tv_sec);
    } else {
        tp->tv_sec = (time_t)(int32_t)kts.tv_sec;
    }
    tp->tv_nsec = (long)kts.tv_nsec;
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    struct kernel_timespec64 kts;
    (void)tz;

    if (shim_ptr_null(tv)) {
        errno = EFAULT;
        return -1;
    }
    if (shim_read_clock64(CLOCK_REALTIME, &kts) != 0) {
        return -1;
    }

    tv->tv_sec = (time_t)shim_map_realtime_sec(kts.tv_sec);
    tv->tv_usec = (suseconds_t)(kts.tv_nsec / 1000LL);
    return 0;
}

time_t time(time_t *tloc)
{
    struct kernel_timespec64 kts;
    time_t sec32;

    if (shim_read_clock64(CLOCK_REALTIME, &kts) != 0) {
        return (time_t)-1;
    }
    sec32 = (time_t)shim_map_realtime_sec(kts.tv_sec);
    if (tloc != NULL) {
        *tloc = sec32;
    }
    return sec32;
}

int sem_timedwait(sem_t *sem, const struct timespec *abstime)
{
    struct kernel_timespec64 now64;
    struct glibc_timespec64 abs64;
    int64_t base;

    if (shim_ptr_null(sem) || shim_ptr_null(abstime) ||
        abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }

    base = __atomic_load_n(&g_shim_base_real_sec, __ATOMIC_ACQUIRE);
    if (base == 0 && shim_read_clock64(CLOCK_REALTIME, &now64) == 0 &&
        now64.tv_sec > SHIM_TIME32_MAX) {
        (void)shim_map_realtime_sec(now64.tv_sec);
        base = __atomic_load_n(&g_shim_base_real_sec, __ATOMIC_ACQUIRE);
    }

    if (base > 0 && abstime->tv_sec >= (SHIM_EPOCH_BASE / 2)) {
        abs64.tv_sec = base + ((int64_t)abstime->tv_sec - SHIM_EPOCH_BASE);
    } else {
        abs64.tv_sec = (int64_t)abstime->tv_sec;
    }
    abs64.tv_nsec = (int32_t)abstime->tv_nsec;
    abs64.pad = 0;

    return __sem_timedwait64(sem, &abs64);
}

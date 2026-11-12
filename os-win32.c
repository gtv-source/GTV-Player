/*
 * os-win32.c
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2010 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <windows.h>
#include <mmsystem.h>
#include "sysemu/runstate.h"

static BOOL WINAPI qemu_ctrl_handler(DWORD type)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
    /* Windows 7 kills application when the function returns.
       Sleep here to give QEMU a try for closing.
       Sleep period is 10000ms because Windows kills the program
       after 10 seconds anyway. */
    Sleep(10000);

    return TRUE;
}

static TIMECAPS mm_tc;
static void os_tune_process(void);

static void os_undo_timer_resolution(void)
{
    timeEndPeriod(mm_tc.wPeriodMin);
}

void os_setup_early_signal_handling(void)
{
    SetConsoleCtrlHandler(qemu_ctrl_handler, TRUE);
    timeGetDevCaps(&mm_tc, sizeof(mm_tc));
    timeBeginPeriod(mm_tc.wPeriodMin);
    atexit(os_undo_timer_resolution);
    os_tune_process();
}

/*
 * TCG is CPU-bound and runs one host thread per vCPU.  On Windows 10/11 the
 * scheduler classifies background-looking processes as "EcoQoS" and parks
 * them on efficiency cores / throttles clocks, which makes a TCG guest
 * several times slower.  Opt out of power throttling and raise priority.
 */
typedef struct {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} QEMUPowerThrottlingState;

#define QEMU_POWER_THROTTLING_EXECUTION_SPEED 0x1
/* process only: honour timeBeginPeriod() even when Windows 11 thinks the
 * process is in the background (window occluded, not focused) */
#define QEMU_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#define QEMU_PROCESS_POWER_THROTTLING 4
#define QEMU_THREAD_POWER_THROTTLING  5

static bool os_disable_throttling(bool process)
{
    typedef BOOL (WINAPI *SetInfoFn)(HANDLE, int, LPVOID, DWORD);
    QEMUPowerThrottlingState st = {
        .Version = 1,
        .ControlMask = QEMU_POWER_THROTTLING_EXECUTION_SPEED |
                       (process ? QEMU_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION : 0),
        .StateMask = 0, /* 0 = never throttle */
    };
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    SetInfoFn fn;

    if (!k32) {
        return false;
    }
    if (process) {
        fn = (SetInfoFn)(void *)GetProcAddress(k32, "SetProcessInformation");
        return fn && fn(GetCurrentProcess(), QEMU_PROCESS_POWER_THROTTLING,
                        &st, sizeof(st));
    }
    fn = (SetInfoFn)(void *)GetProcAddress(k32, "SetThreadInformation");
    return fn && fn(GetCurrentThread(), QEMU_THREAD_POWER_THROTTLING,
                    &st, sizeof(st));
}

/*
 * Every accelerator: Windows' EcoQoS ("Efficiency mode") also hits WHPX/AEHD
 * guests on hybrid CPUs (12th gen+): vCPU threads land on E-cores at low
 * clocks and timers are coalesced, so the guest stalls ~1 s at a time while
 * the host looks idle (G-Player test machine i7-12700F, 10/11/2026).  Opting
 * out here does not depend on the player's power settings.
 */
bool os_win32_throttling_off;      /* logged by ui/gtv-perf.c */

static void os_tune_process(void)
{
    os_win32_throttling_off = os_disable_throttling(true);
}

void os_tune_tcg_process(void)
{
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    os_disable_throttling(true);
}

void os_tune_vcpu_thread(void)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    os_disable_throttling(false);
}

void os_set_line_buffering(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
}

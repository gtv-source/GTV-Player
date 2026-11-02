#include "adb.h"
#include "launcher_gl.h"
#include "launcher_2d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <SDL2/SDL.h>
#include <math.h>

static char *ADB_PATH = NULL;
static HANDLE g_job = NULL;
static char g_serial[64] = {0};
static volatile int adb_running = 1;
static DWORD g_t0;

#define MAX_BUF 2048
#define ADB_TIMEOUT_SHORT 3000

/* seconds since adb_start(), for the boot timeline in the log */
static double adb_elapsed(void)
{
    return (GetTickCount() - g_t0) / 1000.0;
}

// =======================
// EXEC
// =======================
static int adb_exec_hidden(const char *cmd, char *output, int size,
                           DWORD timeout_ms)
{
    if (!adb_running)
        return 0;

    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE hRead, hWrite;

    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return 0;

    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFO si = {0};
    PROCESS_INFORMATION pi = {0};

    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.wShowWindow = SW_HIDE;

    char cmdLine[1024];
    sprintf(cmdLine, "%s", cmd);

    if (!CreateProcess(NULL, cmdLine, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return 0;
    }

    if (g_job)
        AssignProcessToJobObject(g_job, pi.hProcess);

    CloseHandle(hWrite);

    DWORD total = 0;
    DWORD start = GetTickCount();

    while (1)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(hRead, NULL, 0, NULL, &avail, NULL))
            break;

        if (avail > 0)
        {
            DWORD readBytes = 0;
            ReadFile(hRead, output + total, size - total - 1, &readBytes, NULL);
            total += readBytes;

            if (total >= size - 1)
                break;
        }

        if (GetTickCount() - start > timeout_ms)
        {
            printf("[ADB] timeout (%lu ms) -> kill process\n", timeout_ms);
            TerminateProcess(pi.hProcess, 0);
            break;
        }

        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
            break;

        Sleep(10);
    }

    output[total] = '\0';

    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return 1;
}

static void adb_exec_t(const char *cmd, char *out, size_t out_size,
                       DWORD timeout_ms)
{
    memset(out, 0, out_size);
    adb_exec_hidden(cmd, out, out_size, timeout_ms);
}

static void adb_exec(const char *cmd, char *out, size_t out_size)
{
    adb_exec_t(cmd, out, out_size, ADB_TIMEOUT_SHORT);
}

// =======================
// BASIC COMMANDS
// =======================
static void adb_connect(int port)
{
    char cmd[256], buf[MAX_BUF];

    snprintf(g_serial, sizeof(g_serial), "127.0.0.1:%d", port);

    sprintf(cmd,
            "\"%s\" connect %s",
            ADB_PATH, g_serial);

    /*
     * slirp accepts the TCP connection before adbd is listening, so
     * "adb connect" can hang until the guest is up: keep it short and retry.
     */
    adb_exec(cmd, buf, sizeof(buf));
}

// =======================
// CHECK STATE
// =======================
static int adb_is_ready(void)
{
    char cmd[256];
    char buf[MAX_BUF];

    sprintf(cmd,
            "\"%s\" -s %s get-state",
            ADB_PATH,
            g_serial);

    adb_exec(cmd, buf, sizeof(buf));

    /*
     * Exact state only: "error: device '127.0.0.1:N' not found" also
     * contains the word "device".
     */
    return strncmp(buf, "device", 6) == 0;
}

static int adb_boot_completed(void)
{
    char cmd[256], buf[MAX_BUF];

    sprintf(cmd,
            "\"%s\" -s %s shell getprop sys.boot_completed",
            ADB_PATH, g_serial);

    adb_exec(cmd, buf, sizeof(buf));

    return buf[0] == '1';
}

/* (re)connect until adbd answers; 0 on timeout */
static int adb_wait_online(int port, DWORD timeout_ms)
{
    DWORD start = GetTickCount();

    while (adb_running && GetTickCount() - start < timeout_ms)
    {
        adb_connect(port);
        if (adb_is_ready())
        {
            printf("[ADB] +%.1fs device online\n", adb_elapsed());
            launcher_gl_notify_progress(0.66f);
            return 1;
        }
        Sleep(500);
    }
    return 0;
}

/* poll sys.boot_completed; 0 on timeout */
static int adb_wait_boot_completed(DWORD timeout_ms)
{
    DWORD start = GetTickCount();

    while (adb_running && GetTickCount() - start < timeout_ms)
    {
        if (adb_boot_completed())
        {
            printf("[ADB] +%.1fs boot completed\n", adb_elapsed());
            launcher_gl_notify_progress(0.74f);
            return 1;
        }
        Sleep(500);
    }
    return 0;
}

/* wait until the focused window belongs to pkg; 0 on timeout */
static int adb_wait_focus(const char *pkg, DWORD timeout_ms)
{
    char cmd[512], buf[MAX_BUF];
    DWORD start = GetTickCount();

    sprintf(cmd,
            "\"%s\" -s %s shell \"dumpsys window | grep mCurrentFocus\"",
            ADB_PATH, g_serial);

    while (adb_running && GetTickCount() - start < timeout_ms)
    {
        adb_exec(cmd, buf, sizeof(buf));
        if (strstr(buf, pkg))
        {
            return 1;
        }
        Sleep(200);
    }
    return 0;
}

// static int adb_android_ready(void)
// {
//     char cmd[256], buf[MAX_BUF];

//     sprintf(cmd,
//             "\"%s\" -s %s shell getprop init.svc.bootanim",
//             ADB_PATH, g_serial);

//     adb_exec(cmd, buf, sizeof(buf));

//     return strstr(buf, "stopped") != NULL;
// }

// =======================
// CHANGE DEVICEID
// =======================
static void adb_run_changeid(void)
{
    char cmd[512], buf[MAX_BUF];

    snprintf(cmd, sizeof(cmd),
             "\"%s\" -s %s shell sh /data/local/tmp/01changeid.sh",
             ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));

    printf("[ADB] changeid script executed\n");
}

// =======================
// POST BOOT
// =======================
static void adb_clear_ram(void)
{
    char cmd[512], buf[MAX_BUF];

    sprintf(cmd,
            "\"%s\" -s %s shell \"am kill-all\"",
            ADB_PATH, g_serial);

    adb_exec(cmd, buf, sizeof(buf));

    printf("[ADB] RAM cleared\n");
}

/*
 * Only the game should be visible.  SystemUI itself must stay enabled: with it
 * disabled there is no wallpaper/keyguard window and WindowManager holds the
 * screen until its 30 s BOOT TIMEOUT on every boot.  The navigation bar is
 * removed by qemu.hw.mainkeys=1 (/data/local.prop in the base image); the status
 * bar flags below are not persisted by Android, so they are set on every boot.
 */
/*
 * Clones made on base v1 still have SystemUI disabled, so they pay the 30 s BOOT
 * TIMEOUT on every boot.  Switch them to the base v2 settings once; they take
 * effect from the next boot (this session may still show the navigation bar).
 */
static void adb_repair_boot_settings(void)
{
    char cmd[768], buf[MAX_BUF];

    snprintf(cmd, sizeof(cmd),
             "\"%s\" -s %s shell \"if pm list packages -d | grep -q "
             "'package:com.android.systemui$'; then "
             "pm enable com.android.systemui >/dev/null; "
             "locksettings set-disabled true >/dev/null; "
             "su -c 'echo qemu.hw.mainkeys=1 > /data/local.prop; "
             "chmod 644 /data/local.prop'; echo repaired; fi\"",
             ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));

    if (strstr(buf, "repaired"))
    {
        printf("[ADB] +%.1fs old clone: boot settings repaired (fast from next boot)\n",
               adb_elapsed());
    }
}

static void adb_hide_system_ui(void)
{
    char cmd[512], buf[MAX_BUF];

    snprintf(cmd, sizeof(cmd),
             "\"%s\" -s %s shell \"cmd statusbar send-disable-flag search home "
             "recents notification-peek statusbar-expansion system-icons clock "
             "notification-icons\"",
             ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));

    printf("[ADB] +%.1fs system UI hidden\n", adb_elapsed());
    launcher_gl_notify_progress(0.80f);
}

static void adb_reboot(void)
{
    char cmd[512], buf[MAX_BUF];

    sprintf(cmd,
            "\"%s\" -s %s shell \"reboot",
            ADB_PATH, g_serial);

    adb_exec(cmd, buf, sizeof(buf));

    printf("[ADB] REBOOT\n");
}

static int adb_start_app(const char *pkg, const char *act)
{
    char cmd[512];
    char buf[MAX_BUF];

    sprintf(cmd,
            "\"%s\" -s %s shell am start -W -n %s/%s",
            ADB_PATH, g_serial, pkg, act);

    adb_exec_t(cmd, buf, sizeof(buf), 30000);

    // ===== FAIL =====
    if (strstr(buf, "Error") ||
        strstr(buf, "Exception") ||
        strstr(buf, "does not exist") ||
        strstr(buf, "Permission Denial"))
    {
        printf("[ADB] App launch failed\n");
        printf("%s\n", buf);
        return 0;
    }

    // ===== SUCCESS =====
    if (strstr(buf, "Status: ok") ||
        strstr(buf, "Complete"))
    {
        if (pkg == getenv("PKG_NAME"))
        {
            launcher_gl_notify_boot_complete();
        }

        printf("[ADB] +%.1fs %s launched\n", adb_elapsed(), pkg);
        return 1;
    }

    // UNKNOWN
    printf("[ADB] Unknown launch result\n");
    printf("%s\n", buf);

    return 0;
}

static void adb_boost_oom(const char *pkg)
{
    char cmd[512], buf[MAX_BUF];

    sprintf(cmd,
            "\"%s\" -s %s shell \"pid=$(pidof %s); "
            "if [ -n \\\"$pid\\\" ]; then "
            "su -c 'echo -1000 > /proc/'$pid'/oom_score_adj'; fi\"",
            ADB_PATH, g_serial, pkg);

    adb_exec(cmd, buf, sizeof(buf));

    printf("[ADB] OOM boosted\n");
}

static void adb_start_agent(const char *pkg, const char *act)
{
    char cmd[512], buf[MAX_BUF];

    printf("[ADB] Starting agent...\n");

    // 1. Kill agent cũ (fallback pkill -> pidof)
    sprintf(cmd,
            "\"%s\" -s %s shell \"pid=$(pidof agent.sh); "
            "if [ -n \\\"$pid\\\" ]; then kill -9 $pid; fi\"",
            ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));

    // 2. Set config
    sprintf(cmd,
            "\"%s\" -s %s shell \"setprop agent.act '%s'; setprop agent.pkg '%s'\"",
            ADB_PATH, g_serial, act, pkg);
    adb_exec(cmd, buf, sizeof(buf));

    Sleep(500);

    // 3. Start agent (không dùng setsid)
    sprintf(cmd,
            "\"%s\" -s %s shell \"sh /data/local/tmp/agent.sh >/dev/null 2>&1 &\"",
            ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));

    Sleep(1000);

    // 4. Verify agent chạy chưa
    sprintf(cmd,
            "\"%s\" -s %s shell pidof agent.sh",
            ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));

    if (strlen(buf) > 0)
        printf("[ADB] Agent running (pid=%s)\n", buf);
    else
        printf("[ADB] Agent FAILED to start\n");
}

static void adb_input_tap(int x, int y)
{
    char cmd[512];
    char buf[MAX_BUF];

    sprintf(cmd,
            "\"%s\" -s %s shell input tap %d %d",
            ADB_PATH,
            g_serial,
            x,
            y);

    adb_exec(cmd, buf, sizeof(buf));

    printf("[ADB] tap %d %d\n", x, y);
}

// =======================
// THREAD
// =======================
static DWORD WINAPI adb_thread(LPVOID arg)
{
    int port = *(int *)arg;
    free(arg);

    const char *pkg = getenv("PKG_NAME");
    const char *act = getenv("MAIN_ACT");

    if (!pkg || !act)
    {
        printf("[ADB] Missing ENV\n");
        return 0;
    }

    /*
     * Wait on real conditions instead of fixed sleeps: adbd online, then
     * sys.boot_completed.  Only reboot when Android really failed to boot,
     * not merely because a slow machine is still booting.
     */
    int retry = 0;
    const int MAX_RETRY = 3;

    while (adb_running)
    {
        if (!adb_wait_online(port, 120000))
        {
            printf("[ADB] +%.1fs device never came online\n", adb_elapsed());
            return 0;
        }
        if (adb_wait_boot_completed(120000))
        {
            break;
        }
        if (retry >= MAX_RETRY)
        {
            printf("[ADB] Max retry reached\n");
            return 0;
        }
        retry++;
        printf("[ADB] +%.1fs boot timeout -> rebooting (%d/%d)\n",
               adb_elapsed(), retry, MAX_RETRY);
        adb_reboot();
        Sleep(3000);
    }

    adb_repair_boot_settings();
    adb_hide_system_ui();
    adb_run_changeid();
    adb_clear_ram();

    const char *xres_env = getenv("XRES");
    const char *yres_env = getenv("YRES");

    int xres = xres_env ? atoi(xres_env) : 0;
    int yres = yres_env ? atoi(yres_env) : 0;

    /*
     * xtmapper (keyboard/mouse -> touch mapping) is only for landscape
     * games; portrait games are played with the mouse alone, so skip it and
     * its start-button tap.
     */
    if (xres > yres)
    {
        for (int i = 0; adb_running && i < 3; i++)
        {
            if (adb_start_app("xtr.keymapper", "xtr.keymapper.activity.MainActivity"))
            {
                break;
            }
            Sleep(1000);
        }

        /* the tap hits keymapper's button, so it must be on screen first */
        adb_wait_focus("xtr.keymapper", 5000);
        adb_input_tap(600, 621);
        Sleep(300);
    }
    else
    {
        printf("[ADB] +%.1fs portrait game: xtmapper not started\n", adb_elapsed());
    }
    launcher_gl_notify_progress(0.88f);

    int launched = 0;
    for (int i = 0; adb_running && i < 3 && !launched; i++)
    {
        launched = adb_start_app(pkg, act);
        if (!launched)
        {
            Sleep(1000);
        }
    }
    if (!launched)
    {
        printf("[ADB] +%.1fs game launch failed\n", adb_elapsed());
        return 0;
    }

    printf("[ADB] Android ready\n");
    adb_boost_oom(pkg);
    Sleep(3000);
    adb_start_agent(pkg, act);

    adb_stop();

    printf("[ADB] DONE\n");

    return 0;
}

// =======================
// CONTROL
// =======================
static void init_job(void)
{
    g_job = CreateJobObject(NULL, NULL);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {0};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    SetInformationJobObject(g_job,
                            JobObjectExtendedLimitInformation,
                            &info,
                            sizeof(info));
}

void adb_stop(void)
{
    char cmd[512];
    char buf[1024];

    // STOP ADBD
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -s %s shell stop adbd",
             ADB_PATH, g_serial);

    memset(buf, 0, sizeof(buf));
    adb_exec_hidden(cmd, buf, sizeof(buf), ADB_TIMEOUT_SHORT);
    printf("[ADB] stop adbd...\n");

    // DISCONNECT
    snprintf(cmd, sizeof(cmd),
             "\"%s\" disconnect %s",
             ADB_PATH, g_serial);

    memset(buf, 0, sizeof(buf));
    adb_exec_hidden(cmd, buf, sizeof(buf), ADB_TIMEOUT_SHORT);
    printf("[ADB] disconnect...\n");

    // KILL ADB SERVER (PC side)
    snprintf(cmd, sizeof(cmd),
             "\"%s\" kill-server",
             ADB_PATH);

    memset(buf, 0, sizeof(buf));
    adb_exec_hidden(cmd, buf, sizeof(buf), ADB_TIMEOUT_SHORT);
    printf("[ADB] kill-server\n");

    // FORCE KILL (backup)
    system("taskkill /F /IM adb.exe >nul 2>&1");

    adb_running = 0;
}

void adb_start(void)
{
    static LONG started;

    /* launcher_gl_init() runs more than once; a second thread would race */
    if (InterlockedExchange(&started, 1))
    {
        return;
    }

    adb_running = 1;
    g_t0 = GetTickCount();

    init_job();

    int adb_port = 5555;

    const char *port_env = getenv("ADB_PORT");
    const char *env = getenv("ADB_PATH");

    if (env)
    {
        if (ADB_PATH)
            free(ADB_PATH);
        ADB_PATH = strdup(env);
    }

    if (port_env)
        adb_port = atoi(port_env);

    int *arg = malloc(sizeof(int));
    *arg = adb_port;

    HANDLE hThread = CreateThread(NULL, 0, adb_thread, arg, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
}
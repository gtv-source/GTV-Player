#include "adb.h"
#include "launcher_gl.h"
#include "launcher_2d.h"
#include "gtv-clipboard.h"
#include "gtv-clip-dex.h"
#include "gtv-adbguard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <SDL2/SDL.h>
#include <math.h>

static char *ADB_PATH = NULL;
static HANDLE g_job = NULL;
static char g_serial[64] = {0};
static volatile int adb_running = 1;
static DWORD g_t0;

/*
 * ADB security (Smooth.md "Bảo mật ADB"): the hostfwd port to the guest's
 * adbd and the port of this VM's own adb server (ANDROID_ADB_SERVER_PORT for
 * every adb.exe started here), both random.  gtv-adbguard.c drops any other
 * client while they are open; adb_stop() closes both.
 */
static int g_fwd_port;
static int g_server_port;

/* net/slirp.c: close the host side of a tcp hostfwd on 127.0.0.1 */
void gtv_slirp_remove_hostfwd(int port);

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

    /* suspended: the guard must know the PID before adb.exe can connect */
    if (!CreateProcess(NULL, cmdLine, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi))
    {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return 0;
    }

    if (g_job)
        AssignProcessToJobObject(g_job, pi.hProcess);
    gtv_adbguard_allow_pid(pi.dwProcessId);
    ResumeThread(pi.hThread);

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

    /* still running after a timeout kill: forget it once it is gone */
    WaitForSingleObject(pi.hProcess, 1000);
    gtv_adbguard_forget_pid(pi.dwProcessId);

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
/*
 * Same as /data/local/tmp/01changeid.sh in the base image (Android ID from
 * the MAC address the launcher gives this clone), without its "sleep 2" for
 * the network: this runs after sys.boot_completed, when eth0 has long been
 * up, so the sleep only delayed the game by 2 s.
 */
static void adb_run_changeid(void)
{
    char cmd[1024], buf[MAX_BUF];

    snprintf(cmd, sizeof(cmd),
             "\"%s\" -s %s shell \""
             "M=$(cat /sys/class/net/eth0/address 2>/dev/null | tr -d ':'); "
             "[ -z \\\"$M\\\" ] && M=$(cat /proc/sys/kernel/random/uuid | tr -d '-'); "
             "N=$(echo -n \\\"$M\\\" | md5sum | cut -c 1-16); "
             "if [ \\\"$(settings get secure android_id)\\\" != \\\"$N\\\" ]; then "
             "settings put secure android_id $N; setprop ro.serialno $N; "
             "setprop persist.sys.android_id $N; "
             "log -t ChangeID \\\"Device ID updated to $N based on MAC $M\\\"; "
             "echo changed $N; else echo same $N; fi\"",
             ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));

    printf("[ADB] +%.1fs device id %s\n", adb_elapsed(),
           strstr(buf, "changed") ? "updated" : strstr(buf, "same") ? "unchanged" : "FAILED");
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
             "grep -q '^qemu.hw.mainkeys=' /data/local.prop 2>/dev/null || "
             "echo qemu.hw.mainkeys=1 >> /data/local.prop; "
             "chmod 644 /data/local.prop; echo repaired; fi\"",
             ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));

    if (strstr(buf, "repaired"))
    {
        printf("[ADB] +%.1fs old clone: boot settings repaired (fast from next boot)\n",
               adb_elapsed());
    }
}

/*
 * This Android build has no su, but adbd can restart as root ("adb root").
 * Root is needed to write /data/local.prop, set the game's oom_score_adj and
 * keep the game watcher alive after "stop adbd".  1 when adbd runs as root.
 */
static int adb_become_root(int port)
{
    char cmd[512], buf[MAX_BUF];

    snprintf(cmd, sizeof(cmd), "\"%s\" -s %s root", ADB_PATH, g_serial);
    adb_exec_t(cmd, buf, sizeof(buf), 10000);

    /* adbd restarts: connect again */
    if (!strstr(buf, "already running as root"))
    {
        Sleep(1000);
        if (!adb_wait_online(port, 20000))
        {
            return 0;
        }
    }

    snprintf(cmd, sizeof(cmd), "\"%s\" -s %s shell id -u", ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));
    int root = buf[0] == '0';
    printf("[ADB] +%.1fs adbd %s\n", adb_elapsed(), root ? "as root" : "NOT root");
    return root;
}

/*
 * Android raises "<app> isn't responding" when an app does not handle input
 * within 5 s (broadcasts 10 s, services 20 s).  Weak machines often take
 * longer while a game loads.  Android 12+ multiplies all those timeouts by
 * ro.hw_timeout_multiplier (what AOSP does for its own emulators); read only
 * at boot, from /data/local.prop, so a missing line takes effect next boot.
 */
static void adb_set_anr_timeout(void)
{
    char cmd[768], buf[MAX_BUF];

    snprintf(cmd, sizeof(cmd),
             "\"%s\" -s %s shell \"f=/data/local.prop; "
             "grep -q '^ro.hw_timeout_multiplier=' $f 2>/dev/null || "
             "{ echo ro.hw_timeout_multiplier=4 >> $f; chmod 644 $f; echo added; }; "
             "getprop ro.hw_timeout_multiplier\"",
             ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));

    if (strstr(buf, "added"))
    {
        printf("[ADB] +%.1fs anr timeout x4 (from next boot)\n", adb_elapsed());
    }
    else
    {
        printf("[ADB] +%.1fs anr timeout multiplier: %s\n", adb_elapsed(),
               buf[0] >= '0' && buf[0] <= '9' ? strtok(buf, "\r\n") : "1");
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

/*
 * With adb authentication on, an unknown key makes SystemUI show "Allow USB
 * debugging?" over the game: a player could let another tool in.  Disable
 * that activity (root; kept in /data, so only the first boot of a clone does
 * the work).  Runs in the background: pm takes ~1 s.
 */
static void adb_block_auth_prompt(void)
{
    char cmd[512], buf[MAX_BUF];

    snprintf(cmd, sizeof(cmd),
             "\"%s\" -s %s shell \"pm disable "
             "com.android.systemui/.usb.UsbDebuggingActivity >/dev/null 2>&1 &\"",
             ADB_PATH, g_serial);
    adb_exec(cmd, buf, sizeof(buf));
    printf("[ADB] +%.1fs adb confirmation dialog disabled\n", adb_elapsed());
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

/*
 * Login token for the game (GPlayer.exe --token T -> env GAME_TOKEN): passed
 * as the intent extra "token" (getIntent().getStringExtra("token")).  It goes
 * to /data/local/tmp/gtv-token (mode 600) and the guest shell reads it there,
 * so it is on no Windows command line, and the watcher can start the game
 * again with the same token.  It is never printed.  Only token characters
 * (JWT, base64, UUID) are accepted, since it ends up in a shell command.
 */
#define TOKEN_MAX 4096
static int g_has_token;

static int adb_push_data(const char *name, const void *data, size_t len);

static int is_game(const char *pkg)
{
    const char *game = getenv("PKG_NAME");
    return game && pkg && strcmp(pkg, game) == 0;
}

static void adb_push_token(void)
{
    char cmd[512], buf[MAX_BUF];
    const char *t = getenv("GAME_TOKEN");
    size_t len = t ? strlen(t) : 0;
    int ok = len > 0 && len <= TOKEN_MAX;

    for (size_t i = 0; ok && i < len; i++)
    {
        ok = isalnum((unsigned char)t[i]) || strchr("._~+/=:-", t[i]);
    }

    g_has_token = 0;
    if (ok && adb_push_data("gtv-token", t, len))
    {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -s %s shell \"chmod 600 /data/local/tmp/gtv-token && echo ok\"",
                 ADB_PATH, g_serial);
        adb_exec(cmd, buf, sizeof(buf));
        g_has_token = strstr(buf, "ok") != NULL;
    }
    if (!g_has_token)
    {
        /* a clone is used again: no token from an earlier start */
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -s %s shell rm -f /data/local/tmp/gtv-token",
                 ADB_PATH, g_serial);
        adb_exec(cmd, buf, sizeof(buf));
    }

    if (g_has_token)
    {
        printf("[ADB] +%.1fs token passed (%u chars)\n", adb_elapsed(), (unsigned)len);
    }
    else if (len)
    {
        printf("[ADB] token rejected (%s), game started without it\n",
               ok ? "could not copy it" : "bad characters or too long");
    }
}

static int adb_start_app(const char *pkg, const char *act)
{
    char cmd[512];
    char buf[MAX_BUF];

    if (g_has_token && is_game(pkg))
    {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -s %s shell \"am start -W -n %s/%s "
                 "--es token \\\"$(cat /data/local/tmp/gtv-token)\\\"\"",
                 ADB_PATH, g_serial, pkg, act);
    }
    else
    {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -s %s shell am start -W -n %s/%s",
                 ADB_PATH, g_serial, pkg, act);
    }

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
            "\"%s\" -s %s shell \"for pid in $(pidof %s); do "
            "echo -1000 > /proc/$pid/oom_score_adj; done\"",
            ADB_PATH, g_serial, pkg);

    adb_exec(cmd, buf, sizeof(buf));

    printf("[ADB] OOM boosted\n");
}

/*
 * Game watcher: the player must not be able to close the game from inside
 * Android (ESC/Back on the main screen, the game's own "exit" button, "Close
 * app" in an ANR dialog, a crash); only closing the G-Player window ends it.
 * ESC stays Back so it still closes the game's popups.
 *
 * A shell script runs in the guest and starts the game again when its process
 * is gone, or brings it back when the home screen gets the focus.  It moves
 * itself out of adbd's cgroup, which needs root: "stop adbd" kills every
 * process left in it (the old agent.sh died that way).
 */
static const char KEEPGAME_SH[] =
    "#!/system/bin/sh\n"
    "# gtv-keepgame: written and started by G-Player (QEMU ui/adb.c) on every boot\n"
    "PKG=$1\n"
    "ACT=$2\n"
    "DIR=/data/local/tmp\n"
    "LOG=$DIR/gtv-keepgame.log\n"
    "echo $$ > /sys/fs/cgroup/cgroup.procs 2>/dev/null\n"
    "echo $$ > $DIR/gtv-keepgame.pid\n"
    "HOME_ACT=$(cmd package resolve-activity --brief -c android.intent.category.HOME"
    " -a android.intent.action.MAIN | tail -n 1)\n"
    "HOME_PKG=${HOME_ACT%%/*}\n"
    "log() { echo \"$(date +%T) $*\" >> $LOG; }\n"
    "log \"watching $PKG/$ACT (home: $HOME_PKG)\"\n"
    "starts=\"\"\n"
    "away=0\n"
    "relaunch() {\n"
    "  now=$(date +%s); keep=\"\"; n=0\n"
    "  for t in $starts; do\n"
    "    if [ $((now - t)) -lt 60 ]; then keep=\"$keep $t\"; n=$((n + 1)); fi\n"
    "  done\n"
    "  starts=$keep\n"
    "  if [ $n -ge 5 ]; then\n"
    "    log \"back-off 30 s ($n relaunches within 60 s)\"\n"
    "    starts=\"\"; away=0; sleep 30; return\n"
    "  fi\n"
    "  starts=\"$starts $now\"\n"
    "  log \"relaunch ($1)\"\n"
    "  # the same login token as the first start (ui/adb.c adb_push_token)\n"
    "  if [ -s $DIR/gtv-token ]; then\n"
    "    am start -n \"$PKG/$ACT\" --es token \"$(cat $DIR/gtv-token)\" >/dev/null 2>&1\n"
    "  else\n"
    "    am start -n \"$PKG/$ACT\" >/dev/null 2>&1\n"
    "  fi\n"
    "  away=0\n"
    "  sleep 2\n"
    "}\n"
    "while :; do\n"
    "  sleep 1\n"
    "  if [ -z \"$(pidof $PKG)\" ]; then relaunch \"process gone\"; continue; fi\n"
    "  # the home screen has the focus: the game was left (Back, exit button)\n"
    "  # anything else (the game, its ANR dialog, a login/payment page, the\n"
    "  # keyboard) is left alone\n"
    "  case \"$(dumpsys window | grep -m 1 mCurrentFocus)\" in\n"
    "    *\" $HOME_PKG/\"*) away=$((away + 1)) ;;\n"
    "    *) away=0 ;;\n"
    "  esac\n"
    "  if [ $away -ge 2 ]; then relaunch \"left the game\"; fi\n"
    "done\n";

/* copy data built into the exe to /data/local/tmp/<name> in the guest */
static int adb_push_data(const char *name, const void *data, size_t len)
{
    char cmd[1024], buf[MAX_BUF], tmp[MAX_PATH], file[MAX_PATH];
    FILE *f;

    if (!GetTempPathA(sizeof(tmp), tmp) ||
        snprintf(file, sizeof(file), "%sgplayer-%lu-%s", tmp,
                 GetCurrentProcessId(), name) >= (int)sizeof(file) ||
        !(f = fopen(file, "wb")))
    {
        printf("[ADB] cannot write a temp file for %s\n", name);
        return 0;
    }
    int ok = fwrite(data, 1, len, f) == len;
    fclose(f);

    if (ok)
    {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -s %s push \"%s\" /data/local/tmp/%s",
                 ADB_PATH, g_serial, file, name);
        adb_exec_t(cmd, buf, sizeof(buf), 10000);
        ok = strstr(buf, "1 file pushed") != NULL;
    }
    DeleteFileA(file);
    if (!ok)
    {
        printf("[ADB] push %s failed: %s\n", name, buf);
    }
    return ok;
}

/*
 * Host -> Android clipboard: GtvClip (gplayer/android/GtvClip.java, built
 * into ui/gtv-clip-dex.h) runs with app_process, connects to QEMU's listener
 * (ui/gtv-clipboard.c) through 10.0.2.2 and sets every host clipboard text.
 * Like the watcher it leaves adbd's cgroup so "stop adbd" does not kill it.
 */
/* pkg: the game, for the guest load stats (STAT lines, ui/gtv-perf.c) */
static void adb_start_clipboard(const char *pkg)
{
    char cmd[1024], buf[MAX_BUF];
    int port = gtv_clip_start();

    if (!port)
    {
        printf("[ADB] clipboard: no host listener\n");
        return;
    }
    if (!adb_push_data("gtvclip.dex", GTV_CLIP_DEX, sizeof(GTV_CLIP_DEX)))
    {
        return;
    }

    /* no "pasted from your clipboard" toast on every paste */
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -s %s shell \"settings put secure clipboard_show_access_notifications 0; "
             "d=/data/local/tmp; kill $(cat $d/gtvclip.pid 2>/dev/null) 2>/dev/null; "
             "setsid sh -c 'echo $$ > /sys/fs/cgroup/cgroup.procs 2>/dev/null; "
             "echo $$ > /data/local/tmp/gtvclip.pid; "
             "CLASSPATH=/data/local/tmp/gtvclip.dex exec app_process / GtvClip %d %s %s' "
             "</dev/null >/dev/null 2>&1 & sleep 1; "
             "kill -0 $(cat $d/gtvclip.pid) && echo running\"",
             ADB_PATH, g_serial, port, gtv_clip_token(), pkg && *pkg ? pkg : "");
    adb_exec(cmd, buf, sizeof(buf));

    printf("[ADB] +%.1fs clipboard helper %s\n", adb_elapsed(),
           strstr(buf, "running") ? "running" : "FAILED to start");
}

static void adb_start_watcher(const char *pkg, const char *act, int root)
{
    char cmd[1024], buf[MAX_BUF];

    if (!adb_push_data("gtv-keepgame.sh", KEEPGAME_SH, sizeof(KEEPGAME_SH) - 1))
    {
        return;
    }

    /* one watcher only: stop the one a previous launch left running */
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -s %s shell \"d=/data/local/tmp; "
             "kill $(cat $d/gtv-keepgame.pid 2>/dev/null) 2>/dev/null; "
             "setsid sh $d/gtv-keepgame.sh %s %s </dev/null >/dev/null 2>&1 & "
             "sleep 1; kill -0 $(cat $d/gtv-keepgame.pid) && echo running\"",
             ADB_PATH, g_serial, pkg, act);
    adb_exec(cmd, buf, sizeof(buf));

    if (!strstr(buf, "running"))
    {
        printf("[ADB] +%.1fs watcher FAILED to start: %s\n", adb_elapsed(), buf);
    }
    else
    {
        printf("[ADB] +%.1fs watcher running%s\n", adb_elapsed(),
               root ? "" : " (adbd not root: it stops with adbd)");
    }
}

/* xtmapper's input server runs as xtr.keymapper:root:0 once Start is tapped */
static void adb_wait_xtmapper_server(DWORD timeout_ms)
{
    char cmd[256], buf[MAX_BUF];
    DWORD start = GetTickCount();

    snprintf(cmd, sizeof(cmd), "\"%s\" -s %s shell pidof xtr.keymapper:root:0",
             ADB_PATH, g_serial);

    while (adb_running && GetTickCount() - start < timeout_ms)
    {
        adb_exec(cmd, buf, sizeof(buf));
        if (buf[0] >= '0' && buf[0] <= '9')
        {
            printf("[ADB] +%.1fs xtmapper server up\n", adb_elapsed());
            return;
        }
        Sleep(200);
    }
    printf("[ADB] +%.1fs xtmapper server not seen\n", adb_elapsed());
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
static DWORD adb_setup(int port)
{
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
    int root = 0;

    while (adb_running)
    {
        if (!adb_wait_online(port, 120000))
        {
            printf("[ADB] +%.1fs device never came online\n", adb_elapsed());
            return 0;
        }
        if (!root)
        {
            root = adb_become_root(port);
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

    if (root)
    {
        adb_set_anr_timeout();
        adb_block_auth_prompt();
    }
    adb_repair_boot_settings();
    adb_push_token();
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
        /* let xtmapper start its server before the game competes for CPU */
        adb_wait_xtmapper_server(3000);
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
    adb_start_watcher(pkg, act, root);
    /*
     * After the game: it is not needed to start it (GtvClip sends the host
     * clipboard as soon as it connects, so an earlier copy still arrives).
     */
    adb_start_clipboard(pkg);

    return 0;
}

static DWORD WINAPI adb_thread(LPVOID arg)
{
    int port = *(int *)arg;
    free(arg);

    adb_setup(port);

    /*
     * Done, or given up (no boot, game not launched): either way adb is
     * closed, so nothing else on the host can reach the guest.
     * GPLAYER_KEEP_ADB=1 (tests): keep adbd to read logs while playing.
     */
    const char *keep = getenv("GPLAYER_KEEP_ADB");
    if (keep && keep[0] == '1')
    {
        gtv_adbguard_stop();
        printf("[ADB] GPLAYER_KEEP_ADB: adbd kept and NOT protected: "
               "adb -P %d -s 127.0.0.1:%d shell\n", g_server_port, g_fwd_port);
    }
    else
    {
        adb_stop();
    }

    printf("[ADB] DONE\n");
    fflush(stdout);

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
    static LONG stopped;
    char cmd[512];
    char buf[1024];

    /* from the adb thread when done, and again from SDL_QUIT */
    if (InterlockedExchange(&stopped, 1))
    {
        return;
    }

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

    // KILL OUR ADB SERVER (PC side): ANDROID_ADB_SERVER_PORT is ours, so the
    // user's adb and other G-Players' servers are left alone
    snprintf(cmd, sizeof(cmd),
             "\"%s\" kill-server",
             ADB_PATH);

    memset(buf, 0, sizeof(buf));
    adb_exec_hidden(cmd, buf, sizeof(buf), ADB_TIMEOUT_SHORT);
    printf("[ADB] kill-server\n");

    // FORCE KILL (backup): only the process still listening on our port
    Sleep(200);
    DWORD srv = gtv_adbguard_server_pid();
    if (srv)
    {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, srv);
        if (h)
        {
            TerminateProcess(h, 0);
            CloseHandle(h);
            printf("[ADB] adb server pid %lu terminated\n", srv);
        }
    }

    // nothing on the host leads to the guest's adbd any more
    if (g_fwd_port)
    {
        gtv_slirp_remove_hostfwd(g_fwd_port);
    }
    gtv_adbguard_stop();

    adb_running = 0;
}

/* a free random port on 127.0.0.1 in 20000-59999 (no launcher value) */
static int adb_random_port(void)
{
    for (int i = 0; i < 50; i++)
    {
        int port = 20000 + (int)((GetTickCount() * 2654435761u + GetCurrentProcessId() * 40503u
                                  + (unsigned)i * 7919u) % 40000u);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        int ok = s != INVALID_SOCKET && bind(s, (struct sockaddr *)&a, sizeof(a)) == 0;
        if (s != INVALID_SOCKET)
        {
            closesocket(s);
        }
        if (ok)
        {
            return port;
        }
    }
    return 0;
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

    /*
     * This VM's own adb server: every adb.exe started from here inherits the
     * port, so the shared server (5037) never sees the VM.  The launcher picks
     * it (ADB_SERVER_PORT); an older launcher does not, then pick one here.
     */
    const char *srv_env = getenv("ADB_SERVER_PORT");
    g_server_port = srv_env ? atoi(srv_env) : 0;
    if (g_server_port <= 0 || g_server_port > 65535)
        g_server_port = adb_random_port();
    if (g_server_port)
    {
        char p[16];
        snprintf(p, sizeof(p), "%d", g_server_port);
        SetEnvironmentVariableA("ANDROID_ADB_SERVER_PORT", p);
    }
    /* would override the port above */
    SetEnvironmentVariableA("ADB_SERVER_SOCKET", NULL);

    /*
     * Layer 2: the guest's adbd only accepts G-Player's key (launcher
     * core/adbAuth.js).  Our adb server loads it from ADB_VENDOR_KEYS; wide
     * calls, the path may hold a non-ASCII user name.
     */
    {
        wchar_t key[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"ADB_KEY_FILE", key, MAX_PATH * 2);
        if (n > 0 && n < MAX_PATH * 2)
        {
            SetEnvironmentVariableW(L"ADB_VENDOR_KEYS", key);
            printf("[ADB] adb authentication: G-Player key\n");
        }
        else
        {
            SetEnvironmentVariableW(L"ADB_VENDOR_KEYS", NULL);
            printf("[ADB] SECURITY: no G-Player adb key, guest adbd without authentication\n");
        }
    }
    g_fwd_port = adb_port;

    if (adb_port >= 5555 && adb_port <= 5585)
        printf("[ADB] SECURITY: adb port %d is in the range every adb server scans "
               "(old launcher?)\n", adb_port);
    gtv_adbguard_start(g_fwd_port, g_server_port);

    int *arg = malloc(sizeof(int));
    *arg = adb_port;

    HANDLE hThread = CreateThread(NULL, 0, adb_thread, arg, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
}
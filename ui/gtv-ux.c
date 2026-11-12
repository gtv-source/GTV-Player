/*
 * G-Player: user-activity log.
 *
 * What the player does, seen from outside the game (no change in the game):
 *  - screens: the window that has input focus in Android (GtvClip "FOCUS"),
 *    e.g. SplashActivity -> AppActivity (loading) -> Authen (login/register)
 *    -> AppActivity (in game); dialogs too;
 *  - the game SDKs' own event names (GtvClip "DH" / "VTC"): splash/loading
 *    steps, SDK init, login start, register tab ...;
 *  - "Application Not Responding" dialogs and game crashes;
 *  - clicks/keys per screen, AFK, and on which screen the window was closed.
 * Printed as "[UX] key=value ..." lines; the launcher writes them to the log
 * file and sends them to the event API. Never text the player types, field
 * contents or click positions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <windows.h>
#include "gtv-ux.h"

#define AFK_MS      120000      /* no input this long: AFK */
#define PATH_LEN    10          /* last screens in the summary */
#define NAME_LEN    96

static CRITICAL_SECTION lock;
static int inited;
static char game_pkg[128];

static char screen[NAME_LEN] = "-";
static LONGLONG screen_since;       /* epoch ms (guest clock) */
static int screen_taps, screen_keys;

static LONGLONG session_start;      /* host epoch ms */
static int n_screens, total_taps, total_keys, n_anr, n_crash;
static char path[PATH_LEN][NAME_LEN];
static int path_n;

static volatile LONGLONG last_input;   /* host epoch ms */
static volatile int afk;
static volatile int game_seen;      /* AFK counts from the game's first screen */
static volatile LONG ended;

static LONGLONG now_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (LONGLONG)(t / 10000ULL) - 11644473600000LL;
}

static void G_GNUC_PRINTF(1, 2) emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[UX] ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

/* "pkg/full.Activity" of the game -> "Activity"; other windows as they are */
static void short_name(const char *win, char *out, size_t len)
{
    size_t pl = strlen(game_pkg);
    const char *slash = strchr(win, '/');

    if (pl && slash && (size_t)(slash - win) == pl && !strncmp(win, game_pkg, pl)) {
        const char *cls = slash + 1;
        const char *dot = strrchr(cls, '.');
        snprintf(out, len, "%s", dot ? dot + 1 : cls);
    } else {
        snprintf(out, len, "%s", win);
    }
    /* key=value lines: no spaces in values */
    for (char *p = out; *p; p++) {
        if (*p == ' ' || *p == '=') {
            *p = '_';
        }
    }
}

static DWORD WINAPI afk_thread(LPVOID arg)
{
    while (!ended) {
        Sleep(5000);
        LONGLONG idle = now_ms() - last_input;
        if (game_seen && !afk && idle > AFK_MS) {
            afk = 1;
            EnterCriticalSection(&lock);
            emit("afk screen=%s idle_s=%lld", screen, idle / 1000);
            LeaveCriticalSection(&lock);
        }
    }
    return 0;
}

static void init(void)
{
    static LONG once;
    if (InterlockedExchange(&once, 1)) {
        while (!inited) {
            Sleep(1);
        }
        return;
    }
    InitializeCriticalSection(&lock);
    const char *p = getenv("PKG_NAME");
    snprintf(game_pkg, sizeof(game_pkg), "%s", p ? p : "");
    session_start = last_input = now_ms();
    HANDLE t = CreateThread(NULL, 0, afk_thread, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    }
    inited = 1;
}

/* QEMU ends some other way (guest powered off, killed by the launcher) */
static void at_exit(void)
{
    gtv_ux_end("emulator_exit");
}

void gtv_ux_start(void)
{
    init();
    atexit(at_exit);
}

static void enter_screen(const char *name, LONGLONG ms)
{
    if (!strcmp(name, screen)) {
        return;
    }
    LONGLONG on = screen_since && ms > screen_since ? (ms - screen_since) / 1000 : 0;
    emit("screen=%s prev=%s prev_s=%lld taps=%d keys=%d", name, screen, on,
         screen_taps, screen_keys);
    snprintf(screen, sizeof(screen), "%s", name);
    screen_since = ms;
    /* a game screen (short name, no "pkg/"): from now on the player can act */
    if (!game_seen && !strchr(name, '/') && !strchr(name, ':')) {
        game_seen = 1;
        last_input = now_ms();
    }
    screen_taps = screen_keys = 0;
    n_screens++;
    if (path_n < PATH_LEN) {
        snprintf(path[path_n++], NAME_LEN, "%s", name);
    } else {
        memmove(path[0], path[1], sizeof(path[0]) * (PATH_LEN - 1));
        snprintf(path[PATH_LEN - 1], NAME_LEN, "%s", name);
    }
}

void gtv_ux_line(const char *line)
{
    char kind[16], text[200], name[NAME_LEN];
    long long ms;

    init();
    if (sscanf(line, "%lld %15s %199[^\n]", &ms, kind, text) != 3) {
        return;
    }
    EnterCriticalSection(&lock);
    if (!strcmp(kind, "FOCUS")) {
        short_name(text, name, sizeof(name));
        enter_screen(name, ms);
    } else if (!strcmp(kind, "DH") || !strcmp(kind, "VTC")) {
        short_name(text, name, sizeof(name));
        emit("event=%s:%s screen=%s", kind, name, screen);
    } else if (!strcmp(kind, "ANR")) {
        n_anr++;
        emit("anr pkg=%s screen=%s", text, screen);
    } else if (!strcmp(kind, "CRASH")) {
        n_crash++;
        short_name(text, name, sizeof(name));
        emit("crash class=%s screen=%s", name, screen);
    } else if (!strcmp(kind, "KILL")) {
        /* the system killed the game (GtvClip: am_kill, reason class only) */
        short_name(text, name, sizeof(name));
        emit("killed reason=%s screen=%s", name, screen);
    }
    LeaveCriticalSection(&lock);
}

void gtv_ux_input(int key)
{
    init();
    LONGLONG t = now_ms();
    EnterCriticalSection(&lock);
    if (afk) {
        afk = 0;
        emit("active screen=%s after_s=%lld", screen, (t - last_input) / 1000);
    }
    last_input = t;
    if (key) {
        screen_keys++;
        total_keys++;
    } else {
        screen_taps++;
        total_taps++;
    }
    LeaveCriticalSection(&lock);
}

void gtv_ux_end(const char *reason)
{
    char trail[PATH_LEN * (NAME_LEN + 1)] = "";

    init();
    if (InterlockedExchange(&ended, 1)) {
        return;
    }
    EnterCriticalSection(&lock);
    for (int i = 0; i < path_n; i++) {
        if (i) {
            strncat(trail, ">", sizeof(trail) - strlen(trail) - 1);
        }
        strncat(trail, path[i], sizeof(trail) - strlen(trail) - 1);
    }
    LONGLONG t = now_ms();
    emit("session_end reason=%s screen=%s on_screen_s=%lld total_s=%lld screens=%d "
         "taps=%d keys=%d anr=%d crash=%d afk=%d path=%s",
         reason, screen, screen_since && t > screen_since ? (t - screen_since) / 1000 : 0,
         (t - session_start) / 1000, n_screens, total_taps, total_keys, n_anr, n_crash,
         afk, trail[0] ? trail : "-");
    LeaveCriticalSection(&lock);
}

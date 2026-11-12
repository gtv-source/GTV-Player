/*
 * G-Player: performance HUD data and [PERF] log lines, see gtv-perf.h.
 *
 * Sources:
 *   frames   gtv_perf_frame() per guest frame (ui/sdl2-gl.c scanout flush)
 *   GPU      GL renderer string, SDL_GL_SwapWindow time
 *   CPU/RAM  GetSystemTimes / GetProcessTimes / GlobalMemoryStatusEx, and
 *            the guest's own numbers sent by GtvClip ("STAT" lines)
 *   network  slirp byte counters; RTT and retransmissions of QEMU's own TCP
 *            connections (slirp opens the game's connections on the host)
 *            with TCP extended statistics (needs administrator, which
 *            GPlayer.exe has), else ICMP ping to the busiest peer.
 *            iphlpapi.dll is loaded at run time: QEMU does not link it.
 *
 * Plain Win32 like ui/gtv-clipboard.c (no qemu/osdep.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tcpestats.h>
#include <icmpapi.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "gtv-perf.h"

/* os-win32.c (sysemu/os-win32.h; this file does not use qemu/osdep.h) */
extern bool os_win32_throttling_off;

#define SAMPLE_MS 1000
#define LOG_EVERY 5             /* samples per [PERF] line */
#define STALL_MS 100            /* a frame gap that is felt */
#define STALL_LOG_MS 250        /* logged at once */
#define RING 2048               /* frame gaps kept (> 5 s at 240 fps) */
#define WINDOW_MS 5000          /* 1% low / stall count over this */

static CRITICAL_SECTION lock;
static volatile LONG inited;
static volatile LONG hud_on;
static LARGE_INTEGER freq;

/* frames, under lock */
static LONGLONG last_frame;     /* QPC */
static unsigned frames;         /* since the last sample */
static struct { LONGLONG t; float ms; } ring[RING];
static unsigned ring_pos;
static int fps_now;             /* last second, for the stall log */

/* swap time, under lock */
static unsigned long long swap_us_sum;
static unsigned swap_n, swap_us_max;

static char gl_renderer[96] = "?";
static int gl_software;

static volatile LONG64 net_rx, net_tx;

/* guest STAT, under lock */
static int g_cpu = -1, g_game = -1, g_avail = -1, g_swap = -1;
/* 1: the game runs ARM code through the ARM -> x86 translator */
static int g_arm = -1, g_arm_logged = -2;
static ULONGLONG g_time;

static char vm_info[64];

/*
 * Why a frame came late.  Slow events of the QEMU main loop, kept a few
 * seconds and looked up when a stall is logged:
 *   - a guest GPU command (virgl, hw/display/virtio-gpu-virgl.c) that took
 *     long: the host GL driver compiling shaders, uploading textures, or
 *     waiting for the GPU;
 *   - the main loop itself late (heartbeat timer from ui/sdl2.c): something
 *     else held it (the big QEMU lock, disk, display).
 * Neither: the guest did not produce the frame (game/Android side).
 */
#define SLOW_RING 512
#define SLOW_GPU_US 8000      /* GPU commands kept */
#define SLOW_LOOP_MS 50.0f    /* main loop delays kept */
static struct ev {
    LONGLONG end;               /* QPC */
    float ms;
    unsigned type;              /* GPU command, 0 for a main loop delay */
} evs[SLOW_RING];
static unsigned ev_pos;
static LONGLONG last_tick;
/* per sample, under lock */
static unsigned long long gpu_us_sum;
static unsigned gpu_us_max, gpu_max_type;
static float loop_max_ms;

static const char *gpu_cmd_name(unsigned t)
{
    switch (t) {
    case 0x0100: return "GET_DISPLAY_INFO";
    case 0x0101: return "RESOURCE_CREATE_2D";
    case 0x0102: return "RESOURCE_UNREF";
    case 0x0103: return "SET_SCANOUT";
    case 0x0104: return "RESOURCE_FLUSH";
    case 0x0105: return "TRANSFER_TO_HOST_2D";
    case 0x0106: return "ATTACH_BACKING";
    case 0x0107: return "DETACH_BACKING";
    case 0x010c: return "RESOURCE_CREATE_BLOB";
    case 0x010d: return "SET_SCANOUT_BLOB";
    case 0x0200: return "CTX_CREATE";
    case 0x0201: return "CTX_DESTROY";
    case 0x0202: return "CTX_ATTACH";
    case 0x0203: return "CTX_DETACH";
    case 0x0204: return "RESOURCE_CREATE_3D";
    case 0x0205: return "TRANSFER_TO_HOST_3D";
    case 0x0206: return "TRANSFER_FROM_HOST_3D";
    case 0x0207: return "SUBMIT_3D";
    case 0x0208: return "MAP_BLOB";
    case 0x0209: return "UNMAP_BLOB";
    default: return "?";
    }
}

static void ev_add(LONGLONG end, float ms, unsigned type)
{
    evs[ev_pos % SLOW_RING].end = end;
    evs[ev_pos % SLOW_RING].ms = ms;
    evs[ev_pos % SLOW_RING].type = type;
    ev_pos++;
}

/* HUD text, under lock */
static char hud[GTV_HUD_LINES][GTV_HUD_LEN];
static int hud_color[GTV_HUD_LINES];
static int hud_lines;

static LONGLONG qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static float qpc_ms(LONGLONG d)
{
    return (float)((double)d * 1000.0 / (double)freq.QuadPart);
}

void gtv_perf_gpu_cmd(unsigned type, unsigned us)
{
    if (!inited) {
        return;
    }
    EnterCriticalSection(&lock);
    gpu_us_sum += us;
    if (us > gpu_us_max) {
        gpu_us_max = us;
        gpu_max_type = type;
    }
    if (us >= SLOW_GPU_US) {
        ev_add(qpc(), us / 1000.0f, type);
    }
    LeaveCriticalSection(&lock);
}

void gtv_perf_mainloop_tick(void)
{
    LONGLONG now;

    if (!inited) {
        return;
    }
    now = qpc();
    EnterCriticalSection(&lock);
    if (last_tick) {
        /* the timer asks for GTV_TICK_MS; anything above is a delay */
        float late = qpc_ms(now - last_tick) - GTV_TICK_MS;
        if (late > loop_max_ms) {
            loop_max_ms = late;
        }
        if (late >= SLOW_LOOP_MS) {
            ev_add(now, late, 0);
        }
    }
    last_tick = now;
    LeaveCriticalSection(&lock);
}

/*
 * A frame came ms late (ending now): add up what the slow events of that
 * time say and print one line with the likely cause.
 */
static void log_stall(LONGLONG now, float ms)
{
    LONGLONG from = now - (LONGLONG)((double)ms * (double)freq.QuadPart / 1000.0);
    float gpu = 0, gpu_max = 0, loop = 0;
    unsigned gpu_type = 0, gpu_n = 0;
    const char *cause;

    EnterCriticalSection(&lock);
    for (unsigned i = 0; i < SLOW_RING && i < ev_pos; i++) {
        struct ev *e = &evs[(ev_pos - 1 - i) % SLOW_RING];
        if (e->end < from) {
            break;
        }
        if (e->type) {
            gpu += e->ms;
            gpu_n++;
            if (e->ms > gpu_max) {
                gpu_max = e->ms;
                gpu_type = e->type;
            }
        } else {
            loop += e->ms;
        }
    }
    LeaveCriticalSection(&lock);

    if (gpu >= ms * 0.4f) {
        cause = "host-gpu";             /* virgl / host GL driver */
    } else if (loop >= ms * 0.4f) {
        cause = "qemu-mainloop";        /* main loop held, not by the GPU */
    } else {
        cause = "guest";                /* game / Android did not draw */
    }
    printf("[PERF] stall %d ms cause=%s gpu=%dms(%u cmds, max %s %dms) mainloop_late=%dms\n",
           (int)ms, cause, (int)gpu, gpu_n, gpu_n ? gpu_cmd_name(gpu_type) : "-",
           (int)gpu_max, (int)loop);
    fflush(stdout);
}

void gtv_perf_frame(void)
{
    LONGLONG now;
    float ms = 0;

    if (!inited) {
        return;
    }
    now = qpc();
    EnterCriticalSection(&lock);
    if (last_frame) {
        ms = qpc_ms(now - last_frame);
        ring[ring_pos % RING].t = now;
        ring[ring_pos % RING].ms = ms;
        ring_pos++;
    }
    last_frame = now;
    frames++;
    LeaveCriticalSection(&lock);

    /* only while the game was animating: a still menu sends no frames */
    if (ms > STALL_LOG_MS && fps_now >= 10) {
        log_stall(now, ms);
    }
}

void gtv_perf_swap_us(unsigned us)
{
    if (!inited) {
        return;
    }
    EnterCriticalSection(&lock);
    swap_us_sum += us;
    swap_n++;
    if (us > swap_us_max) {
        swap_us_max = us;
    }
    LeaveCriticalSection(&lock);
}

void gtv_perf_gl_info(const char *renderer, const char *vendor, const char *version)
{
    static const char *soft[] = {
        "GDI Generic", "llvmpipe", "softpipe", "Microsoft Basic Render", "SwiftShader",
    };

    snprintf(gl_renderer, sizeof(gl_renderer), "%s", renderer ? renderer : "?");
    for (size_t i = 0; i < sizeof(soft) / sizeof(soft[0]); i++) {
        if (renderer && strstr(renderer, soft[i])) {
            gl_software = 1;
        }
    }
    printf("[PERF] gl renderer=\"%s\" vendor=\"%s\" version=\"%s\"%s\n",
           renderer ? renderer : "?", vendor ? vendor : "?", version ? version : "?",
           gl_software ? " SOFTWARE-RENDERING" : "");
    fflush(stdout);
}

void gtv_perf_net(unsigned rx, unsigned tx)
{
    if (rx) {
        InterlockedExchangeAdd64(&net_rx, rx);
    }
    if (tx) {
        InterlockedExchangeAdd64(&net_tx, tx);
    }
}

void gtv_perf_guest_stat(const char *fields)
{
    int cpu = -1, game = -1, avail = -1, swap = -1, arm = -1;

    if (!inited ||
        sscanf(fields, "cpu=%d game=%d avail=%d swap=%d arm=%d",
               &cpu, &game, &avail, &swap, &arm) < 4) {
        return;
    }
    if (arm != g_arm_logged && arm >= 0) {
        g_arm_logged = arm;
        printf("[PERF] game code: %s\n", arm == 1
               ? "ARM only, runs through the ARM->x86 translator "
                 "(stalls when new scenes/menus run for the first time; "
                 "an x86_64 build of the game fixes it)"
               : "native x86_64");
        fflush(stdout);
    }
    EnterCriticalSection(&lock);
    g_cpu = cpu;
    g_game = game;
    g_avail = avail;
    g_swap = swap;
    g_arm = arm;
    g_time = GetTickCount64();
    LeaveCriticalSection(&lock);
}

int gtv_perf_hud_visible(void)
{
    return hud_on;
}

void gtv_perf_hud_toggle(void)
{
    LONG on = !hud_on;
    InterlockedExchange(&hud_on, on);
    printf("[PERF] HUD %s\n", on ? "on" : "off");
    fflush(stdout);
}

unsigned gtv_perf_ms_since_frame(void)
{
    LONGLONG last;

    if (!inited) {
        return 0;
    }
    EnterCriticalSection(&lock);
    last = last_frame;
    LeaveCriticalSection(&lock);
    return last ? (unsigned)qpc_ms(qpc() - last) : 0;
}

int gtv_perf_hud_text(char lines[GTV_HUD_LINES][GTV_HUD_LEN], int colors[GTV_HUD_LINES])
{
    int n;

    if (!inited) {
        return 0;
    }
    EnterCriticalSection(&lock);
    n = hud_lines;
    memcpy(lines, hud, sizeof(hud));
    memcpy(colors, hud_color, sizeof(hud_color));
    LeaveCriticalSection(&lock);
    return n;
}

/* ===================== network: RTT / retransmissions ===================== */

typedef DWORD (WINAPI *GetExtendedTcpTable_t)(PVOID, PDWORD, BOOL, ULONG, TCP_TABLE_CLASS, ULONG);
typedef ULONG (WINAPI *SetPerTcpConnectionEStats_t)(PMIB_TCPROW, TCP_ESTATS_TYPE, PUCHAR,
                                                    ULONG, ULONG, ULONG);
typedef ULONG (WINAPI *GetPerTcpConnectionEStats_t)(PMIB_TCPROW, TCP_ESTATS_TYPE,
                                                    PUCHAR, ULONG, ULONG,
                                                    PUCHAR, ULONG, ULONG,
                                                    PUCHAR, ULONG, ULONG);
typedef HANDLE (WINAPI *IcmpCreateFile_t)(void);
typedef DWORD (WINAPI *IcmpSendEcho_t)(HANDLE, IPAddr, LPVOID, WORD, PIP_OPTION_INFORMATION,
                                       LPVOID, DWORD, DWORD);

static GetExtendedTcpTable_t p_GetExtendedTcpTable;
static SetPerTcpConnectionEStats_t p_SetEStats;
static GetPerTcpConnectionEStats_t p_GetEStats;
static IcmpCreateFile_t p_IcmpCreateFile;
static IcmpSendEcho_t p_IcmpSendEcho;
static HANDLE icmp = INVALID_HANDLE_VALUE;
static int estats_ok = 1;       /* cleared when Windows refuses (not admin) */

#define MAX_CONN 64
static struct conn {
    MIB_TCPROW row;
    ULONG64 bytes;              /* in + out, last sample */
    ULONG64 retrans, segs;
    int seen;
} conns[MAX_CONN];
static int n_conns;

static void net_load(void)
{
    HMODULE h = LoadLibraryA("iphlpapi.dll");

    if (!h) {
        return;
    }
    p_GetExtendedTcpTable = (GetExtendedTcpTable_t)(void *)GetProcAddress(h, "GetExtendedTcpTable");
    p_SetEStats = (SetPerTcpConnectionEStats_t)(void *)GetProcAddress(h, "SetPerTcpConnectionEStats");
    p_GetEStats = (GetPerTcpConnectionEStats_t)(void *)GetProcAddress(h, "GetPerTcpConnectionEStats");
    p_IcmpCreateFile = (IcmpCreateFile_t)(void *)GetProcAddress(h, "IcmpCreateFile");
    p_IcmpSendEcho = (IcmpSendEcho_t)(void *)GetProcAddress(h, "IcmpSendEcho");
    if (!p_SetEStats || !p_GetEStats) {
        estats_ok = 0;
    }
    if (p_IcmpCreateFile) {
        icmp = p_IcmpCreateFile();
    }
}

static int same_conn(const MIB_TCPROW *a, const MIB_TCPROW *b)
{
    return a->dwLocalPort == b->dwLocalPort && a->dwRemoteAddr == b->dwRemoteAddr &&
           a->dwRemotePort == b->dwRemotePort && a->dwLocalAddr == b->dwLocalAddr;
}

static int ping_ms(DWORD addr)
{
    char data[8] = "gplayer", reply[sizeof(ICMP_ECHO_REPLY) + 64];

    if (icmp == INVALID_HANDLE_VALUE || !p_IcmpSendEcho) {
        return -1;
    }
    if (p_IcmpSendEcho(icmp, addr, data, sizeof(data), NULL, reply, sizeof(reply), 1000) == 0) {
        return -1;
    }
    PICMP_ECHO_REPLY r = (PICMP_ECHO_REPLY)reply;
    return r->Status == IP_SUCCESS ? (int)r->RoundTripTime : -1;
}

/*
 * RTT (ms, -1 unknown) to the peer the game talks to most, and the share of
 * retransmitted segments (%, -1 unknown); *peer gets its address.
 */
static void net_sample(int *rtt, float *retrans_pct, char *peer, size_t peer_len, int tick)
{
    static int last_rtt = -1;
    static DWORD ping_addr;
    DWORD size = 0, pid = GetCurrentProcessId();
    MIB_TCPTABLE_OWNER_PID *tab;
    ULONG64 d_retrans = 0, d_segs = 0, best_d = 0;
    int best = -1;

    *rtt = -1;
    *retrans_pct = -1;
    snprintf(peer, peer_len, "-");
    if (!p_GetExtendedTcpTable) {
        return;
    }
    p_GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    tab = malloc(size + 1024);
    size += 1024;
    if (!tab || p_GetExtendedTcpTable(tab, &size, FALSE, AF_INET,
                                      TCP_TABLE_OWNER_PID_CONNECTIONS, 0) != NO_ERROR) {
        free(tab);
        return;
    }

    for (int i = 0; i < n_conns; i++) {
        conns[i].seen = 0;
    }
    for (DWORD i = 0; i < tab->dwNumEntries; i++) {
        MIB_TCPROW_OWNER_PID *r = &tab->table[i];
        MIB_TCPROW row;
        int k;

        /* the game's connections only: not adb/QMP/clipboard on loopback */
        if (r->dwOwningPid != pid || r->dwState != MIB_TCP_STATE_ESTAB ||
            (r->dwRemoteAddr & 0xff) == 127) {
            continue;
        }
        memset(&row, 0, sizeof(row));
        row.dwState = r->dwState;
        row.dwLocalAddr = r->dwLocalAddr;
        row.dwLocalPort = r->dwLocalPort;
        row.dwRemoteAddr = r->dwRemoteAddr;
        row.dwRemotePort = r->dwRemotePort;

        for (k = 0; k < n_conns && !same_conn(&conns[k].row, &row); k++) {
        }
        if (k == n_conns) {
            if (n_conns == MAX_CONN) {
                continue;
            }
            memset(&conns[k], 0, sizeof(conns[k]));
            conns[k].row = row;
            n_conns++;
            if (estats_ok) {
                TCP_ESTATS_DATA_RW_v0 drw = { TRUE };
                TCP_ESTATS_PATH_RW_v0 prw = { TRUE };
                if (p_SetEStats(&row, TcpConnectionEstatsData, (PUCHAR)&drw, 0,
                                sizeof(drw), 0) != NO_ERROR ||
                    p_SetEStats(&row, TcpConnectionEstatsPath, (PUCHAR)&prw, 0,
                                sizeof(prw), 0) != NO_ERROR) {
                    estats_ok = 0;
                    printf("[PERF] TCP statistics unavailable (not administrator?), using ping\n");
                    fflush(stdout);
                }
            }
        }
        conns[k].seen = 1;

        if (estats_ok) {
            TCP_ESTATS_DATA_ROD_v0 d;
            TCP_ESTATS_PATH_ROD_v0 p;
            memset(&d, 0, sizeof(d));
            memset(&p, 0, sizeof(p));
            if (p_GetEStats(&row, TcpConnectionEstatsData, NULL, 0, 0, NULL, 0, 0,
                            (PUCHAR)&d, 0, sizeof(d)) == NO_ERROR &&
                p_GetEStats(&row, TcpConnectionEstatsPath, NULL, 0, 0, NULL, 0, 0,
                            (PUCHAR)&p, 0, sizeof(p)) == NO_ERROR) {
                ULONG64 bytes = d.DataBytesIn + d.DataBytesOut;
                ULONG64 db = bytes - conns[k].bytes;
                if (conns[k].bytes && db >= best_d && p.SmoothedRtt) {
                    best_d = db;
                    best = k;
                    *rtt = (int)p.SmoothedRtt;
                }
                if (conns[k].bytes) {
                    d_retrans += p.PktsRetrans - conns[k].retrans;
                    d_segs += d.SegsOut - conns[k].segs;
                }
                conns[k].bytes = bytes;
                conns[k].retrans = p.PktsRetrans;
                conns[k].segs = d.SegsOut;
            }
        } else if (best < 0) {
            best = k;               /* ping the first (oldest) connection */
        }
    }
    free(tab);

    /* forget closed connections */
    for (int i = 0; i < n_conns;) {
        if (!conns[i].seen) {
            conns[i] = conns[--n_conns];
        } else {
            i++;
        }
    }

    if (best >= 0) {
        struct in_addr a;
        a.s_addr = conns[best].row.dwRemoteAddr;
        snprintf(peer, peer_len, "%s:%u", inet_ntoa(a),
                 ntohs((u_short)conns[best].row.dwRemotePort));
        ping_addr = conns[best].row.dwRemoteAddr;
    }
    if (estats_ok) {
        if (d_segs) {
            *retrans_pct = (float)d_retrans * 100.0f / (float)d_segs;
        }
        return;
    }

    /* no TCP statistics: ping every 2 s, the game's peer or 8.8.8.8 */
    if (tick % 2 == 0) {
        if (!ping_addr) {
            ping_addr = inet_addr("8.8.8.8");
        }
        last_rtt = ping_ms(ping_addr);
    }
    *rtt = last_rtt;
}

/* ============================ sampling thread ============================ */

static ULONGLONG ft64(FILETIME f)
{
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static DWORD WINAPI perf_thread(LPVOID arg)
{
    FILETIME idle0, kern0, user0, pc, pe, pk0, pu0;
    SYSTEM_INFO si;
    LONGLONG t0 = qpc();
    LONG64 rx0 = 0, tx0 = 0;
    static float gaps[RING];
    /* sums over LOG_EVERY samples for the [PERF] line */
    double sum_fps = 0, sum_host = 0, sum_qemu = 0, sum_swap = 0, sum_gpu = 0;
    unsigned sum_gpumax = 0, sum_gputype = 0;
    float sum_loopmax = 0;
    int sum_n = 0, sum_low = 1000, sum_stall = 0, sum_maxgap = 0, sum_swapmax = 0;
    LONG64 sum_rx = 0, sum_tx = 0;
    int tick = 0;

    GetSystemInfo(&si);
    GetSystemTimes(&idle0, &kern0, &user0);
    GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk0, &pu0);
    net_load();

    for (;;) {
        FILETIME idle, kern, user, pk, pu;
        MEMORYSTATUSEX ms = { sizeof(ms) };
        PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
        LONGLONG now, since_frame = 0;
        unsigned nframes, n_gaps = 0, stalls = 0, swap_n_, swap_max_;
        unsigned long long swap_sum_, gpu_sum_;
        unsigned gpu_max_, gpu_type_;
        float loop_max_;
        float elapsed, fps, low1 = 0, max_gap = 0, swap_avg;
        int host, qemu, gcpu, ggame, gavail, gswap, garm, guest_ok, rtt;
        float retrans;
        char peer[32];

        Sleep(SAMPLE_MS);
        tick++;
        now = qpc();
        elapsed = qpc_ms(now - t0) / 1000.0f;
        t0 = now;

        /* ---- frames ---- */
        EnterCriticalSection(&lock);
        nframes = frames;
        frames = 0;
        for (unsigned i = 0; i < RING && i < ring_pos; i++) {
            unsigned j = (ring_pos - 1 - i) % RING;
            if (qpc_ms(now - ring[j].t) > WINDOW_MS) {
                break;
            }
            gaps[n_gaps++] = ring[j].ms;
            if (ring[j].ms > STALL_MS) {
                stalls++;
            }
        }
        if (last_frame) {
            since_frame = now - last_frame;
        }
        gpu_sum_ = gpu_us_sum;
        gpu_max_ = gpu_us_max;
        gpu_type_ = gpu_max_type;
        loop_max_ = loop_max_ms;
        gpu_us_sum = 0;
        gpu_us_max = gpu_max_type = 0;
        loop_max_ms = 0;
        swap_sum_ = swap_us_sum;
        swap_n_ = swap_n;
        swap_max_ = swap_us_max;
        swap_us_sum = 0;
        swap_n = swap_us_max = 0;
        gcpu = g_cpu;
        ggame = g_game;
        gavail = g_avail;
        gswap = g_swap;
        garm = g_arm;
        guest_ok = g_time && GetTickCount64() - g_time < 6000;
        LeaveCriticalSection(&lock);

        fps = elapsed > 0 ? nframes / elapsed : 0;
        fps_now = (int)(fps + 0.5f);
        if (n_gaps >= 10) {
            qsort(gaps, n_gaps, sizeof(gaps[0]), cmp_float);
            float p99 = gaps[(n_gaps * 99) / 100];
            max_gap = gaps[n_gaps - 1];
            low1 = p99 > 0 ? 1000.0f / p99 : 0;
        }
        swap_avg = swap_n_ ? (float)swap_sum_ / swap_n_ / 1000.0f : 0;

        /* ---- CPU / RAM ---- */
        GetSystemTimes(&idle, &kern, &user);
        GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu);
        {
            ULONGLONG di = ft64(idle) - ft64(idle0);
            ULONGLONG dt = ft64(kern) - ft64(kern0) + ft64(user) - ft64(user0);
            ULONGLONG dp = ft64(pk) - ft64(pk0) + ft64(pu) - ft64(pu0);
            host = dt ? (int)((dt - di) * 100 / dt) : 0;
            /* QEMU's share of the whole machine, like the host number */
            qemu = dt ? (int)(dp * 100 / dt) : 0;
        }
        idle0 = idle;
        kern0 = kern;
        user0 = user;
        pk0 = pk;
        pu0 = pu;
        GlobalMemoryStatusEx(&ms);
        K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));

        /* ---- network ---- */
        LONG64 rx = net_rx, tx = net_tx;
        float rx_kb = (rx - rx0) / 1024.0f / elapsed, tx_kb = (tx - tx0) / 1024.0f / elapsed;
        rx0 = rx;
        tx0 = tx;
        net_sample(&rtt, &retrans, peer, sizeof(peer), tick);

        /* ---- HUD ---- */
        {
            char l[GTV_HUD_LINES][GTV_HUD_LEN];
            int c[GTV_HUD_LINES] = { 0 };
            unsigned frozen = since_frame ? (unsigned)qpc_ms(since_frame) : 0;

            /*
             * No frames while the game uses (almost) no CPU: a still screen
             * (menu, login page) that Android does not redraw, not a freeze.
             */
            if (fps_now == 0 && guest_ok && ggame >= 0 && ggame < 5) {
                snprintf(l[0], GTV_HUD_LEN, "FPS 0 (màn hình tĩnh)   khựng %u (5s)", stalls);
                c[0] = 0;
            } else if (frozen > 500 && fps_now == 0 && n_gaps) {
                snprintf(l[0], GTV_HUD_LEN, "FPS 0   đứng hình %u ms   khựng %u (5s)",
                         frozen, stalls);
                c[0] = 2;
            } else {
                snprintf(l[0], GTV_HUD_LEN, "FPS %d   1%% low %d   khựng %u (5s)   max %d ms",
                         fps_now, (int)(low1 + 0.5f), stalls, (int)max_gap);
                c[0] = fps_now < 20 ? 2 : fps_now < 30 || stalls > 2 ? 1 : 0;
            }
            if (guest_ok) {
                char game[24] = "";
                if (ggame >= 0) {
                    snprintf(game, sizeof(game), " (game %d%%)", ggame);
                }
                snprintf(l[1], GTV_HUD_LEN, "CPU máy %d%%   QEMU %d%%   guest %d%%%s",
                         host, qemu, gcpu, game);
                c[1] = gcpu >= 90 || host >= 95 ? 2 : gcpu >= 75 || host >= 85 ? 1 : 0;
                snprintf(l[2], GTV_HUD_LEN, "RAM máy trống %.1f GB   guest trống %d MB   swap %d MB",
                         ms.ullAvailPhys / 1073741824.0, gavail, gswap);
                c[2] = gavail < 150 || ms.dwMemoryLoad >= 95 ? 2
                     : gavail < 300 || gswap > 300 || ms.dwMemoryLoad >= 90 ? 1 : 0;
            } else {
                snprintf(l[1], GTV_HUD_LEN, "CPU máy %d%%   QEMU %d%%   guest -", host, qemu);
                c[1] = host >= 95 ? 2 : host >= 85 ? 1 : 0;
                snprintf(l[2], GTV_HUD_LEN, "RAM máy trống %.1f GB   guest -",
                         ms.ullAvailPhys / 1073741824.0);
                c[2] = ms.dwMemoryLoad >= 95 ? 2 : ms.dwMemoryLoad >= 90 ? 1 : 0;
            }
            if (rtt >= 0) {
                char rt[24] = "";
                if (retrans >= 0) {
                    snprintf(rt, sizeof(rt), "   gửi lại %.1f%%", retrans);
                }
                snprintf(l[3], GTV_HUD_LEN, "MẠNG %d ms%s   nhận %.0f KB/s   gửi %.0f KB/s",
                         rtt, rt, rx_kb, tx_kb);
                c[3] = rtt > 250 || retrans > 5 ? 2 : rtt > 120 || retrans > 1 ? 1 : 0;
            } else {
                snprintf(l[3], GTV_HUD_LEN, "MẠNG -   nhận %.0f KB/s   gửi %.0f KB/s", rx_kb, tx_kb);
                c[3] = 0;
            }
            snprintf(l[4], GTV_HUD_LEN, "GPU %.40s   bận %.0f%% (max %s %.0f ms)   loop trễ %.0f ms",
                     gl_renderer, gpu_sum_ / 10.0 / (elapsed * 1000.0),
                     gpu_max_ ? gpu_cmd_name(gpu_type_) : "-", gpu_max_ / 1000.0f, loop_max_);
            c[4] = gl_software || gpu_max_ > 250000 ? 2 : gpu_max_ > 50000 || loop_max_ > 100 ? 1 : 0;
            snprintf(l[5], GTV_HUD_LEN, "%s   game %s   Ctrl+Shift+F ẩn", vm_info,
                     !guest_ok || garm < 0 ? "?" : garm ? "ARM (dịch sang x86)" : "x86_64");
            c[5] = guest_ok && garm == 1 ? 1 : 0;

            EnterCriticalSection(&lock);
            memcpy(hud, l, sizeof(hud));
            memcpy(hud_color, c, sizeof(hud_color));
            hud_lines = GTV_HUD_LINES;
            LeaveCriticalSection(&lock);
        }

        /* ---- [PERF] log line ---- */
        sum_fps += fps;
        sum_host += host;
        sum_qemu += qemu;
        sum_swap += swap_avg;
        sum_gpu += gpu_sum_ / 10.0 / (elapsed * 1000.0);
        if (gpu_max_ > sum_gpumax) {
            sum_gpumax = gpu_max_;
            sum_gputype = gpu_type_;
        }
        if (loop_max_ > sum_loopmax) {
            sum_loopmax = loop_max_;
        }
        sum_n++;
        if (n_gaps >= 10 && (int)low1 < sum_low) {
            sum_low = (int)low1;
        }
        sum_stall = stalls > (unsigned)sum_stall ? (int)stalls : sum_stall;
        if ((int)max_gap > sum_maxgap) {
            sum_maxgap = (int)max_gap;
        }
        if ((int)swap_max_ / 1000 > sum_swapmax) {
            sum_swapmax = (int)swap_max_ / 1000;
        }
        sum_rx += (LONG64)(rx_kb * elapsed);
        sum_tx += (LONG64)(tx_kb * elapsed);
        if (sum_n >= LOG_EVERY) {
            printf("[PERF] fps=%.0f low1=%d stall100=%d maxgap=%dms swap=%.1fms/%dms "
                   "gpu_busy=%.0f%% gpu_max=%s/%ums loop_late=%.0fms "
                   "cpu_host=%.0f%% cpu_qemu=%.0f%% cpu_guest=%d%% game=%d%% "
                   "ram_free=%lluMB mem_load=%lu%% qemu_ws=%lluMB guest_avail=%dMB guest_swap=%dMB arm=%d "
                   "rtt=%dms retrans=%.1f%% peer=%s rx=%.0fKB/s tx=%.0fKB/s\n",
                   sum_fps / sum_n, sum_low == 1000 ? -1 : sum_low, sum_stall, sum_maxgap,
                   sum_swap / sum_n, sum_swapmax,
                   sum_gpu / sum_n, sum_gpumax ? gpu_cmd_name(sum_gputype) : "-",
                   sum_gpumax / 1000, sum_loopmax,
                   sum_host / sum_n, sum_qemu / sum_n,
                   guest_ok ? gcpu : -1, guest_ok ? ggame : -1,
                   (unsigned long long)(ms.ullAvailPhys >> 20), ms.dwMemoryLoad,
                   (unsigned long long)(pmc.WorkingSetSize >> 20),
                   guest_ok ? gavail : -1, guest_ok ? gswap : -1, guest_ok ? garm : -1,
                   rtt, retrans, peer,
                   (double)sum_rx / LOG_EVERY, (double)sum_tx / LOG_EVERY);
            fflush(stdout);
            sum_fps = sum_host = sum_qemu = sum_swap = sum_gpu = 0;
            sum_gpumax = sum_gputype = 0;
            sum_loopmax = 0;
            sum_n = sum_stall = sum_maxgap = sum_swapmax = 0;
            sum_low = 1000;
            sum_rx = sum_tx = 0;
        }
    }
    return 0;
}

void gtv_perf_start(const char *accel, int vcpus, int ram_mb)
{
    static LONG started;
    const char *env = getenv("GPLAYER_HUD");
    HANDLE t;

    if (InterlockedExchange(&started, 1)) {
        return;
    }
    QueryPerformanceFrequency(&freq);
    InitializeCriticalSection(&lock);
    snprintf(vm_info, sizeof(vm_info), "%s   %d vCPU   %d MB",
             accel ? accel : "?", vcpus, ram_mb);
    for (char *p = vm_info; *p && *p != ' '; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p -= 'a' - 'A';
        }
    }
    if (env && *env == '1') {
        hud_on = 1;
    }
    InterlockedExchange(&inited, 1);

    t = CreateThread(NULL, 0, perf_thread, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    }
    printf("[PERF] started (%s), HUD %s (Ctrl+Shift+F), power throttling %s%s\n",
           vm_info, hud_on ? "on" : "off",
           os_win32_throttling_off ? "off" : "NOT changed",
           accel && !strcmp(accel, "tcg")
           ? " SLOW: no hardware acceleration (TCG software emulation)" : "");
    fflush(stdout);
}

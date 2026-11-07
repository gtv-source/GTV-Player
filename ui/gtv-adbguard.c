/*
 * G-Player: only G-Player's own adb clients may reach the VM's adb.
 *
 * Without it any program on the host (adb in cmd, scrcpy, auto-clickers) could
 * open a root shell in the guest while adbd is up. The adb port and the adb
 * server port are random and the shared server (5037) is not used, so nothing
 * finds the VM by accident; this thread drops whatever connects on purpose:
 *  - to fwd_port (slirp hostfwd -> guest adbd): only our adb server may;
 *  - to server_port (our adb server): only adb.exe processes QEMU started.
 * Dropping a TCP connection of another process (SetTcpEntry DELETE_TCB) needs
 * administrator rights, which GPlayer.exe has; without them it is only logged.
 * iphlpapi.dll is loaded at run time: QEMU does not link it.
 */
#include "qemu/osdep.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "gtv-adbguard.h"

#define GUARD_MS    20
#define MAX_ALLOWED 32
#define MAX_LOGGED  20

typedef DWORD (WINAPI *GetExtendedTcpTable_t)(PVOID, PDWORD, BOOL, ULONG, TCP_TABLE_CLASS, ULONG);
typedef DWORD (WINAPI *SetTcpEntry_t)(PMIB_TCPROW);

static GetExtendedTcpTable_t p_GetExtendedTcpTable;
static SetTcpEntry_t p_SetTcpEntry;

static CRITICAL_SECTION lock;
static DWORD allowed[MAX_ALLOWED];
static volatile LONG running;
static HANDLE thread;
static int fwd_port, server_port;
static volatile DWORD server_pid;
static int blocked, dropped, logged;

static int is_allowed(DWORD pid)
{
    int ok = 0;

    EnterCriticalSection(&lock);
    for (int i = 0; i < MAX_ALLOWED && !ok; i++) {
        ok = allowed[i] == pid;
    }
    LeaveCriticalSection(&lock);
    return ok;
}

void gtv_adbguard_allow_pid(DWORD pid)
{
    if (!running) {
        return;
    }
    EnterCriticalSection(&lock);
    for (int i = 0; i < MAX_ALLOWED; i++) {
        if (!allowed[i]) {
            allowed[i] = pid;
            break;
        }
    }
    LeaveCriticalSection(&lock);
}

void gtv_adbguard_forget_pid(DWORD pid)
{
    if (!running) {
        return;
    }
    EnterCriticalSection(&lock);
    for (int i = 0; i < MAX_ALLOWED; i++) {
        if (allowed[i] == pid) {
            allowed[i] = 0;
        }
    }
    LeaveCriticalSection(&lock);
}

DWORD gtv_adbguard_server_pid(void)
{
    return server_pid;
}

static void exe_name(DWORD pid, char *out, size_t len)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    char path[MAX_PATH];
    DWORD n = sizeof(path);

    snprintf(out, len, "?");
    if (!h) {
        return;
    }
    if (QueryFullProcessImageNameA(h, 0, path, &n)) {
        const char *base = strrchr(path, '\\');
        snprintf(out, len, "%s", base ? base + 1 : path);
    }
    CloseHandle(h);
}

/*
 * Connections already reported (pid + client port): one that cannot be
 * dropped is seen on every scan, it is counted and logged once.
 */
#define SEEN_LEN 64
static struct { DWORD pid, port; } seen[SEEN_LEN];
static int seen_next;

static int first_time(DWORD pid, DWORD port)
{
    for (int i = 0; i < SEEN_LEN; i++) {
        if (seen[i].pid == pid && seen[i].port == port) {
            return 0;
        }
    }
    seen[seen_next].pid = pid;
    seen[seen_next].port = port;
    seen_next = (seen_next + 1) % SEEN_LEN;
    return 1;
}

/* the client end of a foreign connection: cut it and say who it was */
static void block(const MIB_TCPROW_OWNER_PID *r, const char *what)
{
    MIB_TCPROW row = {
        .dwState = MIB_TCP_STATE_DELETE_TCB,
        .dwLocalAddr = r->dwLocalAddr, .dwLocalPort = r->dwLocalPort,
        .dwRemoteAddr = r->dwRemoteAddr, .dwRemotePort = r->dwRemotePort,
    };
    DWORD err = p_SetTcpEntry ? p_SetTcpEntry(&row) : ERROR_NOT_SUPPORTED;
    char exe[MAX_PATH];

    if (!first_time(r->dwOwningPid, r->dwLocalPort)) {
        return;
    }
    blocked++;
    if (err == NO_ERROR) {
        dropped++;
    }
    if (logged++ < MAX_LOGGED) {
        exe_name(r->dwOwningPid, exe, sizeof(exe));
        if (err == NO_ERROR) {
            printf("[ADB] SECURITY: blocked connection to the %s from pid=%lu exe=%s\n",
                   what, r->dwOwningPid, exe);
        } else {
            printf("[ADB] SECURITY: foreign connection to the %s from pid=%lu exe=%s "
                   "(could not drop it: error %lu, not administrator?)\n",
                   what, r->dwOwningPid, exe, err);
        }
        fflush(stdout);
    }
}

static void scan(void)
{
    static MIB_TCPTABLE_OWNER_PID *tab;
    static DWORD cap;
    DWORD size = cap;
    DWORD me = GetCurrentProcessId();
    DWORD srv = 0, err;

    err = p_GetExtendedTcpTable(tab, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (err == ERROR_INSUFFICIENT_BUFFER) {
        free(tab);
        cap = size + 4096;
        tab = malloc(cap);
        size = cap;
        if (!tab) {
            cap = 0;
            return;
        }
        err = p_GetExtendedTcpTable(tab, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    }
    if (err != NO_ERROR) {
        return;
    }

    /* who is our adb server: the process listening on server_port */
    for (DWORD i = 0; i < tab->dwNumEntries; i++) {
        MIB_TCPROW_OWNER_PID *r = &tab->table[i];
        if (r->dwState == MIB_TCP_STATE_LISTEN &&
            ntohs((u_short)r->dwLocalPort) == server_port) {
            srv = r->dwOwningPid;
        }
    }
    server_pid = srv;

    for (DWORD i = 0; i < tab->dwNumEntries; i++) {
        MIB_TCPROW_OWNER_PID *r = &tab->table[i];
        int rport = ntohs((u_short)r->dwRemotePort);
        DWORD pid = r->dwOwningPid;

        /* client ends only, of live connections (TIME_WAIT has no owner) */
        if (r->dwState != MIB_TCP_STATE_SYN_SENT && r->dwState != MIB_TCP_STATE_ESTAB) {
            continue;
        }
        if (!pid || pid == me) {
            continue;
        }
        if (rport == fwd_port && fwd_port) {
            if (pid != srv) {
                block(r, "VM adb port");
            }
        } else if (rport == server_port && server_port) {
            if (pid != srv && !is_allowed(pid)) {
                block(r, "G-Player adb server");
            }
        }
    }
}

static DWORD WINAPI guard_thread(LPVOID arg)
{
    while (running) {
        scan();
        Sleep(GUARD_MS);
    }
    return 0;
}

void gtv_adbguard_start(int fwd, int server)
{
    static int init;
    HMODULE h;

    if (running) {
        return;
    }
    if (!init) {
        InitializeCriticalSection(&lock);
        init = 1;
    }
    h = LoadLibraryA("iphlpapi.dll");
    if (h) {
        p_GetExtendedTcpTable = (GetExtendedTcpTable_t)(void *)GetProcAddress(h, "GetExtendedTcpTable");
        p_SetTcpEntry = (SetTcpEntry_t)(void *)GetProcAddress(h, "SetTcpEntry");
    }
    if (!p_GetExtendedTcpTable) {
        printf("[ADB] SECURITY: iphlpapi not available, adb connections not guarded\n");
        return;
    }
    fwd_port = fwd;
    server_port = server;
    running = 1;
    thread = CreateThread(NULL, 0, guard_thread, NULL, 0, NULL);
    if (!thread) {
        running = 0;
        return;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    printf("[ADB] guard on: adb port %d, adb server port %d\n", fwd, server);
}

void gtv_adbguard_stop(void)
{
    if (!InterlockedExchange(&running, 0)) {
        return;
    }
    if (thread) {
        WaitForSingleObject(thread, 1000);
        CloseHandle(thread);
        thread = NULL;
    }
    printf("[ADB] guard off: %d foreign connection(s) %s\n", blocked,
           dropped == blocked ? "blocked" : "seen (not all could be dropped)");
}

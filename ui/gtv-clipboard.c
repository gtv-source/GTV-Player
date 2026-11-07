/*
 * G-Player: host <-> Android clipboard (text), see gtv-clipboard.h.
 *
 * One thread: accept a connection from GtvClip in the guest, check its token,
 * then
 *   - send "SET <base64 UTF-8>\n" with the current host clipboard text and
 *     again on every change (ui/sdl2.c calls gtv_clip_host_text());
 *   - read "CLIP <base64 UTF-8>\n" lines (a copy inside Android) and put the
 *     text on the Windows clipboard.  That text is also stored as the current
 *     text, so the clipboard change it causes is not sent back.
 *
 * Plain Win32/Winsock like ui/adb.c (no qemu/osdep.h: its socket wrappers
 * would turn SOCKETs into CRT fds).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _CRT_RAND_S
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gtv-clipboard.h"
#include "gtv-perf.h"
#include "gtv-ux.h"

#define CLIP_MAX (1024 * 1024)        /* larger texts are not sent */
#define LINE_MAX_B64 (CLIP_MAX / 3 * 4 + 16)

static CRITICAL_SECTION lock;
static HANDLE changed;                /* auto-reset: a new text is waiting */
static char *text;                    /* latest text (no CRs), under lock */
static unsigned long text_gen;        /* bumped on every host change */
static char token[33];
static SOCKET listener = INVALID_SOCKET;
static int port;

static char *base64(const char *in, size_t len)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc((len + 2) / 3 * 4 + 1), *p = out;
    size_t i;

    if (!out) {
        return NULL;
    }
    for (i = 0; i + 2 < len; i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 |
                     (unsigned char)in[i + 2];
        *p++ = tbl[v >> 18];
        *p++ = tbl[(v >> 12) & 63];
        *p++ = tbl[(v >> 6) & 63];
        *p++ = tbl[v & 63];
    }
    if (i < len) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < len) {
            v |= (unsigned char)in[i + 1] << 8;
        }
        *p++ = tbl[v >> 18];
        *p++ = tbl[(v >> 12) & 63];
        *p++ = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        *p++ = '=';
    }
    *p = 0;
    return out;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* decode base64 (stops at '=' or junk); NUL-terminated, NULL on OOM */
static char *unbase64(const char *in)
{
    char *out = malloc(strlen(in) / 4 * 3 + 4), *p = out;
    unsigned v = 0;
    int bits = 0, d;

    if (!out) {
        return NULL;
    }
    for (; *in && (d = b64val(*in)) >= 0; in++) {
        v = v << 6 | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            *p++ = (char)(v >> bits);
        }
    }
    *p = 0;
    return out;
}

/* copy without the CR of CRLF, so both sides compare the same text */
static char *strip_cr(const char *in)
{
    char *out = malloc(strlen(in) + 1), *p = out;

    if (!out) {
        return NULL;
    }
    for (; *in; in++) {
        if (!(in[0] == '\r' && in[1] == '\n')) {
            *p++ = *in;
        }
    }
    *p = 0;
    return out;
}

/* UTF-8 (LF) -> Windows clipboard as CF_UNICODETEXT (CRLF) */
static int set_windows_clipboard(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0), lf = 0;
    const char *c;
    HGLOBAL mem;
    wchar_t *w, *src;

    if (n <= 0) {
        return 0;
    }
    for (c = utf8; *c; c++) {
        lf += *c == '\n';
    }
    src = malloc(n * sizeof(wchar_t));
    mem = GlobalAlloc(GMEM_MOVEABLE, (n + lf) * sizeof(wchar_t));
    if (!src || !mem) {
        free(src);
        if (mem) {
            GlobalFree(mem);
        }
        return 0;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, src, n);
    w = GlobalLock(mem);
    for (int i = 0; i < n; i++) {
        if (src[i] == L'\n') {
            *w++ = L'\r';
        }
        *w++ = src[i];
    }
    GlobalUnlock(mem);
    free(src);

    /* another program may hold the clipboard for a moment */
    for (int tries = 0; tries < 20; tries++) {
        if (OpenClipboard(NULL)) {
            int ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem);
            CloseClipboard();
            if (!ok) {
                GlobalFree(mem);
            }
            return ok;
        }
        Sleep(25);
    }
    GlobalFree(mem);
    return 0;
}

/* a "CLIP <base64>" (or "STAT ...") line from the guest */
static void guest_line(const char *line)
{
    char *raw, *t;

    if (strncmp(line, "STAT ", 5) == 0) {
        gtv_perf_guest_stat(line + 5);      /* guest load for the HUD */
        return;
    }
    if (strncmp(line, "UX ", 3) == 0) {
        gtv_ux_line(line + 3);              /* user-activity log */
        return;
    }
    if (strncmp(line, "CLIP ", 5) != 0) {
        return;
    }
    raw = unbase64(line + 5);
    t = raw ? strip_cr(raw) : NULL;
    free(raw);
    if (!t || !*t) {
        free(t);
        return;
    }

    EnterCriticalSection(&lock);
    free(text);
    text = t;                   /* no text_gen++: do not send it back */
    LeaveCriticalSection(&lock);

    if (set_windows_clipboard(t)) {
        printf("[CLIP] guest copy -> host clipboard (%u bytes)\n",
               (unsigned)strlen(t));
    } else {
        printf("[CLIP] guest copy: cannot set host clipboard (%lu)\n",
               GetLastError());
    }
    fflush(stdout);
}

static int send_all(SOCKET s, const char *buf, int len)
{
    while (len > 0) {
        int n = send(s, buf, len, 0);
        if (n <= 0) {
            return 0;
        }
        buf += n;
        len -= n;
    }
    return 1;
}

/* send the current text; 0 when the connection is gone */
static int send_text(SOCKET s)
{
    char *b64 = NULL;
    int ok = 1;

    EnterCriticalSection(&lock);
    if (text) {
        b64 = base64(text, strlen(text));
    }
    LeaveCriticalSection(&lock);

    if (b64) {
        ok = send_all(s, "SET ", 4) && send_all(s, b64, (int)strlen(b64)) &&
             send_all(s, "\n", 1);
        free(b64);
    }
    return ok;
}

/* line being received from the guest */
struct guest_buf {
    char *data;                 /* LINE_MAX_B64 + 1 bytes, allocated lazily */
    size_t len;
    int skip;                   /* line too long: drop it up to '\n' */
};

/*
 * Read what the guest sent, waiting up to wait_ms.  Complete lines go to
 * guest_line().  Returns 0 when the connection is gone.
 */
static int read_guest(SOCKET s, struct guest_buf *b, int wait_ms)
{
    fd_set rd;
    struct timeval tv = {wait_ms / 1000, (wait_ms % 1000) * 1000};
    char chunk[16384];
    int n;

    FD_ZERO(&rd);
    FD_SET(s, &rd);
    if (select(0, &rd, NULL, NULL, &tv) != 1) {
        return 1;
    }
    n = recv(s, chunk, sizeof(chunk), 0);
    if (n <= 0) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (chunk[i] == '\n') {
            if (!b->skip && b->data) {
                b->data[b->len] = 0;
                if (b->len && b->data[b->len - 1] == '\r') {
                    b->data[b->len - 1] = 0;
                }
                guest_line(b->data);
            }
            b->len = 0;
            b->skip = 0;
            continue;
        }
        if (b->skip) {
            continue;
        }
        if (b->len >= LINE_MAX_B64) {
            printf("[CLIP] guest copy too large, dropped\n");
            b->skip = 1;
            continue;
        }
        if (!b->data) {
            b->data = malloc(LINE_MAX_B64 + 1);
            if (!b->data) {
                b->skip = 1;
                continue;
            }
        }
        b->data[b->len++] = chunk[i];
    }
    return 1;
}

/* first line must be "AUTH <token>" within 5 s */
static int authenticate(SOCKET s)
{
    char line[64];
    int len = 0;
    DWORD timeout = 5000;

    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    while (len < (int)sizeof(line) - 1) {
        int n = recv(s, line + len, 1, 0);
        if (n <= 0) {
            return 0;
        }
        if (line[len] == '\n') {
            break;
        }
        len++;
    }
    line[len] = 0;
    if (len && line[len - 1] == '\r') {
        line[len - 1] = 0;
    }
    timeout = 0;                /* later reads wait in select() only */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    return strncmp(line, "AUTH ", 5) == 0 && strcmp(line + 5, token) == 0;
}

static DWORD WINAPI clip_thread(LPVOID arg)
{
    for (;;) {
        SOCKET s = accept(listener, NULL, NULL);
        if (s == INVALID_SOCKET) {
            Sleep(1000);
            continue;
        }
        if (!authenticate(s)) {
            printf("[CLIP] guest helper: bad token, closed\n");
            closesocket(s);
            continue;
        }
        printf("[CLIP] guest helper connected\n");
        fflush(stdout);

        unsigned long sent = (unsigned long)-1;
        struct guest_buf in = {0};
        for (;;) {
            unsigned long gen;

            EnterCriticalSection(&lock);
            gen = text_gen;
            LeaveCriticalSection(&lock);

            if (gen != sent) {
                if (!send_text(s)) {
                    break;
                }
                sent = gen;
            }
            /* host changes are picked up within 200 ms */
            if (!read_guest(s, &in, 200)) {
                break;
            }
        }
        free(in.data);
        printf("[CLIP] guest helper disconnected\n");
        fflush(stdout);
        closesocket(s);
    }
    return 0;
}

int gtv_clip_start(void)
{
    static LONG started;
    struct sockaddr_in addr = {0};
    int len = sizeof(addr);
    WSADATA wsa;

    if (InterlockedExchange(&started, 1)) {
        return port;
    }

    InitializeCriticalSection(&lock);
    changed = CreateEvent(NULL, FALSE, FALSE, NULL);

    for (int i = 0; i < 16; i++) {
        unsigned int r = 0;
        rand_s(&r);
        snprintf(token + i * 2, 3, "%02x", r & 0xff);
    }

    WSAStartup(MAKEWORD(2, 2), &wsa);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return 0;
    }
    /* loopback only, port chosen by Windows */
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&addr, &len) != 0) {
        closesocket(listener);
        listener = INVALID_SOCKET;
        return 0;
    }
    port = ntohs(addr.sin_port);

    HANDLE t = CreateThread(NULL, 0, clip_thread, NULL, 0, NULL);
    if (!t) {
        return 0;
    }
    CloseHandle(t);
    printf("[CLIP] listening on 127.0.0.1:%d\n", port);
    return port;
}

const char *gtv_clip_token(void)
{
    return token;
}

void gtv_clip_host_text(const char *utf8)
{
    size_t len;
    char *t;

    if (!changed || !utf8 || !*utf8) {
        return;
    }
    len = strlen(utf8);
    if (len > CLIP_MAX) {
        printf("[CLIP] host text too large (%u bytes), not sent\n", (unsigned)len);
        return;
    }
    t = strip_cr(utf8);
    if (!t) {
        return;
    }

    EnterCriticalSection(&lock);
    if (!text || strcmp(text, t) != 0) {
        free(text);
        text = t;
        t = NULL;
        text_gen++;
        SetEvent(changed);
    }
    LeaveCriticalSection(&lock);
    free(t);
}

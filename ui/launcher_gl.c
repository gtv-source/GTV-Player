#include "stb_image.h"

#include "launcher_gl.h"
#include <epoxy/gl.h>
#include <stdio.h>
#include <SDL2/SDL.h>

#include <pthread.h>
#include <windows.h>
#include <winhttp.h>
#include <math.h>
#include "stb_truetype.h"
#include "adb.h"
#include "gtv-perf.h"

// ================= STATE =================
static float alpha = 1.0f;
static int ready = 0;

static float progress = 0.0f;
// static int frame_count = 0;
static int boot_done = 0;

// ===== IMAGE STATE =====
static unsigned char *img_data = NULL;
// static unsigned char *img_rgb = NULL;

static int img_w = 0, img_h = 0;

static GLuint bg_texture = 0;
static int image_ready = 0;
static int texture_ready = 0;

// ================= THREAD LOAD =================

static int http_download(const char *url, unsigned char **out_buf, size_t *out_size)
{
    *out_buf = NULL;
    *out_size = 0;

    // ===== INIT ALL HANDLE (FIX WARNING) =====
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;

    unsigned char *buffer = NULL;
    size_t total = 0;

    // ===== URL -> WCHAR =====
    wchar_t wurl[1024];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);

    URL_COMPONENTS parts = {0};
    wchar_t host[256] = {0};
    wchar_t path[1024] = {0};
    wchar_t extra[1024] = {0};

    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 1024;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 1024;

    if (!WinHttpCrackUrl(wurl, 0, 0, &parts))
    {
        fprintf(stderr, "[HTTP] CrackUrl FAILED %lu\n", GetLastError());
        goto fail;
    }

    // ===== FULL PATH (PATH + QUERY) =====
    wchar_t fullPath[2048];
    swprintf(fullPath, 2048, L"%s%s",
             parts.lpszUrlPath,
             parts.lpszExtraInfo ? parts.lpszExtraInfo : L"");

    // ===== SESSION =====
    session = WinHttpOpen(L"Mozilla/5.0",
                          WINHTTP_ACCESS_TYPE_NO_PROXY,
                          NULL, NULL, 0);
    if (!session)
    {
        fprintf(stderr, "[HTTP] Open FAILED %lu\n", GetLastError());
        goto fail;
    }

    // ===== TLS FIX =====
    DWORD protocols =
        WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 |
        WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;

    WinHttpSetOption(session,
                     WINHTTP_OPTION_SECURE_PROTOCOLS,
                     &protocols,
                     sizeof(protocols));

    // timeout
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

    // ===== CONNECT =====
    connect = WinHttpConnect(session,
                             parts.lpszHostName,
                             parts.nPort, 0);

    if (!connect)
    {
        fprintf(stderr, "[HTTP] Connect FAILED %lu\n", GetLastError());
        goto fail;
    }

    DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS)
                      ? WINHTTP_FLAG_SECURE
                      : 0;

    // ===== REQUEST =====
    request = WinHttpOpenRequest(connect,
                                 L"GET",
                                 fullPath,
                                 NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 flags);

    if (!request)
    {
        fprintf(stderr, "[HTTP] OpenRequest FAILED %lu\n", GetLastError());
        goto fail;
    }

    // ===== IGNORE SSL (DEBUG/CDN) =====
    DWORD secFlags =
        SECURITY_FLAG_IGNORE_UNKNOWN_CA |
        SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
        SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
        SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

    WinHttpSetOption(request,
                     WINHTTP_OPTION_SECURITY_FLAGS,
                     &secFlags,
                     sizeof(secFlags));

    // ===== SEND =====
    if (!WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 0))
    {
        fprintf(stderr, "[HTTP] Send FAILED %lu\n", GetLastError());
        goto fail;
    }

    if (!WinHttpReceiveResponse(request, NULL))
    {
        fprintf(stderr, "[HTTP] Receive FAILED %lu\n", GetLastError());
        goto fail;
    }

    // ===== READ LOOP =====
    while (1)
    {
        DWORD size = 0;

        if (!WinHttpQueryDataAvailable(request, &size))
        {
            fprintf(stderr, "[HTTP] QueryData FAILED %lu\n", GetLastError());
            goto fail;
        }

        if (size == 0)
            break;

        unsigned char *new_buf = realloc(buffer, total + size);
        if (!new_buf)
        {
            fprintf(stderr, "[HTTP] realloc FAILED\n");
            goto fail;
        }

        buffer = new_buf;

        DWORD read = 0;
        if (!WinHttpReadData(request, buffer + total, size, &read))
        {
            fprintf(stderr, "[HTTP] Read FAILED %lu\n", GetLastError());
            goto fail;
        }

        total += read;
    }

    if (total == 0)
    {
        fprintf(stderr, "[HTTP] EMPTY RESPONSE\n");
        goto fail;
    }

    *out_buf = buffer;
    *out_size = total;

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    return 1;

fail:
    if (buffer)
        free(buffer);

    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    if (session)
        WinHttpCloseHandle(session);

    return 0;
}

static void *image_loader_thread(void *arg)
{
    char *url = (char *)arg;

    fprintf(stderr, "[IMG] Source: %s\n", url);

    // ===== CASE 1: LOCAL FILE =====
    if (strncmp(url, "http", 4) != 0)
    {
        img_data = stbi_load(url, &img_w, &img_h, NULL, 4);

        if (img_data)
        {
            fprintf(stderr, "[IMG] Load LOCAL OK (%dx%d)\n", img_w, img_h);
            image_ready = 1;
        }
        else
        {
            fprintf(stderr, "[IMG] Load LOCAL FAILED\n");
        }

        free(url);
        return NULL;
    }

    // ===== CASE 2: HTTP =====
    unsigned char *buf = NULL;
    size_t size = 0;

    if (http_download(url, &buf, &size))
    {
        int n;
        img_data = stbi_load_from_memory(buf, size,
                                         &img_w, &img_h, &n, 4);

        if (img_data)
        {
            fprintf(stderr, "[IMG] Decode OK (%dx%d)\n", img_w, img_h);
            image_ready = 1;
        }
        else
        {
            fprintf(stderr, "[IMG] Decode FAILED\n");
        }

        free(buf);
    }
    else
    {
        fprintf(stderr, "[HTTP] DOWNLOAD FAILED\n");
    }

    free(url);
    return NULL;
}

// ================= START DOWNLOAD =================

static void start_download(void)
{
    const char *env = getenv("LINK_ADS");

    if (!env || strlen(env) == 0)
    {
        fprintf(stderr, "[IMG] LINK_ADS empty\n");
        return;
    }

    pthread_t t;
    char *url = strdup(env);

    pthread_create(&t, NULL, image_loader_thread, url);
    pthread_detach(t);
}

// ================= TEXTURE UPLOAD =================
// The window's GL context keeps the unpack state of whoever uploaded last:
// ui/console-gl.c sets GL_UNPACK_ROW_LENGTH to the guest framebuffer stride
// and leaves it. glTexImage2D then reads far past our small buffer and the
// NVIDIA driver crashes later in its present thread (0xC0000005). Upload with
// default unpack state (no PBO, no row length/skips) and restore it after.
static void upload_texture(GLint ifmt, int w, int h, GLenum fmt, const void *pixels)
{
    GLint pbo = 0;

    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);

    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);

    glPopClientAttrib();
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
}

// ================= TEXT (stb_truetype, Windows system font) =================
// Segoe UI ships with every Windows since Vista and covers Vietnamese; the font
// is read from the system at run time, never redistributed.
static stbtt_fontinfo font;
static unsigned char *font_data = NULL;
static int font_ok = 0;

static void font_load(void)
{
    static const wchar_t *names[] = { L"segoeuib.ttf", L"segoeui.ttf", L"arial.ttf" };
    wchar_t dir[MAX_PATH], path[MAX_PATH];

    if (font_ok || !GetWindowsDirectoryW(dir, MAX_PATH))
        return;

    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])) && !font_ok; i++)
    {
        swprintf(path, MAX_PATH, L"%ls\\Fonts\\%ls", dir, names[i]);
        FILE *f = _wfopen(path, L"rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        font_data = malloc(size);
        if (font_data && fread(font_data, 1, size, f) == (size_t)size &&
            stbtt_InitFont(&font, font_data, stbtt_GetFontOffsetForIndex(font_data, 0)))
        {
            font_ok = 1;
            fprintf(stderr, "[LAUNCHER] font %ls\n", names[i]);
        }
        else
        {
            free(font_data);
            font_data = NULL;
        }
        fclose(f);
    }
}

static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++, n = 0;

    if (c >= 0xF0) { c &= 0x07; n = 3; }
    else if (c >= 0xE0) { c &= 0x0F; n = 2; }
    else if (c >= 0xC0) { c &= 0x1F; n = 1; }
    while (n-- && (*p & 0xC0) == 0x80)
        c = (c << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

// A string rasterised once into an alpha texture; redone only when it changes.
typedef struct
{
    GLuint tex;
    int w, h, ascent;
    float px;
    char str[GTV_HUD_LEN];   /* also the HUD lines (ui/gtv-perf.c) */
} TextTex;

// width in pixels of str at px (font loaded)
static float text_width(const char *str, float px)
{
    float scale = stbtt_ScaleForPixelHeight(&font, px);
    float x = 0;
    int prev = 0;
    for (const char *s = str; *s;)
    {
        int cp = utf8_next(&s);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
        if (prev)
            x += scale * stbtt_GetCodepointKernAdvance(&font, prev, cp);
        x += adv * scale;
        prev = cp;
    }
    return x + 2;
}

static void text_update(TextTex *t, const char *str, float px)
{
    if (!font_ok || (t->tex && t->px == px && strcmp(t->str, str) == 0))
        return;

    float scale = stbtt_ScaleForPixelHeight(&font, px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
    int ascent = (int)ceilf(asc * scale);
    int h = ascent + (int)ceilf(-desc * scale) + 2;

    // width
    float x = 0;
    int prev = 0;
    for (const char *s = str; *s;)
    {
        int cp = utf8_next(&s);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
        if (prev)
            x += scale * stbtt_GetCodepointKernAdvance(&font, prev, cp);
        x += adv * scale;
        prev = cp;
    }
    int w = (int)ceilf(x) + 2;

    unsigned char *bmp = calloc(w * h, 1);
    if (!bmp)
        return;
    x = 1;
    prev = 0;
    for (const char *s = str; *s;)
    {
        int cp = utf8_next(&s);
        int adv, lsb, x0, y0, x1, y1;
        if (prev)
            x += scale * stbtt_GetCodepointKernAdvance(&font, prev, cp);
        stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBoxSubpixel(&font, cp, scale, scale, x - floorf(x), 0,
                                            &x0, &y0, &x1, &y1);
        int gx = (int)floorf(x) + x0, gy = ascent + y0;
        if (gx >= 0 && gy >= 0 && gx + (x1 - x0) <= w && gy + (y1 - y0) <= h)
        {
            stbtt_MakeCodepointBitmapSubpixel(&font, bmp + gy * w + gx, x1 - x0, y1 - y0, w,
                                              scale, scale, x - floorf(x), 0, cp);
        }
        x += adv * scale;
        prev = cp;
    }

    if (!t->tex)
        glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    upload_texture(GL_ALPHA, w, h, GL_ALPHA, bmp);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    free(bmp);

    t->w = w;
    t->h = h;
    t->ascent = ascent;
    t->px = px;
    snprintf(t->str, sizeof(t->str), "%s", str);
}

// baseline at y; a soft shadow keeps the text readable on busy ad images
static void text_draw(const TextTex *t, float x, float y, float r, float g, float b, float a)
{
    if (!t->tex)
        return;
    float top = y - t->ascent;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    for (int pass = 0; pass < 2; pass++)
    {
        float o = pass ? 0.0f : 1.5f;
        if (pass)
            glColor4f(r, g, b, a);
        else
            glColor4f(0, 0, 0, a * 0.55f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x + o, top + o);
        glTexCoord2f(1, 0); glVertex2f(x + o + t->w, top + o);
        glTexCoord2f(1, 1); glVertex2f(x + o + t->w, top + o + t->h);
        glTexCoord2f(0, 1); glVertex2f(x + o, top + o + t->h);
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);
}

// ================= SHAPES =================
// Pill (fully rounded bar) as one triangle strip, so a horizontal colour
// gradient and a moving highlight can be given per vertex.
static void draw_pill(float x, float y, float w, float h,
                      const float c0[3], const float c1[3], float a,
                      float shine_x, float shine_w)
{
    if (w <= 0.5f)
        return;
    float r = fminf(h, w) * 0.5f;
    float cy = y + h * 0.5f;
    // samples: dense by angle in the two round caps, even in between
    const int cap = 10, mid = 48, n = 2 * cap + mid;

    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= n; i++)
    {
        float xs;
        if (i <= cap)
            xs = x + r - r * cosf(1.5707963f * i / cap);
        else if (i < cap + mid)
            xs = x + r + (w - 2 * r) * (i - cap) / mid;
        else
            xs = x + w - r + r * sinf(1.5707963f * (i - cap - mid) / cap);
        float dx = 0;
        if (xs < x + r)
            dx = x + r - xs;
        else if (xs > x + w - r)
            dx = xs - (x + w - r);
        float dy = sqrtf(fmaxf(r * r - dx * dx, 0)) + (h * 0.5f - r);

        float k = (xs - x) / w;
        float s = 0;
        if (shine_w > 0)
            s = fmaxf(0, 1 - fabsf(xs - shine_x) / shine_w) * 0.45f;
        float cr = c0[0] + (c1[0] - c0[0]) * k, cg = c0[1] + (c1[1] - c0[1]) * k,
              cb = c0[2] + (c1[2] - c0[2]) * k;
        glColor4f(cr + (1 - cr) * s, cg + (1 - cg) * s, cb + (1 - cb) * s, a);
        glVertex2f(xs, cy - dy);
        glVertex2f(xs, cy + dy);
    }
    glEnd();
}

static void draw_dot(float cx, float cy, float r, float a)
{
    glColor4f(1, 1, 1, a);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= 20; i++)
    {
        float t = i * 6.2831853f / 20;
        glVertex2f(cx + cosf(t) * r, cy + sinf(t) * r);
    }
    glEnd();
}

// ================= INIT =================
static volatile float stage_floor = 0.0f;   // set by the adb thread
static TextTex label_tex, pct_tex;

// ================= NOTICE (GPLAYER_NOTICE, e.g. no hardware acceleration) =================
// Shown on the loading screen and for NOTICE_GAME_MS once the game is on
// screen. The launcher sets it (core/gPlayer.js); UTF-8 read from UTF-16,
// getenv() would turn Vietnamese letters into '?'.
#define NOTICE_LINES 4
#define NOTICE_GAME_MS 12000
static char *notice;
static Uint32 hidden_at;    // when the loading screen was gone

static const char *notice_text(void)
{
    static int done;
    if (!done)
    {
        done = 1;
        wchar_t buf[1024];
        DWORD n = GetEnvironmentVariableW(L"GPLAYER_NOTICE", buf, 1024);
        if (n > 0 && n < 1024)
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, NULL, 0, NULL, NULL);
            notice = calloc(len + 1, 1);
            if (notice)
                WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, notice, len, NULL, NULL);
        }
    }
    return notice && notice[0] ? notice : NULL;
}

/*
 * The notice, word-wrapped to max_w, in a dark panel whose top is at y,
 * centred. Line textures are made again only when the text or size changes.
 */
static void draw_notice(float w, float y, float px, float a)
{
    static TextTex tex[NOTICE_LINES];
    const char *txt = notice_text();
    char lines[NOTICE_LINES][GTV_HUD_LEN];
    int n = 0;

    font_load();
    if (!txt || !font_ok || a <= 0)
        return;

    // wrap on spaces (and on '\n' from the launcher)
    float max_w = w * 0.88f;
    const char *s = txt;
    while (*s && n < NOTICE_LINES)
    {
        char cur[GTV_HUD_LEN] = "";
        while (*s == ' ')
            s++;
        for (;;)
        {
            const char *e = s;
            while (*e && *e != ' ' && *e != '\n')
                e++;
            char next[GTV_HUD_LEN];
            snprintf(next, sizeof(next), "%s%s%.*s", cur, cur[0] ? " " : "", (int)(e - s), s);
            if (cur[0] && text_width(next, px) > max_w)
                break;      // the word starts the next line
            snprintf(cur, sizeof(cur), "%s", next);
            s = e;
            if (*s == '\n')
            {
                s++;
                break;
            }
            if (!*s)
                break;
            s++;
        }
        snprintf(lines[n++], GTV_HUD_LEN, "%s", cur);
    }

    float line_h = roundf(px * 1.3f), box_w = 0;
    for (int i = 0; i < n; i++)
    {
        text_update(&tex[i], lines[i], px);
        if (tex[i].w > box_w)
            box_w = tex[i].w;
    }
    float pad = roundf(px * 0.6f);
    float x0 = roundf((w - box_w) * 0.5f);

    glColor4f(0.25f, 0.05f, 0.02f, 0.78f * a);
    glBegin(GL_QUADS);
    glVertex2f(x0 - pad, y);
    glVertex2f(x0 + box_w + pad, y);
    glVertex2f(x0 + box_w + pad, y + n * line_h + 2 * pad);
    glVertex2f(x0 - pad, y + n * line_h + 2 * pad);
    glEnd();
    for (int i = 0; i < n; i++)
    {
        text_draw(&tex[i], roundf((w - tex[i].w) * 0.5f), y + pad + i * line_h + tex[i].ascent,
                  1.0f, 0.85f, 0.30f, a);
    }
}

static float notice_px(int w, int h)
{
    return w >= h ? fminf(fmaxf(h * 0.030f, 16.0f), 30.0f)
                  : fminf(fmaxf(w * 0.036f, 14.0f), 24.0f);
}

/*
 * 1 while the banner is on screen, and a little after so the frame without
 * it is drawn: the guest may send no frames (black loading screen), then the
 * window must be redrawn for the banner to fade and go.
 */
int launcher_gl_notice_pending(void)
{
    return notice_text() && hidden_at &&
           SDL_GetTicks() - hidden_at < NOTICE_GAME_MS + 1500;
}

/* over the game, after the loading screen, for NOTICE_GAME_MS */
void launcher_gl_draw_notice(SDL_Window *window)
{
    int w, h;

    if (!notice_text() || !hidden_at)
        return;
    Uint32 since = SDL_GetTicks() - hidden_at;
    if (since > NOTICE_GAME_MS)
        return;
    float a = since > NOTICE_GAME_MS - 1000 ? (NOTICE_GAME_MS - since) / 1000.0f : 1.0f;

    SDL_GL_GetDrawableSize(window, &w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    draw_notice(w, roundf(h * 0.04f), notice_px(w, h), a);
}

void launcher_gl_init(int w, int h)
{
    fprintf(stderr, "[LAUNCHER] init: w=%d h=%d\n", w, h);

    alpha = 1.0f;
    ready = 0;

    progress = 0.0f;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    font_load();

    // start async load
    start_download();

    /* the adb thread waits for sys.boot_completed itself, no fixed delay */
    adb_start();
}

// Boot milestones reported by ui/adb.c; the bar never goes back.
void launcher_gl_notify_progress(float p)
{
    if (p > stage_floor)
        stage_floor = p;
}

// ================= UPDATE =================
void launcher_gl_update(SDL_Window *window)
{
    if (!ready || (boot_done && alpha <= 0.0f))
        return;

    // ===== TIME =====
    static Uint32 last = 0;
    Uint32 now = SDL_GetTicks();

    if (last == 0)
        last = now;

    float dt = (now - last) / 1000.0f;
    last = now;

    // Frames come rarely while Android boots or the game starts, so time is
    // wall-clock; dt is only capped so the easing below stays stable.
    if (dt > 0.5f)
        dt = 0.5f;

    static Uint32 start = 0;
    if (start == 0)
        start = now;
    float elapsed = (now - start) / 1000.0f;

    // ===== PROGRESS =====
    // Time estimate that slows down instead of stopping (~62% when adb comes
    // up at ~19 s with WHPX/AEHD), raised by the real boot milestones.
    float target = 0.70f * (1.0f - expf(-elapsed / 9.0f));
    if (stage_floor > target)
        target = stage_floor;
    if (boot_done)
        target = 1.0f;
    progress += (target - progress) * fminf(1.0f, dt * (boot_done ? 6.0f : 2.5f));
    if (progress > 1.0f)
        progress = 1.0f;

    // ===== FADE OUT (once the bar is full) =====
    if (boot_done && progress > 0.995f)
    {
        alpha -= dt * 2.0f;
        if (alpha <= 0.0f)
        {
            alpha = 0.0f;
            hidden_at = now ? now : 1;
            fprintf(stderr, "[LAUNCHER] HIDDEN\n");
        }
    }

    launcher_gl_render_background(window);
    launcher_gl_render_progress(window);
}

// ================= RENDER =================

void launcher_gl_render_progress(SDL_Window *window)
{
    int w, h;
    SDL_GL_GetDrawableSize(window, &w, &h);

    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    float t = SDL_GetTicks() / 1000.0f;

    // ===== LAYOUT =====
    float bar_w = roundf(w * 2.0f / 3.0f);
    float bar_h = fmaxf(8.0f, roundf(h * 0.011f));
    float bar_x = roundf((w - bar_w) * 0.5f);
    float bar_y = roundf(h * 0.90f);
    /* landscape: by height; portrait: the bar is only 2/3 of a narrow
     * window, so size by width or the label crowds the percentage */
    float px = w >= h ? fminf(fmaxf(h * 0.038f, 22.0f), 42.0f)
                      : fminf(fmaxf(w * 0.046f, 16.0f), 30.0f);
    float base = bar_y - roundf(px * 0.6f);

    // ===== SHADE behind the text and the bar =====
    float shade_top = base - px * 3.0f;
    glBegin(GL_QUADS);
    glColor4f(0, 0, 0, 0);
    glVertex2f(0, shade_top);
    glVertex2f(w, shade_top);
    glColor4f(0, 0, 0, 0.65f * alpha);
    glVertex2f(w, h);
    glVertex2f(0, h);
    glEnd();

    // ===== TEXT =====
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", (int)(progress * 100.0f + 0.5f));
    text_update(&label_tex, "Đang vào game", px);
    text_update(&pct_tex, pct, px * 0.9f);

    text_draw(&label_tex, bar_x - 1, base, 1, 1, 1, alpha);
    if (label_tex.tex)
    {
        // three dots pulsing one after another
        float dr = fmaxf(2.0f, px * 0.11f);
        float dx = bar_x + label_tex.w + dr * 2.5f;
        for (int i = 0; i < 3; i++)
        {
            float k = 0.5f + 0.5f * sinf(t * 5.0f - i * 0.9f);
            draw_dot(dx + i * dr * 3.4f, base - dr - 1, dr, alpha * (0.25f + 0.75f * k));
        }
    }
    text_draw(&pct_tex, bar_x + bar_w - pct_tex.w + 1, base, 1.0f, 0.75f, 0.75f, alpha);

    // ===== NOTICE (e.g. no hardware acceleration), top of the screen =====
    draw_notice(w, roundf(h * 0.05f), notice_px(w, h), alpha);

    // ===== BAR =====
    static const float track[3] = { 1, 1, 1 };
    static const float fill0[3] = { 0.80f, 0.04f, 0.06f };   // deep red
    static const float fill1[3] = { 1.00f, 0.22f, 0.20f };   // bright red
    static const float glow[3] = { 1.00f, 0.10f, 0.10f };

    draw_pill(bar_x, bar_y, bar_w, bar_h, track, track, 0.16f * alpha, 0, 0);

    float fill_w = bar_w * progress;
    if (fill_w > 0.5f)
    {
        float g = bar_h * 0.6f;
        draw_pill(bar_x - g, bar_y - g, fill_w + 2 * g, bar_h + 2 * g, glow, glow,
                  0.18f * alpha, 0, 0);
        // highlight sweeping across the filled part every 1.6 s
        float sweep = fmodf(t, 1.6f) / 1.6f;
        float shine_x = bar_x - bar_w * 0.15f + sweep * (fill_w + bar_w * 0.3f);
        draw_pill(bar_x, bar_y, fmaxf(fill_w, bar_h), bar_h, fill0, fill1, alpha,
                  shine_x, bar_w * 0.12f);
    }
}

void launcher_gl_render_background(SDL_Window *window)
{
    int w, h;
    SDL_GL_GetDrawableSize(window, &w, &h);

    // ===== RESET STATE =====
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, w, h);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1, 1, 1, -1, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // ===== UPLOAD TEXTURE =====
    if (image_ready && !texture_ready)
    {
        glGenTextures(1, &bg_texture);
        glBindTexture(GL_TEXTURE_2D, bg_texture);

        upload_texture(GL_RGBA, img_w, img_h, GL_RGBA, img_data);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        free(img_data);
        img_data = NULL;

        texture_ready = 1;

        fprintf(stderr, "[LAUNCHER] texture RGB uploaded\n");
    }

    // ===== DRAW =====
    // Quads, not glClear: while fading out the game frame must show through.
    if (texture_ready)
    {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, bg_texture);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

        glColor4f(1, 1, 1, alpha);

        glBegin(GL_QUADS);
        glTexCoord2f(0, 0);
        glVertex2f(-1, -1);
        glTexCoord2f(1, 0);
        glVertex2f(1, -1);
        glTexCoord2f(1, 1);
        glVertex2f(1, 1);
        glTexCoord2f(0, 1);
        glVertex2f(-1, 1);
        glEnd();

        glDisable(GL_TEXTURE_2D);
    }
    else
    {
        // no ad image: dark blue-grey gradient
        glBegin(GL_QUADS);
        glColor4f(0.10f, 0.11f, 0.15f, alpha);
        glVertex2f(-1, -1);
        glVertex2f(1, -1);
        glColor4f(0.03f, 0.03f, 0.05f, alpha);
        glVertex2f(1, 1);
        glVertex2f(-1, 1);
        glEnd();
    }
}

// ================= TRIGGER =================

void launcher_gl_notify_framebuffer(void)
{
    ready = 1;
    fprintf(stderr, "[LAUNCHER] framebuffer detected\n");
}

void launcher_gl_notify_boot_complete(void)
{
    boot_done = 1;
    fprintf(stderr, "[LAUNCHER] BOOT COMPLETE\n");
}

// ================= PERFORMANCE HUD (Ctrl+Shift+F, ui/gtv-perf.c) =================
// Drawn over the game frame just before the swap; the text is rasterised
// again only when it changes (once a second).
void launcher_gl_draw_hud(SDL_Window *window)
{
    static TextTex lines_tex[GTV_HUD_LINES];
    static const float col[3][3] = {
        { 1.00f, 1.00f, 1.00f },    // normal
        { 1.00f, 0.82f, 0.25f },    // warning
        { 1.00f, 0.35f, 0.30f },    // bad
    };
    char lines[GTV_HUD_LINES][GTV_HUD_LEN];
    int colors[GTV_HUD_LINES];
    int n = gtv_perf_hud_text(lines, colors);
    int w, h;

    font_load();
    if (!n || !font_ok)
        return;

    SDL_GL_GetDrawableSize(window, &w, &h);
    float px = fminf(fmaxf((w < h ? w : h) * 0.026f, 13.0f), 20.0f);
    float line_h = roundf(px * 1.3f);
    float x = roundf(px * 0.6f), y = roundf(px * 0.6f);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    float box_w = 0;
    for (int i = 0; i < n; i++)
    {
        text_update(&lines_tex[i], lines[i], px);
        if (lines_tex[i].w > box_w)
            box_w = lines_tex[i].w;
    }

    // translucent panel
    float pad = roundf(px * 0.5f);
    glColor4f(0, 0, 0, 0.55f);
    glBegin(GL_QUADS);
    glVertex2f(x - pad, y - pad);
    glVertex2f(x + box_w + pad, y - pad);
    glVertex2f(x + box_w + pad, y + n * line_h + pad);
    glVertex2f(x - pad, y + n * line_h + pad);
    glEnd();

    for (int i = 0; i < n; i++)
    {
        const float *c = col[colors[i] < 0 || colors[i] > 2 ? 0 : colors[i]];
        text_draw(&lines_tex[i], x, y + i * line_h + lines_tex[i].ascent, c[0], c[1], c[2], 1.0f);
    }
}

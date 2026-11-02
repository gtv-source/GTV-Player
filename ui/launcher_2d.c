#include "stb_image.h"

#include <SDL2/SDL.h>
#include <pthread.h>
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include "launcher_2d.h"
#include "adb.h"

// ================= STATE =================
static float alpha = 1.0f;
static int ready = 0;
static float progress = 0.0f;
static int boot_done = 0;

// ===== IMAGE =====
static unsigned char *img_data = NULL;
static int img_w = 0, img_h = 0;

static SDL_Texture *bg_texture = NULL;
static int image_ready = 0;

// ================= THREAD LOAD =================
static void *image_loader_thread(void *arg)
{
    char *url = (char *)arg;

    // wchar_t wurl[512];
    // MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 512);

    // URL_COMPONENTS parts = {0};
    // wchar_t host[256];
    // wchar_t path[512];

    // parts.dwStructSize = sizeof(parts);
    // parts.lpszHostName = host;
    // parts.dwHostNameLength = 256;
    // parts.lpszUrlPath = path;
    // parts.dwUrlPathLength = 512;

    // WinHttpCrackUrl(wurl, 0, 0, &parts);

    // HINTERNET session = WinHttpOpen(L"Launcher/1.0",
    //                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
    //                                 NULL, NULL, 0);

    // if (!session)
    //     return NULL;

    // HINTERNET connect = WinHttpConnect(session,
    //                                    parts.lpszHostName,
    //                                    parts.nPort, 0);

    // if (!connect)
    //     return NULL;

    // DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS)
    //                   ? WINHTTP_FLAG_SECURE
    //                   : 0;

    // HINTERNET request = WinHttpOpenRequest(connect,
    //                                        L"GET",
    //                                        parts.lpszUrlPath,
    //                                        NULL,
    //                                        WINHTTP_NO_REFERER,
    //                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
    //                                        flags);

    // if (!request)
    //     return NULL;

    // WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 0);
    // WinHttpReceiveResponse(request, NULL);

    // unsigned char *buffer = NULL;
    // size_t total = 0;
    // DWORD size = 0;

    // do
    // {
    //     WinHttpQueryDataAvailable(request, &size);
    //     if (size == 0)
    //         break;

    //     buffer = realloc(buffer, total + size);

    //     DWORD downloaded = 0;
    //     WinHttpReadData(request, buffer + total, size, &downloaded);
    //     total += downloaded;

    // } while (size > 0);

    // if (buffer)
    // {
    //     int n;
    //     img_data = stbi_load_from_memory(buffer, total,
    //                                      &img_w, &img_h, &n, 4);
    //     image_ready = 1;
    //     free(buffer);
    // }

    // WinHttpCloseHandle(request);
    // WinHttpCloseHandle(connect);
    // WinHttpCloseHandle(session);

    free(url);
    return NULL;
}

static void start_download(void)
{
    pthread_t t;
    char *url = strdup(getenv("LINK_ADS"));
    pthread_create(&t, NULL, image_loader_thread, url);
    pthread_detach(t);
}

// ================= INIT =================
void launcher_2d_init(void)
{
    alpha = 1.0f;
    ready = 0;
    progress = 0.0f;
    boot_done = 0;

    start_download();
    adb_start();
}

// ================= UPDATE =================
void launcher_2d_update(SDL_Renderer *renderer, SDL_Window *window)
{
    if (!ready)
        return;

    // ===== TIME =====
    static Uint32 last = 0;
    Uint32 now = SDL_GetTicks();

    if (last == 0)
    {
        last = now;
        return;
    }

    float dt = (now - last) / 1000.0f;
    last = now;

    if (dt > 0.1f)
        dt = 0.1f;

    // ===== PROGRESS =====
    static float elapsed = 0.0f;
    elapsed += dt;

    if (!boot_done)
    {
        progress = elapsed / 45.0f;
        if (progress > 0.99f)
            progress = 0.99f;
    }
    else
    {
        progress += (1.0f - progress) * dt * 3.0f;
        alpha -= dt * 1.5f;
    }

    if (progress > 1.0f)
        progress = 1.0f;
    if (alpha < 0.0f)
        alpha = 0.0f;

    if (alpha <= 0.0f)
        return;

    // ===== SIZE (FIX CHÍNH) =====
    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);

    // ===== CLEAR =====
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    // ===== CREATE TEXTURE =====
    if (image_ready && !bg_texture && img_data)
    {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

        bg_texture = SDL_CreateTexture(renderer,
                                       SDL_PIXELFORMAT_RGBA8888,
                                       SDL_TEXTUREACCESS_STATIC,
                                       img_w, img_h);

        SDL_UpdateTexture(bg_texture, NULL, img_data, img_w * 4);
        SDL_SetTextureBlendMode(bg_texture, SDL_BLENDMODE_BLEND);

        free(img_data);
        img_data = NULL;
    }

    // ===== DRAW BACKGROUND (COVER FULL) =====
    if (bg_texture)
    {
        float scale_w = (float)w / img_w;
        float scale_h = (float)h / img_h;
        float scale = fmaxf(scale_w, scale_h);

        int draw_w = (int)ceilf(img_w * scale);
        int draw_h = (int)ceilf(img_h * scale);

        int x = (w - draw_w) / 2;
        int y = (h - draw_h) / 2;

        // clamp chống viền
        if (x > 0) x = 0;
        if (y > 0) y = 0;

        SDL_Rect dst = { x, y, draw_w, draw_h };

        // ===== DEBUG (1 LẦN) =====
        static int log_once = 0;
        if (!log_once)
        {
            int win_w, win_h;
            SDL_GetWindowSize(window, &win_w, &win_h);

            fprintf(stderr, "\n===== BG DEBUG =====\n");
            fprintf(stderr, "Window size      : %d x %d\n", win_w, win_h);
            fprintf(stderr, "Renderer size    : %d x %d\n", w, h);
            fprintf(stderr, "Image size       : %d x %d\n", img_w, img_h);
            fprintf(stderr, "Scale (w/h/max)  : %.4f / %.4f / %.4f\n",
                    scale_w, scale_h, scale);
            fprintf(stderr, "Draw size        : %d x %d\n", draw_w, draw_h);
            fprintf(stderr, "Draw pos         : x=%d y=%d\n", x, y);
            fprintf(stderr, "Alpha            : %.2f\n", alpha);
            fprintf(stderr, "====================\n\n");

            log_once = 1;
        }

        SDL_SetTextureAlphaMod(bg_texture, (Uint8)(alpha * 255));
        SDL_RenderCopy(renderer, bg_texture, NULL, &dst);
    }

    // ===== PROGRESS BAR =====
    int bar_w_full = (int)(w * 0.8f);
    int bar_x = (w - bar_w_full) / 2;
    int bar_h = 10;
    int bar_y = h - (int)(h * 0.1f);

    SDL_Rect bg = {bar_x, bar_y, bar_w_full, bar_h};

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 120);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Rect fill = {
        bar_x,
        bar_y,
        (int)(bar_w_full * progress),
        bar_h};

    SDL_SetRenderDrawColor(renderer, 255, 50, 50, (Uint8)(alpha * 255));
    SDL_RenderFillRect(renderer, &fill);

    // ===== FADE =====
    if (boot_done)
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0,
                               (Uint8)((1.0f - alpha) * 255));
        SDL_RenderFillRect(renderer, NULL);
    }
}

// ================= TRIGGER =================
void launcher_2d_notify_framebuffer(void)
{
    ready = 1;
}

void launcher_2d_notify_boot_complete(void)
{
    boot_done = 1;
}
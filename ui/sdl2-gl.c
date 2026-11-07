/*
 * QEMU SDL display driver -- opengl support
 *
 * Copyright (c) 2014 Red Hat
 *
 * Authors:
 *     Gerd Hoffmann <kraxel@redhat.com>
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
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"
#include "launcher_gl.h"
#include "gtv-perf.h"

static int frame_count = 0;

static GLuint cursor_tex = 0;
static int cursor_w = 0;
static int cursor_h = 0;
int mouse_x = 0;
int mouse_y = 0;

static void draw_cursor_gl(int win_w, int win_h)
{
    if (!cursor_tex)
        return;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, cursor_tex);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // setup 2D
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, win_w, win_h, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    int x = mouse_x;
    int y = mouse_y;

    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex2f(x, y);
    glTexCoord2f(1, 0);
    glVertex2f(x + cursor_w, y);
    glTexCoord2f(1, 1);
    glVertex2f(x + cursor_w, y + cursor_h);
    glTexCoord2f(0, 1);
    glVertex2f(x, y + cursor_h);
    glEnd();

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    glDisable(GL_BLEND);
}

static void sdl2_set_scanout_mode(struct sdl2_console *scon, bool scanout)
{
    if (scon->scanout_mode == scanout)
    {
        return;
    }

    scon->scanout_mode = scanout;
    if (!scon->scanout_mode)
    {
        egl_fb_destroy(&scon->guest_fb);
        if (scon->surface)
        {
            surface_gl_destroy_texture(scon->gls, scon->surface);
            surface_gl_create_texture(scon->gls, scon->surface);
        }
    }
}

static void sdl2_gl_render_surface(struct sdl2_console *scon)
{
    int ww, wh;

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    sdl2_set_scanout_mode(scon, false);

    SDL_GetWindowSize(scon->real_window, &ww, &wh);
    surface_gl_setup_viewport(scon->gls, scon->surface, ww, wh);

    surface_gl_render_texture(scon->gls, scon->surface);

    SDL_GetWindowSize(scon->real_window, &ww, &wh);

    draw_cursor_gl(ww, wh);

    // ================= ADD LAUNCHER =================

    frame_count++;

    if (frame_count == 1)
    {
        int w, h;
        SDL_GetWindowSize(scon->real_window, &w, &h);

        launcher_gl_init(w, h);
        launcher_gl_notify_framebuffer();
    }

    // RESET STATE TRƯỚC KHI VẼ
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    // quan trọng nếu dùng immediate mode
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    launcher_gl_update(scon->real_window);
    // =================================================

    SDL_GL_SwapWindow(scon->real_window);
}

void sdl2_gl_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);

    if (!scon->real_window)
    {
        return;
    }

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    surface_gl_update_texture(scon->gls, scon->surface, x, y, w, h);
    scon->updates++;
}

void sdl2_gl_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    DisplaySurface *old_surface = scon->surface;

    assert(scon->opengl);

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    surface_gl_destroy_texture(scon->gls, scon->surface);

    scon->surface = new_surface;

    if (surface_is_placeholder(new_surface) && qemu_console_get_index(dcl->con))
    {
        qemu_gl_fini_shader(scon->gls);
        scon->gls = NULL;
        sdl2_window_destroy(scon);
        return;
    }

    if (!scon->real_window)
    {
        sdl2_window_create(scon);
        scon->gls = qemu_gl_init_shader();
    }
    else if (old_surface &&
             ((surface_width(old_surface) != surface_width(new_surface)) ||
              (surface_height(old_surface) != surface_height(new_surface))))
    {
        sdl2_window_resize(scon);
    }

    surface_gl_create_texture(scon->gls, scon->surface);
}

void sdl2_gl_refresh(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);

    graphic_hw_update(dcl->con);
    if (scon->updates && scon->real_window)
    {
        scon->updates = 0;
        sdl2_gl_render_surface(scon);
    }

    /*
     * HUD on while the guest sends no frames (game frozen, still menu): draw
     * the last frame again twice a second so the numbers keep moving.
     */
    /* same for the no-acceleration banner, so it fades out on time */
    int notice = launcher_gl_notice_pending();
    if ((gtv_perf_hud_visible() || notice) && scon->scanout_mode && scon->real_window &&
        gtv_perf_ms_since_frame() > (notice ? 100 : 500))
    {
        static gint64 last_redraw;
        gint64 now = g_get_monotonic_time();
        if (now - last_redraw > (notice ? 100000 : 500000))
        {
            last_redraw = now;
            sdl2_gl_scanout_flush(dcl, 0, 0, 0, 0);
        }
    }
    sdl2_poll_events(scon);
}

void sdl2_gl_redraw(struct sdl2_console *scon)
{
    assert(scon->opengl);

    if (scon->scanout_mode)
    {
        /* sdl2_gl_scanout_flush actually only care about
         * the first argument. */
        return sdl2_gl_scanout_flush(&scon->dcl, 0, 0, 0, 0);
    }
    if (scon->surface)
    {
        sdl2_gl_render_surface(scon);
    }
}

QEMUGLContext sdl2_gl_create_context(DisplayGLCtx *dgc,
                                     QEMUGLParams *params)
{
    struct sdl2_console *scon = container_of(dgc, struct sdl2_console, dgc);
    SDL_GLContext ctx;

    assert(scon->opengl);

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    if (scon->opts->gl == DISPLAY_GL_MODE_ON ||
        scon->opts->gl == DISPLAY_GL_MODE_CORE)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
    }
    else if (scon->opts->gl == DISPLAY_GL_MODE_ES)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_ES);
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, params->major_ver);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, params->minor_ver);

    ctx = SDL_GL_CreateContext(scon->real_window);

    /* If SDL fail to create a GL context and we use the "on" flag,
     * then try to fallback to GLES.
     */
    if (!ctx && scon->opts->gl == DISPLAY_GL_MODE_ON)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_ES);
        ctx = SDL_GL_CreateContext(scon->real_window);
    }
    return (QEMUGLContext)ctx;
}

void sdl2_gl_destroy_context(DisplayGLCtx *dgc, QEMUGLContext ctx)
{
    SDL_GLContext sdlctx = (SDL_GLContext)ctx;

    SDL_GL_DeleteContext(sdlctx);
}

int sdl2_gl_make_context_current(DisplayGLCtx *dgc,
                                 QEMUGLContext ctx)
{
    struct sdl2_console *scon = container_of(dgc, struct sdl2_console, dgc);
    SDL_GLContext sdlctx = (SDL_GLContext)ctx;

    assert(scon->opengl);

    return SDL_GL_MakeCurrent(scon->real_window, sdlctx);
}

void sdl2_gl_scanout_disable(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);
    scon->w = 0;
    scon->h = 0;
    sdl2_set_scanout_mode(scon, false);
}

void sdl2_gl_scanout_texture(DisplayChangeListener *dcl,
                             uint32_t backing_id,
                             bool backing_y_0_top,
                             uint32_t backing_width,
                             uint32_t backing_height,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h,
                             void *d3d_tex2d)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);
    scon->x = x;
    scon->y = y;
    scon->w = w;
    scon->h = h;
    scon->y0_top = backing_y_0_top;

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    sdl2_set_scanout_mode(scon, true);
    egl_fb_setup_for_tex(&scon->guest_fb, backing_width, backing_height,
                         backing_id, false);
}

void sdl2_gl_scanout_flush(DisplayChangeListener *dcl,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    int ww, wh;

    assert(scon->opengl);
    if (!scon->scanout_mode)
    {
        return;
    }
    if (!scon->guest_fb.framebuffer)
    {
        return;
    }

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    /* a real guest frame has a damaged area; redraws pass 0,0,0,0 */
    if (w || h)
    {
        static bool gl_logged;
        if (!gl_logged)
        {
            gl_logged = true;
            gtv_perf_gl_info((const char *)glGetString(GL_RENDERER),
                             (const char *)glGetString(GL_VENDOR),
                             (const char *)glGetString(GL_VERSION));
        }
        gtv_perf_frame();
    }

    SDL_GetWindowSize(scon->real_window, &ww, &wh);
    egl_fb_setup_default(&scon->win_fb, ww, wh);
    egl_fb_blit(&scon->win_fb, &scon->guest_fb, !scon->y0_top);

    launcher_gl_update(scon->real_window);
    launcher_gl_draw_notice(scon->real_window);
    if (gtv_perf_hud_visible())
    {
        launcher_gl_draw_hud(scon->real_window);
    }

    gint64 t0 = g_get_monotonic_time();
    SDL_GL_SwapWindow(scon->real_window);
    gtv_perf_swap_us((unsigned)(g_get_monotonic_time() - t0));
}

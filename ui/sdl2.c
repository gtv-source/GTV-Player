/*
 * QEMU SDL display driver
 *
 * Copyright (c) 2003 Fabrice Bellard
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
/* Ported SDL 1.2 code to 2.0 by Dave Airlie. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include <math.h>
#include "block/aio.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"
#include "sysemu/runstate.h"
#include "sysemu/runstate-action.h"
#include "sysemu/sysemu.h"
#include "qemu/log.h"
#include "launcher_gl.h"
#include "adb.h"
#include "stb_image.h"

static int sdl2_num_outputs;
static struct sdl2_console *sdl2_console;

// custom
static SDL_Surface *window_icon;
static int forced_xres = 0;
static int forced_yres = 0;

static SDL_Surface *guest_sprite_surface;
static int gui_grab; /* if true, all keyboard/mouse events are grabbed */
static bool alt_grab;
static bool ctrl_grab;

// static int gui_saved_grab;
static int gui_fullscreen;
static int gui_grab_code = KMOD_LALT | KMOD_LCTRL;
static SDL_Cursor *sdl_cursor_normal;
static SDL_Cursor *sdl_cursor_hidden;
static int absolute_enabled;
static bool guest_cursor;
static int guest_x, guest_y;
static SDL_Cursor *guest_sprite;
static Notifier mouse_mode_notifier;

extern int mouse_x;
extern int mouse_y;

#define SDL2_REFRESH_INTERVAL_BUSY 10
#define SDL2_MAX_IDLE_COUNT (2 * GUI_REFRESH_INTERVAL_DEFAULT / SDL2_REFRESH_INTERVAL_BUSY + 1)

/* introduced in SDL 2.0.10 */
#ifndef SDL_HINT_RENDER_BATCHING
#define SDL_HINT_RENDER_BATCHING "SDL_RENDER_BATCHING"
#endif

static void sdl_update_caption(struct sdl2_console *scon);

static struct sdl2_console *get_scon_from_window(uint32_t window_id)
{
    int i;
    for (i = 0; i < sdl2_num_outputs; i++)
    {
        if (sdl2_console[i].real_window == SDL_GetWindowFromID(window_id))
        {
            return &sdl2_console[i];
        }
    }
    return NULL;
}

void sdl2_window_create(struct sdl2_console *scon)
{
    int flags = 0;

    if (!scon->surface)
    {
        return;
    }
    assert(!scon->real_window);

    if (gui_fullscreen)
    {
        // flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    else
    {
        // flags |= SDL_WINDOW_RESIZABLE;
    }
    if (scon->hidden)
    {
        flags |= SDL_WINDOW_HIDDEN;
    }
#ifdef CONFIG_OPENGL
    if (scon->opengl)
    {
        flags |= SDL_WINDOW_OPENGL;
    }
#endif

    int w = forced_xres ? forced_xres : surface_width(scon->surface);
    int h = forced_yres ? forced_yres : surface_height(scon->surface);

    scon->real_window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED,
                                         SDL_WINDOWPOS_UNDEFINED,
                                         w,
                                         h,
                                         flags);

    SDL_RaiseWindow(scon->real_window);

    if (window_icon)
    {
        fprintf(stderr, "[ICON] Applying icon to window\n");
        SDL_SetWindowIcon(scon->real_window, window_icon);
    }

    if (scon->opengl)
    {
        const char *driver = "opengl";

        if (scon->opts->gl == DISPLAY_GL_MODE_ES)
        {
            driver = "opengles2";
        }

        SDL_SetHint(SDL_HINT_RENDER_DRIVER, driver);
        SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");

        scon->winctx = SDL_GL_CreateContext(scon->real_window);
        SDL_GL_SetSwapInterval(0);

        // ADD LAUCHER
        launcher_gl_init(w, h);
    }
    else
    {
        /* The SDL renderer is only used by sdl2-2D, when OpenGL is disabled */
        scon->real_renderer = SDL_CreateRenderer(scon->real_window, -1, 0);
    }
    sdl_update_caption(scon);
}

void sdl2_window_destroy(struct sdl2_console *scon)
{
    if (!scon->real_window)
    {
        return;
    }

    if (scon->winctx)
    {
        SDL_GL_DeleteContext(scon->winctx);
        scon->winctx = NULL;
    }
    if (scon->real_renderer)
    {
        SDL_DestroyRenderer(scon->real_renderer);
        scon->real_renderer = NULL;
    }
    SDL_DestroyWindow(scon->real_window);
    scon->real_window = NULL;
}

void sdl2_window_resize(struct sdl2_console *scon)
{
    if (!scon->real_window)
    {
        return;
    }

    int w = forced_xres ? forced_xres : surface_width(scon->surface);
    int h = forced_yres ? forced_yres : surface_height(scon->surface);

    SDL_SetWindowSize(scon->real_window, w, h);
}

static void sdl2_redraw(struct sdl2_console *scon)
{
    if (scon->opengl)
    {
#ifdef CONFIG_OPENGL
        sdl2_gl_redraw(scon);
#endif
    }
    else
    {
        sdl2_2d_redraw(scon);
    }
}

/*
 * GAME_TITLE as UTF-8 for SDL. getenv() converts the environment to the ANSI
 * code page on Windows, which loses Vietnamese letters, so read it as UTF-16.
 */
static const char *sdl_game_title(void)
{
    static char *title;
#ifdef _WIN32
    static bool done;
    wchar_t buf[512];
    DWORD n;

    if (!done) {
        done = true;
        n = GetEnvironmentVariableW(L"GAME_TITLE", buf, G_N_ELEMENTS(buf));
        if (n > 0 && n < G_N_ELEMENTS(buf)) {
            title = g_utf16_to_utf8((const gunichar2 *)buf, n, NULL, NULL, NULL);
        }
    }
#else
    title = getenv("GAME_TITLE");
#endif
    return title && title[0] ? title : NULL;
}

static void sdl_update_caption(struct sdl2_console *scon)
{
    char win_title[1024];
    char icon_title[1024];
    const char *status = "";
    const char *game_title = sdl_game_title();
    if (!game_title)
        game_title = "G-PLAYER";

    //     if (!runstate_is_running())
    //     {
    //         status = " [Stopped]";
    //     }
    //     else if (gui_grab)
    //     {
    //         if (alt_grab)
    //         {
    // #ifdef CONFIG_DARWIN
    //             status = " - Press ⌃⌥⇧G to exit grab";
    // #else
    //             status = " - Press Ctrl-Alt-Shift-G to exit grab";
    // #endif
    //         }
    //         else if (ctrl_grab)
    //         {
    //             status = " - Press Right-Ctrl-G to exit grab";
    //         }
    //         else
    //         {
    // #ifdef CONFIG_DARWIN
    //             status = " - Press ⌃⌥G to exit grab";
    // #else
    //             status = " - Press Ctrl-Alt-G to exit grab";
    // #endif
    //         }
    //     }

    if (qemu_name)
    {
        snprintf(
            win_title, sizeof(win_title), "%s (%s-%d)%s",
            game_title,
            qemu_name,
            scon->idx, status);

        snprintf(
            icon_title, sizeof(icon_title),
            "%s (%s)",
            game_title,
            qemu_name);
    }
    else
    {
        snprintf(
            win_title, sizeof(win_title),
            "%s%s",
            game_title,
            status);

        snprintf(
            icon_title, sizeof(icon_title),
            "%s",
            game_title);
    }

    if (scon->real_window)
    {
        SDL_SetWindowTitle(scon->real_window, win_title);
    }
}

static void sdl_hide_cursor(struct sdl2_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor)
    {
        return;
    }

    SDL_ShowCursor(SDL_DISABLE);
    SDL_SetCursor(sdl_cursor_hidden);

    if (!qemu_input_is_absolute(scon->dcl.con))
    {
        SDL_SetRelativeMouseMode(SDL_TRUE);
    }
}

static void sdl_show_cursor(struct sdl2_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor)
    {
        return;
    }

    if (!qemu_input_is_absolute(scon->dcl.con))
    {
        SDL_SetRelativeMouseMode(SDL_FALSE);
    }

    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled))
    {
        SDL_SetCursor(guest_sprite);
    }
    else
    {
        SDL_SetCursor(sdl_cursor_normal);
    }

    SDL_ShowCursor(SDL_DISABLE);
}

static void sdl_grab_start(struct sdl2_console *scon)
{
    QemuConsole *con = scon ? scon->dcl.con : NULL;

    if (!con || !qemu_console_is_graphic(con))
    {
        return;
    }
    /*
     * If the application is not active, do not try to enter grab state. This
     * prevents 'SDL_WM_GrabInput(SDL_GRAB_ON)' from blocking all the
     * application (SDL bug).
     */
    if (!(SDL_GetWindowFlags(scon->real_window) & SDL_WINDOW_INPUT_FOCUS))
    {
        return;
    }
    if (guest_cursor)
    {
        SDL_SetCursor(guest_sprite);
        if (!qemu_input_is_absolute(scon->dcl.con) && !absolute_enabled)
        {
            SDL_WarpMouseInWindow(scon->real_window, guest_x, guest_y);
        }
    }
    else
    {
        sdl_hide_cursor(scon);
    }
    SDL_SetWindowGrab(scon->real_window, SDL_TRUE);
    gui_grab = 1;
    sdl_update_caption(scon);
}

static void sdl_grab_end(struct sdl2_console *scon)
{
    SDL_SetWindowGrab(scon->real_window, SDL_FALSE);
    gui_grab = 0;
    sdl_show_cursor(scon);
    sdl_update_caption(scon);
}

static void absolute_mouse_grab(struct sdl2_console *scon)
{
    int mouse_x, mouse_y;
    int scr_w, scr_h;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
    if (mouse_x > 0 && mouse_x < scr_w - 1 &&
        mouse_y > 0 && mouse_y < scr_h - 1)
    {
        sdl_grab_start(scon);
    }
}

static void sdl_mouse_mode_change_bh(void *opaque)
{
    if (qemu_input_is_absolute(sdl2_console[0].dcl.con))
    {
        if (!absolute_enabled)
        {
            absolute_enabled = 1;
            SDL_SetRelativeMouseMode(SDL_FALSE);
            absolute_mouse_grab(&sdl2_console[0]);
        }
    }
    else if (absolute_enabled)
    {
        if (!gui_fullscreen)
        {
            sdl_grab_end(&sdl2_console[0]);
        }
        absolute_enabled = 0;
    }
}

/*
 * The mouse mode notifier runs in whatever thread changed the input mode, e.g. a
 * vCPU thread writing a virtio status register with the BQL held.  SDL window
 * calls from there (SetWindowTextW, grab) send a message to the UI thread and
 * wait for it, while the UI thread itself is waiting for the BQL: the whole VM
 * hangs ("Not Responding").  Do the SDL work in the main loop instead.
 */
static void sdl_mouse_mode_change(Notifier *notify, void *data)
{
    aio_bh_schedule_oneshot(qemu_get_aio_context(), sdl_mouse_mode_change_bh, NULL);
}

static void sdl_send_mouse_event(struct sdl2_console *scon, int dx, int dy,
                                 int x, int y, int state)
{
    static uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT] = SDL_BUTTON(SDL_BUTTON_LEFT),
        [INPUT_BUTTON_MIDDLE] = SDL_BUTTON(SDL_BUTTON_MIDDLE),
        [INPUT_BUTTON_RIGHT] = SDL_BUTTON(SDL_BUTTON_RIGHT),
        [INPUT_BUTTON_SIDE] = SDL_BUTTON(SDL_BUTTON_X1),
        [INPUT_BUTTON_EXTRA] = SDL_BUTTON(SDL_BUTTON_X2)};
    static uint32_t prev_state;

    if (prev_state != state)
    {
        qemu_input_update_buttons(scon->dcl.con, bmap, prev_state, state);
        prev_state = state;
    }

    if (qemu_input_is_absolute(scon->dcl.con))
    {
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_X,
                             x, 0, surface_width(scon->surface));
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_Y,
                             y, 0, surface_height(scon->surface));
    }
    else
    {
        if (guest_cursor)
        {
            x -= guest_x;
            y -= guest_y;
            guest_x += x;
            guest_y += y;
            dx = x;
            dy = y;
        }
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
}

// static void toggle_full_screen(struct sdl2_console *scon)
// {
//     gui_fullscreen = !gui_fullscreen;
//     if (gui_fullscreen)
//     {
//         SDL_SetWindowFullscreen(scon->real_window,
//                                 SDL_WINDOW_FULLSCREEN_DESKTOP);
//         gui_saved_grab = gui_grab;
//         sdl_grab_start(scon);
//     }
//     else
//     {
//         if (!gui_saved_grab)
//         {
//             sdl_grab_end(scon);
//         }
//         SDL_SetWindowFullscreen(scon->real_window, 0);
//     }
//     sdl2_redraw(scon);
// }

static int get_mod_state(void)
{
    SDL_Keymod mod = SDL_GetModState();

    if (alt_grab)
    {
        return (mod & (gui_grab_code | KMOD_LSHIFT)) ==
               (gui_grab_code | KMOD_LSHIFT);
    }
    else if (ctrl_grab)
    {
        return (mod & KMOD_RCTRL) == KMOD_RCTRL;
    }
    else
    {
        return (mod & gui_grab_code) == gui_grab_code;
    }
}

static void handle_keydown(SDL_Event *ev)
{
    // int win;
    struct sdl2_console *scon = get_scon_from_window(ev->key.windowID);
    // int gui_key_modifier_pressed = get_mod_state();

    if (!scon)
    {
        return;
    }

    scon->gui_keysym = false;

    if ((ev->key.keysym.mod & KMOD_SHIFT) &&
        (ev->key.keysym.mod & KMOD_GUI) &&
        ev->key.keysym.sym == SDLK_s)
    {
        scon->gui_keysym = true;
    }
    //     if (!scon->ignore_hotkeys && gui_key_modifier_pressed && !ev->key.repeat)
    //     {
    //         switch (ev->key.keysym.scancode)
    //         {
    //         case SDL_SCANCODE_2:
    //         case SDL_SCANCODE_3:
    //         case SDL_SCANCODE_4:
    //         case SDL_SCANCODE_5:
    //         case SDL_SCANCODE_6:
    //         case SDL_SCANCODE_7:
    //         case SDL_SCANCODE_8:
    //         case SDL_SCANCODE_9:
    //             if (gui_grab)
    //             {
    //                 sdl_grab_end(scon);
    //             }

    //             win = ev->key.keysym.scancode - SDL_SCANCODE_1;
    //             if (win < sdl2_num_outputs)
    //             {
    //                 sdl2_console[win].hidden = !sdl2_console[win].hidden;
    //                 if (sdl2_console[win].real_window)
    //                 {
    //                     if (sdl2_console[win].hidden)
    //                     {
    //                         SDL_HideWindow(sdl2_console[win].real_window);
    //                     }
    //                     else
    //                     {
    //                         SDL_ShowWindow(sdl2_console[win].real_window);
    //                     }
    //                 }
    //                 sdl2_release_modifiers(scon);
    //                 scon->gui_keysym = true;
    //             }
    //             break;
    //         case SDL_SCANCODE_F:
    //             toggle_full_screen(scon);
    //             scon->gui_keysym = true;
    //             break;
    //         case SDL_SCANCODE_G:
    //             scon->gui_keysym = true;
    //             if (!gui_grab)
    //             {
    //                 sdl_grab_start(scon);
    //             }
    //             else if (!gui_fullscreen)
    //             {
    //                 sdl_grab_end(scon);
    //             }
    //             break;
    //         case SDL_SCANCODE_U:
    //             sdl2_window_resize(scon);
    //             if (!scon->opengl)
    //             {
    //                 /* re-create scon->texture */
    //                 sdl2_2d_switch(&scon->dcl, scon->surface);
    //             }
    //             scon->gui_keysym = true;
    //             break;
    // #if 0
    //         case SDL_SCANCODE_KP_PLUS:
    //         case SDL_SCANCODE_KP_MINUS:
    //             if (!gui_fullscreen) {
    //                 int scr_w, scr_h;
    //                 int width, height;
    //                 SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);

    //                 width = MAX(scr_w + (ev->key.keysym.scancode ==
    //                                      SDL_SCANCODE_KP_PLUS ? 50 : -50),
    //                             160);
    //                 height = (surface_height(scon->surface) * width) /
    //                     surface_width(scon->surface);
    //                 fprintf(stderr, "%s: scale to %dx%d\n",
    //                         __func__, width, height);
    //                 sdl_scale(scon, width, height);
    //                 sdl2_redraw(scon);
    //                 scon->gui_keysym = true;
    //             }
    // #endif
    //         default:
    //             break;
    //         }
    //     }

    if (!scon->gui_keysym)
    {
        // if (ev->key.repeat)
        // {
        //     return;
        // }

        sdl2_process_key(scon, &ev->key);
    }
}

static void handle_keyup(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->key.windowID);

    if (!scon)
    {
        return;
    }

    scon->ignore_hotkeys = false;
    sdl2_process_key(scon, &ev->key);
}

static void handle_textinput(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->text.windowID);
    QemuConsole *con = scon ? scon->dcl.con : NULL;

    if (!con)
    {
        return;
    }

    if (!scon->gui_keysym && QEMU_IS_TEXT_CONSOLE(con))
    {
        qemu_text_console_put_string(QEMU_TEXT_CONSOLE(con), ev->text.text, strlen(ev->text.text));
    }
}

/*
 * Mouse wheel -> swipe.  The guest only has a touchscreen, which has no
 * wheel, so each wheel step becomes a finger drag that starts at the
 * pointer: press, move a little every tick, hold still and lift.  Steps that
 * arrive while the finger is down extend the same drag, so spinning the
 * wheel scrolls smoothly; holding still before lifting means the guest sees
 * no fling and every step scrolls the same distance.
 */
#define WHEEL_TICK_MS      10
#define WHEEL_STEP_FRAC    0.10f   /* drag per wheel step, of window height */
#define WHEEL_SPEED_FRAC   0.025f  /* drag per tick, of window height */
#define WHEEL_HOLD_TICKS   8       /* still ticks before lifting */
#define WHEEL_MIN_DRAG     24.0f   /* px, well above Android's touch slop */

static struct
{
    struct sdl2_console *scon;
    QEMUTimer *timer;
    bool down;
    int x0, y0;          /* where the finger went down */
    float cx, cy;        /* current offset from x0/y0 */
    float tx, ty;        /* target offset */
    float px, py;        /* wheel distance not yet added to the target */
    int still;
} wheel;

static void wheel_send(int x, int y, bool down)
{
    sdl_send_mouse_event(wheel.scon, 0, 0, x, y,
                         down ? SDL_BUTTON(SDL_BUTTON_LEFT) : 0);
}

static void wheel_lift(void)
{
    if (wheel.down)
    {
        wheel_send(wheel.x0 + (int)wheel.cx, wheel.y0 + (int)wheel.cy, false);
        wheel.down = false;
    }
}

/* move the pending wheel distance into the target, keeping the finger
 * inside the window; returns false when some of it did not fit */
static bool wheel_take_pending(int w, int h)
{
    float tx = wheel.tx + wheel.px, ty = wheel.ty + wheel.py;
    float cx = MIN(MAX(tx, 1 - wheel.x0), w - 2 - wheel.x0);
    float cy = MIN(MAX(ty, 1 - wheel.y0), h - 2 - wheel.y0);

    wheel.px = tx - cx;
    wheel.py = ty - cy;
    wheel.tx = cx;
    wheel.ty = cy;
    return fabsf(wheel.px) < 1 && fabsf(wheel.py) < 1;
}

static void wheel_tick(void *opaque)
{
    int w, h;

    SDL_GetWindowSize(wheel.scon->real_window, &w, &h);

    if (!wheel.down)
    {
        /* a drag shorter than the guest's touch slop would be a tap and
         * click whatever is under the pointer (small touchpad scrolls):
         * keep collecting until there is enough to move */
        if (fabsf(wheel.px) < WHEEL_MIN_DRAG && fabsf(wheel.py) < WHEEL_MIN_DRAG)
        {
            return;                     /* timer stops until the next step */
        }
        SDL_GetMouseState(&wheel.x0, &wheel.y0);
        wheel.x0 = MIN(MAX(wheel.x0, 1), w - 2);
        wheel.y0 = MIN(MAX(wheel.y0, 1), h - 2);
        wheel.cx = wheel.cy = wheel.tx = wheel.ty = 0;
        wheel.still = 0;
        wheel.down = true;
        wheel_send(wheel.x0, wheel.y0, true);
    }
    else
    {
        bool fits = wheel_take_pending(w, h);
        float dx = wheel.tx - wheel.cx, dy = wheel.ty - wheel.cy;
        float dist = sqrtf(dx * dx + dy * dy);
        float speed = MAX(4.0f, h * WHEEL_SPEED_FRAC);

        if (dist >= 0.5f)
        {
            float k = dist > speed ? speed / dist : 1.0f;
            wheel.cx += dx * k;
            wheel.cy += dy * k;
            wheel.still = 0;
            wheel_send(wheel.x0 + (int)wheel.cx, wheel.y0 + (int)wheel.cy, true);
        }
        else if (!fits || ++wheel.still >= WHEEL_HOLD_TICKS)
        {
            /* reached the window edge, or done: lift; what is left of the
             * wheel distance starts a new drag from the pointer */
            wheel_lift();
        }
    }
    timer_mod(wheel.timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + WHEEL_TICK_MS);
}

/* a real click or drag takes over: end the synthetic one first */
static void wheel_cancel(void)
{
    if (wheel.timer)
    {
        timer_del(wheel.timer);
    }
    wheel_lift();
    wheel.px = wheel.py = 0;
}

static void handle_mousewheel(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->wheel.windowID);
    SDL_MouseWheelEvent *wev = &ev->wheel;
    float sx, sy, step;
    int w, h;

    if (!scon || !qemu_console_is_graphic(scon->dcl.con) ||
        !qemu_input_is_absolute(scon->dcl.con) ||
        (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT)))
    {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 18)
    sx = wev->preciseX;
    sy = wev->preciseY;
#else
    sx = wev->x;
    sy = wev->y;
#endif
    if (wev->direction == SDL_MOUSEWHEEL_FLIPPED)
    {
        sx = -sx;
        sy = -sy;
    }

    SDL_GetWindowSize(scon->real_window, &w, &h);
    step = h * WHEEL_STEP_FRAC;
    /* wheel up shows what is above: the finger moves down; wheel right
     * shows what is on the right: the finger moves left */
    wheel.px -= sx * step;
    wheel.py += sy * step;

    if (!wheel.timer)
    {
        wheel.timer = timer_new_ms(QEMU_CLOCK_REALTIME, wheel_tick, NULL);
    }
    wheel.scon = scon;
    if (!timer_pending(wheel.timer))
    {
        wheel_tick(NULL);
    }
}

static void handle_mousemotion(SDL_Event *ev)
{
    mouse_x = ev->motion.x;
    mouse_y = ev->motion.y;
    int max_x, max_y;
    struct sdl2_console *scon = get_scon_from_window(ev->motion.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con))
    {
        return;
    }
    if (wheel.down)
    {
        return;     /* the wheel drag owns the finger until it lifts */
    }

    if (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)
    {
        int scr_w, scr_h;
        SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
        max_x = scr_w - 1;
        max_y = scr_h - 1;
        if (gui_grab && !gui_fullscreen && (ev->motion.x == 0 || ev->motion.y == 0 || ev->motion.x == max_x || ev->motion.y == max_y))
        {
            sdl_grab_end(scon);
        }
        if (!gui_grab &&
            (ev->motion.x > 0 && ev->motion.x < max_x &&
             ev->motion.y > 0 && ev->motion.y < max_y))
        {
            sdl_grab_start(scon);
        }
    }
    if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)
    {
        sdl_send_mouse_event(scon, ev->motion.xrel, ev->motion.yrel,
                             ev->motion.x, ev->motion.y, ev->motion.state);
    }
}

static void handle_mousebutton(SDL_Event *ev)
{
    int buttonstate = SDL_GetMouseState(NULL, NULL);
    SDL_MouseButtonEvent *bev;
    struct sdl2_console *scon = get_scon_from_window(ev->button.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con))
    {
        return;
    }

    bev = &ev->button;
    wheel_cancel();
    if (!gui_grab && !qemu_input_is_absolute(scon->dcl.con))
    {
        if (ev->type == SDL_MOUSEBUTTONUP && bev->button == SDL_BUTTON_LEFT)
        {
            /* start grabbing all events */
            sdl_grab_start(scon);
        }
    }
    else
    {
        if (ev->type == SDL_MOUSEBUTTONDOWN)
        {
            buttonstate |= SDL_BUTTON(bev->button);
        }
        else
        {
            buttonstate &= ~SDL_BUTTON(bev->button);
        }
        sdl_send_mouse_event(scon, 0, 0, bev->x, bev->y, buttonstate);
    }
}

static void handle_windowevent(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->window.windowID);
    bool allow_close = true;

    if (!scon)
    {
        return;
    }

    switch (ev->window.event)
    {
    case SDL_WINDOWEVENT_RESIZED:
    {
        QemuUIInfo info;
        memset(&info, 0, sizeof(info));
        info.width = ev->window.data1;
        info.height = ev->window.data2;
        dpy_set_ui_info(scon->dcl.con, &info, true);
    }
        sdl2_redraw(scon);
        break;
    case SDL_WINDOWEVENT_EXPOSED:
        sdl2_redraw(scon);
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED:
        /* fall through */
    case SDL_WINDOWEVENT_ENTER:
        if (!gui_grab && (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled))
        {
            absolute_mouse_grab(scon);
        }
        /* If a new console window opened using a hotkey receives the
         * focus, SDL sends another KEYDOWN event to the new window,
         * closing the console window immediately after.
         *
         * Work around this by ignoring further hotkey events until a
         * key is released.
         */
        scon->ignore_hotkeys = get_mod_state();
        break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        if (gui_grab && !gui_fullscreen)
        {
            sdl_grab_end(scon);
        }
        break;
    case SDL_WINDOWEVENT_RESTORED:
        update_displaychangelistener(&scon->dcl, GUI_REFRESH_INTERVAL_DEFAULT);
        break;
    case SDL_WINDOWEVENT_MINIMIZED:
        update_displaychangelistener(&scon->dcl, 500);
        break;
    case SDL_WINDOWEVENT_CLOSE:
        if (qemu_console_is_graphic(scon->dcl.con))
        {
            if (scon->opts->has_window_close && !scon->opts->window_close)
            {
                allow_close = false;
            }
            if (allow_close)
            {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
        }
        else
        {
            SDL_HideWindow(scon->real_window);
            scon->hidden = true;
        }
        break;
    case SDL_WINDOWEVENT_SHOWN:
        scon->hidden = false;
        break;
    case SDL_WINDOWEVENT_HIDDEN:
        scon->hidden = true;
        break;
    }
}

void sdl2_poll_events(struct sdl2_console *scon)
{
    SDL_Event ev1, *ev = &ev1;
    bool allow_close = true;
    int idle = 1;

    if (scon->last_vm_running != runstate_is_running())
    {
        scon->last_vm_running = runstate_is_running();
        sdl_update_caption(scon);
    }

    while (SDL_PollEvent(ev))
    {
        switch (ev->type)
        {
        case SDL_KEYDOWN:
            idle = 0;
            handle_keydown(ev);
            break;
        case SDL_KEYUP:
            idle = 0;
            handle_keyup(ev);
            break;
        case SDL_TEXTINPUT:
            idle = 0;
            handle_textinput(ev);
            break;
        case SDL_QUIT:
            adb_stop();
            if (scon->opts->has_window_close && !scon->opts->window_close)
            {
                allow_close = false;
            }
            if (allow_close)
            {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
            break;
        case SDL_MOUSEMOTION:
            idle = 0;
            handle_mousemotion(ev);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            idle = 0;
            handle_mousebutton(ev);
            break;
        case SDL_MOUSEWHEEL:
            idle = 0;
            handle_mousewheel(ev);
            break;
        case SDL_WINDOWEVENT:
            handle_windowevent(ev);
            break;
        default:
            break;
        }
    }

    if (idle)
    {
        if (scon->idle_counter < SDL2_MAX_IDLE_COUNT)
        {
            scon->idle_counter++;
            if (scon->idle_counter >= SDL2_MAX_IDLE_COUNT)
            {
                scon->dcl.update_interval = GUI_REFRESH_INTERVAL_DEFAULT;
            }
        }
    }
    else
    {
        scon->idle_counter = 0;
        scon->dcl.update_interval = SDL2_REFRESH_INTERVAL_BUSY;
    }
}

static void sdl_mouse_warp(DisplayChangeListener *dcl,
                           int x, int y, bool on)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    if (!qemu_console_is_graphic(scon->dcl.con))
    {
        return;
    }

    if (on)
    {
        if (!guest_cursor)
        {
            sdl_show_cursor(scon);
        }
        if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)
        {
            SDL_SetCursor(guest_sprite);
            if (!qemu_input_is_absolute(scon->dcl.con) && !absolute_enabled)
            {
                SDL_WarpMouseInWindow(scon->real_window, x, y);
            }
        }
    }
    else if (gui_grab)
    {
        sdl_hide_cursor(scon);
    }
    guest_cursor = on;
    guest_x = x, guest_y = y;
}

static void sdl_mouse_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{

    if (guest_sprite)
    {
        SDL_FreeCursor(guest_sprite);
    }

    if (guest_sprite_surface)
    {
        SDL_FreeSurface(guest_sprite_surface);
    }

    guest_sprite_surface =
        SDL_CreateRGBSurfaceFrom(c->data, c->width, c->height, 32, c->width * 4,
                                 0xff0000, 0x00ff00, 0xff, 0xff000000);

    if (!guest_sprite_surface)
    {
        fprintf(stderr, "Failed to make rgb surface from %p\n", c);
        return;
    }
    guest_sprite = SDL_CreateColorCursor(guest_sprite_surface,
                                         c->hot_x, c->hot_y);
    if (!guest_sprite)
    {
        fprintf(stderr, "Failed to make color cursor from %p\n", c);
        return;
    }
    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(dcl->con) || absolute_enabled))
    {
        SDL_SetCursor(guest_sprite);
    }
}

static void sdl_cleanup(void)
{
    if (guest_sprite)
    {
        SDL_FreeCursor(guest_sprite);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static const DisplayChangeListenerOps dcl_2d_ops = {
    .dpy_name = "sdl2-2d",
    .dpy_gfx_update = sdl2_2d_update,
    .dpy_gfx_switch = sdl2_2d_switch,
    .dpy_gfx_check_format = sdl2_2d_check_format,
    .dpy_refresh = sdl2_2d_refresh,
    .dpy_mouse_set = sdl_mouse_warp,
    .dpy_cursor_define = sdl_mouse_define,
};

#ifdef CONFIG_OPENGL
static const DisplayChangeListenerOps dcl_gl_ops = {
    .dpy_name = "sdl2-gl",
    .dpy_gfx_update = sdl2_gl_update,
    .dpy_gfx_switch = sdl2_gl_switch,
    .dpy_gfx_check_format = console_gl_check_format,
    .dpy_refresh = sdl2_gl_refresh,
    .dpy_mouse_set = sdl_mouse_warp,
    .dpy_cursor_define = sdl_mouse_define,

    .dpy_gl_scanout_disable = sdl2_gl_scanout_disable,
    .dpy_gl_scanout_texture = sdl2_gl_scanout_texture,
    .dpy_gl_update = sdl2_gl_scanout_flush,
};

static bool
sdl2_gl_is_compatible_dcl(DisplayGLCtx *dgc,
                          DisplayChangeListener *dcl)
{
    return dcl->ops == &dcl_gl_ops;
}

static const DisplayGLCtxOps gl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = sdl2_gl_is_compatible_dcl,
    .dpy_gl_ctx_create = sdl2_gl_create_context,
    .dpy_gl_ctx_destroy = sdl2_gl_destroy_context,
    .dpy_gl_ctx_make_current = sdl2_gl_make_context_current,
};
#endif

static void sdl2_display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_SDL);
    if (o->has_gl && o->gl)
    {
#ifdef CONFIG_OPENGL
        display_opengl = 1;
#endif
    }
}

/*
 * Window icon (PNG) through stb_image, which the launcher already uses, so
 * QEMU does not need SDL2_image and its image codec DLLs.
 */
static SDL_Surface *sdl_load_icon(const char *path)
{
    int w, h;
    unsigned char *px = stbi_load(path, &w, &h, NULL, 4);
    SDL_Surface *s;

    if (!px) {
        return NULL;
    }
    s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (s) {
        for (int y = 0; y < h; y++) {
            memcpy((uint8_t *)s->pixels + y * s->pitch, px + y * w * 4, w * 4);
        }
    }
    stbi_image_free(px);
    return s;
}

static void sdl2_display_init(DisplayState *ds, DisplayOptions *o)
{
    uint8_t data = 0;
    int i;
    SDL_SysWMinfo info;
    window_icon = NULL;
    char *dir;

    const char *xres_env = getenv("XRES");
    const char *yres_env = getenv("YRES");

    if (xres_env && yres_env)
    {
        forced_xres = atoi(xres_env);
        forced_yres = atoi(yres_env);
    }

    assert(o->type == DISPLAY_TYPE_SDL);

    if (SDL_GetHintBoolean("QEMU_ENABLE_SDL_LOGGING", SDL_FALSE))
    {
        SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);
    }

    if (SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "Could not initialize SDL(%s) - exiting\n",
                SDL_GetError());
        exit(1);
    }
#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR /* only available since SDL 2.0.8 */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
#ifdef SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
#endif
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "1");
    SDL_EnableScreenSaver();
    memset(&info, 0, sizeof(info));
    SDL_VERSION(&info.version);

    gui_fullscreen = o->has_full_screen && o->full_screen;

    if (o->u.sdl.has_grab_mod)
    {
        if (o->u.sdl.grab_mod == HOT_KEY_MOD_LSHIFT_LCTRL_LALT)
        {
            alt_grab = true;
        }
        else if (o->u.sdl.grab_mod == HOT_KEY_MOD_RCTRL)
        {
            ctrl_grab = true;
        }
    }

    for (i = 0;; i++)
    {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        if (!con)
        {
            break;
        }
    }
    sdl2_num_outputs = i;
    if (sdl2_num_outputs == 0)
    {
        return;
    }
    sdl2_console = g_new0(struct sdl2_console, sdl2_num_outputs);
    for (i = 0; i < sdl2_num_outputs; i++)
    {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        assert(con != NULL);
        if (!qemu_console_is_graphic(con) &&
            qemu_console_get_index(con) != 0)
        {
            sdl2_console[i].hidden = true;
        }
        sdl2_console[i].idx = i;
        sdl2_console[i].opts = o;
#ifdef CONFIG_OPENGL
        sdl2_console[i].opengl = display_opengl;
        sdl2_console[i].dcl.ops = display_opengl ? &dcl_gl_ops : &dcl_2d_ops;
        sdl2_console[i].dgc.ops = display_opengl ? &gl_ctx_ops : NULL;
#else
        sdl2_console[i].opengl = 0;
        sdl2_console[i].dcl.ops = &dcl_2d_ops;
#endif
        sdl2_console[i].dcl.con = con;
        sdl2_console[i].kbd = qkbd_state_init(con);
        if (display_opengl)
        {
            qemu_console_set_display_gl_ctx(con, &sdl2_console[i].dgc);
        }
        register_displaychangelistener(&sdl2_console[i].dcl);

#if defined(SDL_VIDEO_DRIVER_WINDOWS) || defined(SDL_VIDEO_DRIVER_X11)
        if (SDL_GetWindowWMInfo(sdl2_console[i].real_window, &info))
        {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
            qemu_console_set_window_id(con, (uintptr_t)info.info.win.window);
#elif defined(SDL_VIDEO_DRIVER_X11)
            qemu_console_set_window_id(con, info.info.x11.window);
#endif
        }
#endif
    }

    // #ifdef CONFIG_SDL_IMAGE
    //     dir = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/128x128/apps/gtv.png");
    //     window_icon = IMG_Load(dir);
    // #else
    //     /* Load a 32x32x4 image. White pixels are transparent. */
    //     dir = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/32x32/apps/gtv.bmp");
    //     window_icon = SDL_LoadBMP(dir);
    //     if (window_icon)
    //     {
    //         uint32_t colorkey = SDL_MapRGB(window_icon->format, 255, 255, 255);
    //         SDL_SetColorKey(window_icon, SDL_TRUE, colorkey);
    //     }
    // #endif

    const char *icon_env = getenv("ICON_PATH");

    if (icon_env && icon_env[0] != '\0')
    {
        fprintf(stderr, "[ICON] Using env ICON_PATH: %s\n", icon_env);
        dir = g_strdup(icon_env);
    }
    else
    {
        dir = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/128x128/apps/gtv.png");
        fprintf(stderr, "[ICON] Using default path: %s\n", dir);
    }

    window_icon = sdl_load_icon(dir);

    if (!window_icon)
    {
        fprintf(stderr, "[ICON] Failed to load icon: %s | %s\n",
                dir, stbi_failure_reason());
    }
    else
    {
        fprintf(stderr, "[ICON] Loaded icon successfully: %s\n", dir);
    }
    g_free(dir);

    if (window_icon)
    {
        SDL_SetWindowIcon(sdl2_console[0].real_window, window_icon);
    }

    mouse_mode_notifier.notify = sdl_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&mouse_mode_notifier);

    sdl_cursor_hidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    sdl_cursor_normal = SDL_GetCursor();

    if (gui_fullscreen)
    {
        sdl_grab_start(&sdl2_console[0]);
    }

    atexit(sdl_cleanup);
}

static QemuDisplay qemu_display_sdl2 = {
    .type = DISPLAY_TYPE_SDL,
    .early_init = sdl2_display_early_init,
    .init = sdl2_display_init,
};

static void register_sdl1(void)
{
    qemu_display_register(&qemu_display_sdl2);
}

type_init(register_sdl1);

#ifdef CONFIG_OPENGL
module_dep("ui-opengl");
#endif

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
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"
#include "trace.h"

/*
 * Lock keys (NumLock, CapsLock): the guest boots with them off and only
 * learns about them from key presses, so with NumLock already on in Windows
 * the keypad typed arrows/Home/End in Android instead of digits.  Before each
 * key we compare the host lock state with the guest's and press the lock key
 * in the guest when they differ.
 *
 * guest_locks is what we believe the guest has: toggled by every lock key we
 * send, and replaced by the real LED state whenever the guest reports it
 * (Android writes EV_LED to the virtio keyboard when its lock state changes).
 */
static int guest_locks;
static QEMUPutLEDEntry *led_entry;

static void sdl2_led_event(void *opaque, int ledstate)
{
    int locks = ledstate & (QEMU_NUM_LOCK_LED | QEMU_CAPS_LOCK_LED);

    if (locks != guest_locks)
    {
        printf("[KBD] guest locks: num=%d caps=%d\n",
               !!(locks & QEMU_NUM_LOCK_LED), !!(locks & QEMU_CAPS_LOCK_LED));
        fflush(stdout);
    }
    guest_locks = locks;
}

static void sdl2_sync_lock(struct sdl2_console *scon, int led,
                           QKeyCode qcode, bool host_on)
{
    if (host_on == !!(guest_locks & led))
    {
        return;
    }
    qkbd_state_key_event(scon->kbd, qcode, true);
    qkbd_state_key_event(scon->kbd, qcode, false);
    guest_locks ^= led;
}

static void sdl2_sync_locks(struct sdl2_console *scon, SDL_KeyboardEvent *ev)
{
    if (!led_entry)
    {
        led_entry = qemu_add_led_event_handler(sdl2_led_event, NULL);
    }
    if (ev->type != SDL_KEYDOWN || QEMU_IS_TEXT_CONSOLE(scon->dcl.con))
    {
        return;
    }
    switch (ev->keysym.scancode)
    {
    case SDL_SCANCODE_NUMLOCKCLEAR:
        guest_locks ^= QEMU_NUM_LOCK_LED;   /* forwarded below */
        return;
    case SDL_SCANCODE_CAPSLOCK:
        guest_locks ^= QEMU_CAPS_LOCK_LED;
        return;
    default:
        break;
    }
#ifdef _WIN32
    /* SDL's KMOD_NUM/KMOD_CAPS are only read from Windows on focus changes
     * and can be stale; ask Windows for the toggle state of this message */
    sdl2_sync_lock(scon, QEMU_NUM_LOCK_LED, Q_KEY_CODE_NUM_LOCK,
                   GetKeyState(VK_NUMLOCK) & 1);
    sdl2_sync_lock(scon, QEMU_CAPS_LOCK_LED, Q_KEY_CODE_CAPS_LOCK,
                   GetKeyState(VK_CAPITAL) & 1);
#else
    sdl2_sync_lock(scon, QEMU_NUM_LOCK_LED, Q_KEY_CODE_NUM_LOCK,
                   ev->keysym.mod & KMOD_NUM);
    sdl2_sync_lock(scon, QEMU_CAPS_LOCK_LED, Q_KEY_CODE_CAPS_LOCK,
                   ev->keysym.mod & KMOD_CAPS);
#endif
}

void sdl2_process_key(struct sdl2_console *scon,
                      SDL_KeyboardEvent *ev)
{
    int qcode;
    QemuConsole *con = scon->dcl.con;

    /* ---- disable hotkeys ---- */
    if (ev->keysym.sym == SDLK_LGUI ||
        ev->keysym.sym == SDLK_RGUI)
    {
        return;
    }

    if (ev->keysym.sym >= SDLK_F1 &&
        ev->keysym.sym <= SDLK_F12)
    {
        return;
    }

    if ((ev->keysym.mod & KMOD_ALT) &&
        ev->keysym.sym >= SDLK_F1 &&
        ev->keysym.sym <= SDLK_F12)
    {
        return;
    }

    /* ---- detect Shift + Win + S ---- */
    if ((ev->keysym.mod & KMOD_SHIFT) &&
        (ev->keysym.mod & KMOD_GUI) &&
        ev->keysym.sym == SDLK_s)
    {
        return;
    }
    /* --------------------------------------- */

    if (ev->keysym.scancode >= qemu_input_map_usb_to_qcode_len)
    {
        return;
    }
    qcode = qemu_input_map_usb_to_qcode[ev->keysym.scancode];
    sdl2_sync_locks(scon, ev);

    trace_sdl2_process_key(ev->keysym.scancode, qcode,
                           ev->type == SDL_KEYDOWN ? "down" : "up");
    qkbd_state_key_event(scon->kbd, qcode, ev->type == SDL_KEYDOWN);

    if (QEMU_IS_TEXT_CONSOLE(con))
    {
        QemuTextConsole *s = QEMU_TEXT_CONSOLE(con);
        bool ctrl = qkbd_state_modifier_get(scon->kbd, QKBD_MOD_CTRL);
        if (ev->type == SDL_KEYDOWN)
        {
            switch (qcode)
            {
            case Q_KEY_CODE_RET:
                qemu_text_console_put_keysym(s, '\n');
                break;
            default:
                qemu_text_console_put_qcode(s, qcode, ctrl);
                break;
            }
        }
    }
}

void sdl2_release_modifiers(struct sdl2_console *scon)
{
    qkbd_state_lift_all_keys(scon->kbd);
}

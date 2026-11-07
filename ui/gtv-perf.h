/*
 * G-Player: performance HUD data and [PERF] log lines (ui/gtv-perf.c).
 *
 * What makes a game stutter on some machines: too few vCPUs (guest CPU at
 * 100 %), too little RAM (guest swapping), a weak/software GPU on the host,
 * a slow network.  This module samples all of them once a second, keeps the
 * text for the HUD (Ctrl+Shift+F, or GPLAYER_HUD=1) and prints a [PERF] line
 * every 5 s to stdout, which the launcher writes to its log file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GTV_PERF_H
#define GTV_PERF_H

#define GTV_HUD_LINES 6
#define GTV_HUD_LEN 160

/* start the sampling thread once; the HUD starts visible with GPLAYER_HUD=1 */
void gtv_perf_start(const char *accel, int vcpus, int ram_mb);

/* the guest showed a new frame (display thread) */
void gtv_perf_frame(void);
/* SDL_GL_SwapWindow took that long (display thread) */
void gtv_perf_swap_us(unsigned us);
/* GL strings of the window context, once */
void gtv_perf_gl_info(const char *renderer, const char *vendor, const char *version);
/* a guest GPU command (virgl) took that long (main loop) */
void gtv_perf_gpu_cmd(unsigned type, unsigned us);
/* heartbeat from a GTV_TICK_MS main loop timer (ui/sdl2.c) */
#define GTV_TICK_MS 20
void gtv_perf_mainloop_tick(void);
/* bytes from the network to the guest / from the guest (net/slirp.c) */
void gtv_perf_net(unsigned rx, unsigned tx);
/* "cpu=.. game=.. avail=.. swap=.." from GtvClip in the guest */
void gtv_perf_guest_stat(const char *fields);

int gtv_perf_hud_visible(void);
void gtv_perf_hud_toggle(void);
/* ms since the last guest frame, 0 before the first one */
unsigned gtv_perf_ms_since_frame(void);
/*
 * Copy the HUD text: GTV_HUD_LINES lines, color per line (0 normal,
 * 1 warning, 2 bad).  Returns the number of lines (0 before the first sample).
 */
int gtv_perf_hud_text(char lines[GTV_HUD_LINES][GTV_HUD_LEN], int colors[GTV_HUD_LINES]);

#endif

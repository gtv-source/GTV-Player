#pragma once
#include <SDL2/SDL.h>

void launcher_gl_init(int w, int h);
void launcher_gl_update(SDL_Window *window);
void launcher_gl_notify_framebuffer(void);
void launcher_gl_notify_boot_complete(void);
void launcher_gl_render_progress(SDL_Window *window);
void launcher_gl_render_background(SDL_Window *window);
void launcher_gl_notify_progress(float p);
void launcher_gl_draw_hud(SDL_Window *window);
void launcher_gl_draw_notice(SDL_Window *window);
int launcher_gl_notice_pending(void);

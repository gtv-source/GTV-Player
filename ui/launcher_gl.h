#pragma once
#include <SDL2/SDL.h>

void launcher_gl_init(int w, int h);
void launcher_gl_update(SDL_Window *window);
void launcher_gl_notify_framebuffer(void);
void launcher_gl_notify_boot_complete(void);
void launcher_gl_render_progress(SDL_Window *window);
void launcher_gl_render_background(SDL_Window *window);
void launcher_gl_notify_progress(float p);

#ifndef LAUNCHER_2D_H
#define LAUNCHER_2D_H

#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// ================= INIT =================
// gọi 1 lần khi bắt đầu
void launcher_2d_init(void);

// ================= UPDATE =================
// gọi mỗi frame (trong sdl2_2d_update)
void launcher_2d_update(SDL_Renderer *renderer, SDL_Window *window);

// ================= EVENTS =================
// framebuffer ready (frame đầu)
void launcher_2d_notify_framebuffer(void);

// boot android xong
void launcher_2d_notify_boot_complete(void);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_2D_H
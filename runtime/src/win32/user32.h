/* Window/message layer shared with the graphics and input modules. */
#ifndef USER32_H
#define USER32_H

#include "../rt.h"
#include <SDL3/SDL.h>

void user32_init(void);

/* The SDL window behind a guest HWND (top-level windows only). */
SDL_Window *user32_sdl_window(uint32_t hwnd);
uint32_t user32_main_hwnd(void);
/* Logical client size of the window, as the guest sees it. */
void user32_client_size(uint32_t hwnd, int *w, int *h);
/* Called by D3D when it (re)sizes the back buffer / changes mode. */
void user32_set_client_size(uint32_t hwnd, int w, int h, bool fullscreen);

/* Pump SDL events into the message queue (main thread only). */
void user32_pump(void);

/* Event hooks run before translation; returning true swallows the event. */
typedef bool (*EventHook)(const SDL_Event *e);
void user32_add_event_hook(EventHook h);

/* Host -> guest SendMessage. */
uint32_t user32_send(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

#endif

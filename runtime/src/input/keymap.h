/* SDL scancode <-> Windows virtual key / DirectInput key code. */
#ifndef KEYMAP_H
#define KEYMAP_H

#include <SDL3/SDL.h>
#include <stdint.h>

/* DIK code is the PC set-1 scancode, with 0x80 set for E0-prefixed keys. */
uint8_t key_sdl_to_vk(SDL_Scancode sc);
uint8_t key_sdl_to_dik(SDL_Scancode sc);
SDL_Scancode key_vk_to_sdl(uint8_t vk);
SDL_Scancode key_dik_to_sdl(uint8_t dik);
const char *key_dik_name(uint8_t dik);

#endif

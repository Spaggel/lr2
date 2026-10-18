#include "keymap.h"

typedef struct {
    SDL_Scancode sc;
    uint8_t vk, dik;
    const char *name;
} Key;

static const Key s_keys[] = {
    {SDL_SCANCODE_ESCAPE, 0x1b, 0x01, "Esc"},
    {SDL_SCANCODE_1, '1', 0x02, "1"}, {SDL_SCANCODE_2, '2', 0x03, "2"},
    {SDL_SCANCODE_3, '3', 0x04, "3"}, {SDL_SCANCODE_4, '4', 0x05, "4"},
    {SDL_SCANCODE_5, '5', 0x06, "5"}, {SDL_SCANCODE_6, '6', 0x07, "6"},
    {SDL_SCANCODE_7, '7', 0x08, "7"}, {SDL_SCANCODE_8, '8', 0x09, "8"},
    {SDL_SCANCODE_9, '9', 0x0a, "9"}, {SDL_SCANCODE_0, '0', 0x0b, "0"},
    {SDL_SCANCODE_MINUS, 0xbd, 0x0c, "-"}, {SDL_SCANCODE_EQUALS, 0xbb, 0x0d, "="},
    {SDL_SCANCODE_BACKSPACE, 0x08, 0x0e, "Backspace"}, {SDL_SCANCODE_TAB, 0x09, 0x0f, "Tab"},
    {SDL_SCANCODE_Q, 'Q', 0x10, "Q"}, {SDL_SCANCODE_W, 'W', 0x11, "W"},
    {SDL_SCANCODE_E, 'E', 0x12, "E"}, {SDL_SCANCODE_R, 'R', 0x13, "R"},
    {SDL_SCANCODE_T, 'T', 0x14, "T"}, {SDL_SCANCODE_Y, 'Y', 0x15, "Y"},
    {SDL_SCANCODE_U, 'U', 0x16, "U"}, {SDL_SCANCODE_I, 'I', 0x17, "I"},
    {SDL_SCANCODE_O, 'O', 0x18, "O"}, {SDL_SCANCODE_P, 'P', 0x19, "P"},
    {SDL_SCANCODE_LEFTBRACKET, 0xdb, 0x1a, "["}, {SDL_SCANCODE_RIGHTBRACKET, 0xdd, 0x1b, "]"},
    {SDL_SCANCODE_RETURN, 0x0d, 0x1c, "Enter"}, {SDL_SCANCODE_LCTRL, 0x11, 0x1d, "Ctrl"},
    {SDL_SCANCODE_A, 'A', 0x1e, "A"}, {SDL_SCANCODE_S, 'S', 0x1f, "S"},
    {SDL_SCANCODE_D, 'D', 0x20, "D"}, {SDL_SCANCODE_F, 'F', 0x21, "F"},
    {SDL_SCANCODE_G, 'G', 0x22, "G"}, {SDL_SCANCODE_H, 'H', 0x23, "H"},
    {SDL_SCANCODE_J, 'J', 0x24, "J"}, {SDL_SCANCODE_K, 'K', 0x25, "K"},
    {SDL_SCANCODE_L, 'L', 0x26, "L"}, {SDL_SCANCODE_SEMICOLON, 0xba, 0x27, ";"},
    {SDL_SCANCODE_APOSTROPHE, 0xde, 0x28, "'"}, {SDL_SCANCODE_GRAVE, 0xc0, 0x29, "`"},
    {SDL_SCANCODE_LSHIFT, 0x10, 0x2a, "Shift"}, {SDL_SCANCODE_BACKSLASH, 0xdc, 0x2b, "\\"},
    {SDL_SCANCODE_Z, 'Z', 0x2c, "Z"}, {SDL_SCANCODE_X, 'X', 0x2d, "X"},
    {SDL_SCANCODE_C, 'C', 0x2e, "C"}, {SDL_SCANCODE_V, 'V', 0x2f, "V"},
    {SDL_SCANCODE_B, 'B', 0x30, "B"}, {SDL_SCANCODE_N, 'N', 0x31, "N"},
    {SDL_SCANCODE_M, 'M', 0x32, "M"}, {SDL_SCANCODE_COMMA, 0xbc, 0x33, ","},
    {SDL_SCANCODE_PERIOD, 0xbe, 0x34, "."}, {SDL_SCANCODE_SLASH, 0xbf, 0x35, "/"},
    {SDL_SCANCODE_RSHIFT, 0x10, 0x36, "Right Shift"}, {SDL_SCANCODE_KP_MULTIPLY, 0x6a, 0x37, "Num *"},
    {SDL_SCANCODE_LALT, 0x12, 0x38, "Alt"}, {SDL_SCANCODE_SPACE, 0x20, 0x39, "Space"},
    {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3a, "Caps Lock"},
    {SDL_SCANCODE_F1, 0x70, 0x3b, "F1"}, {SDL_SCANCODE_F2, 0x71, 0x3c, "F2"},
    {SDL_SCANCODE_F3, 0x72, 0x3d, "F3"}, {SDL_SCANCODE_F4, 0x73, 0x3e, "F4"},
    {SDL_SCANCODE_F5, 0x74, 0x3f, "F5"}, {SDL_SCANCODE_F6, 0x75, 0x40, "F6"},
    {SDL_SCANCODE_F7, 0x76, 0x41, "F7"}, {SDL_SCANCODE_F8, 0x77, 0x42, "F8"},
    {SDL_SCANCODE_F9, 0x78, 0x43, "F9"}, {SDL_SCANCODE_F10, 0x79, 0x44, "F10"},
    {SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 0x45, "Num Lock"}, {SDL_SCANCODE_SCROLLLOCK, 0x91, 0x46, "Scroll Lock"},
    {SDL_SCANCODE_KP_7, 0x67, 0x47, "Num 7"}, {SDL_SCANCODE_KP_8, 0x68, 0x48, "Num 8"},
    {SDL_SCANCODE_KP_9, 0x69, 0x49, "Num 9"}, {SDL_SCANCODE_KP_MINUS, 0x6d, 0x4a, "Num -"},
    {SDL_SCANCODE_KP_4, 0x64, 0x4b, "Num 4"}, {SDL_SCANCODE_KP_5, 0x65, 0x4c, "Num 5"},
    {SDL_SCANCODE_KP_6, 0x66, 0x4d, "Num 6"}, {SDL_SCANCODE_KP_PLUS, 0x6b, 0x4e, "Num +"},
    {SDL_SCANCODE_KP_1, 0x61, 0x4f, "Num 1"}, {SDL_SCANCODE_KP_2, 0x62, 0x50, "Num 2"},
    {SDL_SCANCODE_KP_3, 0x63, 0x51, "Num 3"}, {SDL_SCANCODE_KP_0, 0x60, 0x52, "Num 0"},
    {SDL_SCANCODE_KP_PERIOD, 0x6e, 0x53, "Num ."},
    {SDL_SCANCODE_F11, 0x7a, 0x57, "F11"}, {SDL_SCANCODE_F12, 0x7b, 0x58, "F12"},
    {SDL_SCANCODE_KP_ENTER, 0x0d, 0x9c, "Num Enter"}, {SDL_SCANCODE_RCTRL, 0x11, 0x9d, "Right Ctrl"},
    {SDL_SCANCODE_KP_DIVIDE, 0x6f, 0xb5, "Num /"}, {SDL_SCANCODE_PRINTSCREEN, 0x2c, 0xb7, "Print Screen"},
    {SDL_SCANCODE_RALT, 0x12, 0xb8, "Right Alt"}, {SDL_SCANCODE_PAUSE, 0x13, 0xc5, "Pause"},
    {SDL_SCANCODE_HOME, 0x24, 0xc7, "Home"}, {SDL_SCANCODE_UP, 0x26, 0xc8, "Up"},
    {SDL_SCANCODE_PAGEUP, 0x21, 0xc9, "Page Up"}, {SDL_SCANCODE_LEFT, 0x25, 0xcb, "Left"},
    {SDL_SCANCODE_RIGHT, 0x27, 0xcd, "Right"}, {SDL_SCANCODE_END, 0x23, 0xcf, "End"},
    {SDL_SCANCODE_DOWN, 0x28, 0xd0, "Down"}, {SDL_SCANCODE_PAGEDOWN, 0x22, 0xd1, "Page Down"},
    {SDL_SCANCODE_INSERT, 0x2d, 0xd2, "Insert"}, {SDL_SCANCODE_DELETE, 0x2e, 0xd3, "Delete"},
    {SDL_SCANCODE_LGUI, 0x5b, 0xdb, "Left Win"}, {SDL_SCANCODE_RGUI, 0x5c, 0xdc, "Right Win"},
    {SDL_SCANCODE_APPLICATION, 0x5d, 0xdd, "Apps"},
};
#define NKEYS (sizeof s_keys / sizeof *s_keys)

uint8_t key_sdl_to_vk(SDL_Scancode sc) {
    for (size_t i = 0; i < NKEYS; i++)
        if (s_keys[i].sc == sc)
            return s_keys[i].vk;
    return 0;
}

uint8_t key_sdl_to_dik(SDL_Scancode sc) {
    for (size_t i = 0; i < NKEYS; i++)
        if (s_keys[i].sc == sc)
            return s_keys[i].dik;
    return 0;
}

SDL_Scancode key_vk_to_sdl(uint8_t vk) {
    for (size_t i = 0; i < NKEYS; i++)
        if (s_keys[i].vk == vk)
            return s_keys[i].sc;
    return SDL_SCANCODE_UNKNOWN;
}

SDL_Scancode key_dik_to_sdl(uint8_t dik) {
    for (size_t i = 0; i < NKEYS; i++)
        if (s_keys[i].dik == dik)
            return s_keys[i].sc;
    return SDL_SCANCODE_UNKNOWN;
}

const char *key_dik_name(uint8_t dik) {
    for (size_t i = 0; i < NKEYS; i++)
        if (s_keys[i].dik == dik)
            return s_keys[i].name;
    return NULL;
}

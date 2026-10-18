/* LEGO Racers 2 gamepad layout: what the DirectInput gamepad reports and
   how races and menus use it (lr2pad.c). */
#ifndef LR2PAD_H
#define LR2PAD_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* DirectInput button numbers of a gamepad. */
enum {
    PADB_A, PADB_B, PADB_X, PADB_Y, PADB_LB, PADB_RB, PADB_BACK, PADB_LSTICK,
    PADB_START,                 /* 8: the game's joystick pause button */
    PADB_RSTICK, PADB_LT, PADB_RT, PADB_GUIDE,
    /* virtual buttons */
    PADB_RS_DOWN, PADB_RS_UP, PADB_RS_LEFT, PADB_RS_RIGHT,   /* right stick pushed */
    PADB_RACE,                  /* PADB_RACE + i: race action i, pad or keyboard */
    PADB_COUNT = PADB_RACE + 10,
};
_Static_assert(PADB_COUNT <= 32, "DIJOYSTATE has 32 buttons");

/* The game's joystick axis number of the race steering axis (the Ry slot). */
#define PAD_STEER_AXIS 4
#define PAD_TRIGGER_THRESHOLD 8000

/* dinput8.c */
bool di_key_down(uint8_t dik);
int di_pad_game_index(int slot);

/* lr2pad.c, called with the DirectInput lock held */
bool lr2pad_button(SDL_Gamepad *gp, int slot, int button);
float lr2pad_steer(SDL_Gamepad *gp, int slot);
void lr2pad_init(void);

#endif

/* Online VS on the 3DS (online_3ds.c): the lobby on the bottom screen, the
 * synchronised restart and boot, the lockstep steps, the one-console sync
 * test.  main_3ds.c asks it, every refresh, whether it drives the game. */
#ifndef PORT_ONLINE_3DS_H
#define PORT_ONLINE_3DS_H
#include <stdint.h>
#include "../pad.h"

#define ONLINE_DELAY 3   /* frames of input delay (50 ms), the same for both players */

/* dir: the app's folder; build: this program's id (both consoles must match);
 * local_card / local_unlock: the memory card folder and "everything
 * unlocked" of normal play, restored when an online match ends */
void online_init(const char *dir, uint32_t build, const char *local_card, int local_unlock);
void online_set_local(const char *local_card, int local_unlock);
void online_shutdown(void);                 /* leaving the app: tell the peer */

/* normal play: VS chosen on the main menu (the cursor on VS and a button
 * pressed): opens the lobby; 1 = the press was taken (give the game nothing) */
int online_take_vs(uint16_t buttons);
int online_lobby_open(void);                /* the lobby or a match: the bottom screen is ours */
int online_blocks_pad(void);                /* the player's buttons must not reach the game */
void online_touch(int code);                /* a lobby button (movelist_touch codes >= 100) */

/* every refresh, before the game's steps: the network, the lobby, a restart
 * when a match or the test begins (it takes a few seconds) */
void online_refresh(void);
int online_drives_game(void);               /* a match or the test runs the game */
/* how many game steps to run this refresh (0: none, show the last picture
 * and wait for the next refresh); buttons = this console's pad */
int online_plan(int steps_wanted, uint16_t buttons);
void online_pads(PadRaw *p0, PadRaw *p1);   /* before each of those steps */
int online_after_step(void);                /* after each; 0 = run no more steps this refresh */
int online_status(char *buf, int n);        /* a short line for the footer, 0 = none */
#endif

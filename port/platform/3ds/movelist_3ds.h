/* The 3DS app's bottom screen: move list + status (movelist_3ds.c). */
#ifndef PORT_MOVELIST_3DS_H
#define PORT_MOVELIST_3DS_H

int movelist_load(const char *dir);   /* dir/movelist.txt; 0 = loaded */
/* after C3D_Init: the bottom screen becomes a citro2d target (the text
 * console's output stops there; the log file keeps everything) */
int movelist_ui_init(void);
/* between the frame's draws and C3D_FrameEnd: redraws the bottom screen
 * when something changed; 1 if it drew (its vertices need the cache flush
 * of C3D_FrameEnd(0)) */
int movelist_render(void);
void movelist_update(void);           /* follow player 1's point character / team */
void movelist_redraw(void);
/* the status rows: a line of stats, and the touch toggles' states */
void movelist_status(const char *s, int frame_skip, int stretch);
enum { MOVELIST_TOGGLE_SKIP = 1, MOVELIST_TOGGLE_ASPECT = 2, MOVELIST_UPDATE_CHECK = 3, MOVELIST_UPDATE_INSTALL = 4,
       MOVELIST_TOGGLE_LOG = 5, MOVELIST_TOGGLE_UNLOCK = 6 };
/* the settings screen's logging and characters rows */
/* unlock_all: the setting; unlock_active: the mode this run plays (a change applies at the next start) */
void movelist_settings(int logging, int unlock_all, int unlock_active);
/* the settings screen's updater line (update_3ds.h state + text) */
void movelist_update_status(int state, const char *line);
/* a touch at (x, y) bottom-screen pixels: what main should do (or 0).  The
 * cog at the footer's left opens / closes the settings screen. */
int movelist_touch(int x, int y);

#endif

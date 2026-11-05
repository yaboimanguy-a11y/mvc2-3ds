/* The 3DS app's bottom screen: move list + status (movelist_3ds.c). */
#ifndef PORT_MOVELIST_3DS_H
#define PORT_MOVELIST_3DS_H

int movelist_load(const char *dir);   /* dir/movelist.txt; 0 = loaded */
/* after C3D_Init: the bottom screen becomes a citro2d target (the text
 * console's output stops there; the log file keeps everything) */
int movelist_ui_init(void);
/* between the frame's draws and C3D_FrameEnd: redraws the bottom screen
 * when something changed */
void movelist_render(void);
void movelist_update(void);           /* follow player 1's point character / team */
void movelist_redraw(void);
/* the status rows: a line of stats, and the touch toggles' states */
void movelist_status(const char *s, int frame_skip, int stretch);
enum { MOVELIST_TOGGLE_SKIP = 1, MOVELIST_TOGGLE_ASPECT = 2 };
/* a touch at (x, y) bottom-screen pixels: what main should toggle (or 0) */
int movelist_touch(int x, int y);

#endif

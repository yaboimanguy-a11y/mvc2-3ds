/* In-app updates from the GitHub releases (update_3ds.c). */
#ifndef PORT_UPDATE_3DS_H
#define PORT_UPDATE_3DS_H
#include <stddef.h>

enum {
    UPD_IDLE,        /* nothing asked yet */
    UPD_BUSY,        /* checking or downloading (background thread) */
    UPD_UPTODATE,    /* the latest release is this version (or older) */
    UPD_AVAILABLE,   /* a newer release: update_install() gets it */
    UPD_DONE,        /* installed: quit and start the game again */
    UPD_ERROR
};

const char *update_version(void);          /* this build's version, e.g. "0.2" */
void update_check(const char *app_dir);    /* starts a check (background) */
void update_install(void);                 /* after UPD_AVAILABLE: download and install */
/* the state and a line for the screen; *changed = 1 when it changed since the last call */
int update_state(char *msg, size_t n, int *changed);
void update_shutdown(void);
#endif

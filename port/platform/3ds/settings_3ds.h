/* The 3DS app's settings, kept in settings.txt in the app's folder. */
#ifndef PORT_SETTINGS_3DS_H
#define PORT_SETTINGS_3DS_H
typedef struct Settings3DS {
    int frame_skip;   /* 1: keep the game's speed by skipping drawing */
    int stretch;      /* 1: widescreen (stretched), 0: 4:3 */
    int logging;      /* 1: write mvc2_log.txt (off by default) */
    int unlock_all;   /* 1: every character / extra unlocked, 0: the game's own progression (shop) */
    int version;      /* of the file: older files get the new defaults (settings_load) */
} Settings3DS;
void settings_load(const char *dir, Settings3DS *s);  /* defaults where the file has nothing */
void settings_save(const char *dir, const Settings3DS *s);
#endif

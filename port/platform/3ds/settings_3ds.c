/* The 3DS app's settings (settings_3ds.h): "key=value" lines in
 * <app folder>/settings.txt, written whenever one changes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings_3ds.h"

void settings_load(const char *dir, Settings3DS *s)
{
    s->frame_skip = 1;
    s->stretch = 0;
    s->logging = 0;
    s->unlock_all = 1;
    s->version = 0;
    char path[300], line[96];
    snprintf(path, sizeof path, "%s/settings.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "frame_skip")) s->frame_skip = v != 0;
        else if (!strcmp(line, "widescreen")) s->stretch = v != 0;
        else if (!strcmp(line, "logging")) s->logging = v != 0;
        else if (!strcmp(line, "unlock_all")) s->unlock_all = v != 0;
        else if (!strcmp(line, "version")) s->version = v;
    }
    fclose(f);
    if (s->version < 2) {
        /* files of v0.2 test builds: frame skip on */
        s->frame_skip = 1;
    }
}

void settings_save(const char *dir, const Settings3DS *s)
{
    char path[300];
    snprintf(path, sizeof path, "%s/settings.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "version=2\nframe_skip=%d\nwidescreen=%d\nlogging=%d\nunlock_all=%d\n",
            s->frame_skip, s->stretch, s->logging, s->unlock_all);
    fclose(f);
}

/* Shared pieces of the 3DS programs (the boot test host_main_3ds.c and the
 * app main_3ds.c): the app folder, finding and opening the disc image, the
 * log file, timed boot. */
#ifndef PORT_SYS_3DS_H
#define PORT_SYS_3DS_H
#include <stddef.h>

#define SYS3DS_DEFAULT_DIR "sdmc:/3ds/mvc2"

/* The folder the .3dsx was started from (the Homebrew Launcher passes its
 * path as argv[0]); SYS3DS_DEFAULT_DIR when started some other way. */
void sys3ds_app_dir(int argc, char **argv, char *out, size_t n);

/* Find and open the disc image: argv[1] if given, else every .iso / .cue /
 * .bin in dir, preferred first, until one opens as the game disc.  Prints
 * what it found or why nothing opened.  0 = opened. */
int sys3ds_open_disc(int argc, char **argv, const char *dir);

/* Everything printed (stdout and stderr) also goes to dir/mvc2_log.txt. */
int sys3ds_log_open(const char *dir); /* tee + file; 0 = logging */
void sys3ds_log_tee(void);            /* stdout / stderr to the console (and the file when open) */
int sys3ds_log_file(const char *dir, int on); /* mvc2_log.txt on / off; 0 = done */
void sys3ds_log_flush(void);
void sys3ds_log_close(void);
/* 0: stdout / stderr go to the log file only (the bottom screen shows the
 * move list); sys3ds_con_print writes to the console directly */
void sys3ds_log_console(int on);
void sys3ds_con_print(const char *s);

/* mvc2_boot(1) with each step's time and disc reads printed; returns ms */
unsigned long sys3ds_boot_timed(void);
void sys3ds_print_read_stats(const char *what);

/* The EE's float semantics in hardware: FPSCR round toward zero, flush to
 * zero, default NaN (0x03C00000).  The other builds rewrite every float
 * operation into port/host/ps2_float.c helpers instead (MVC2_PS2_FLOAT).
 * Prints the FPSCR before and after. */
void sys3ds_fpu_ps2_mode(void);

/* START pressed since the last scan (scans input) */
int sys3ds_start_pressed(void);
/* "Press START to exit." and wait for it */
void sys3ds_wait_for_start(void);
#endif

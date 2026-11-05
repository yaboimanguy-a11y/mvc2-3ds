/* Function sampling profiler of the profiling build (prof_fn_3ds.c). */
#ifndef PORT_PROF_FN_3DS_H
#define PORT_PROF_FN_3DS_H

enum { PROF_PHASE_OTHER, PROF_PHASE_LOGIC, PROF_PHASE_GPUWAIT, PROF_PHASE_VBLANK, PROF_PHASE_SUBMIT, PROF_PHASE_N };

int prof_fn_start(void);          /* after the boot: the sampler thread (0 = running) */
void prof_fn_phase(int phase);    /* what the main thread does now */
void prof_fn_report(int reset);   /* the top functions to stdout (the log) */
void prof_fn_stop(void);

#endif

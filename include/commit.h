#ifndef MICROMODE_COMMIT_H
#define MICROMODE_COMMIT_H

#include "micromode.h"

/* Enqueue `mode` on the firing-gate dirty list. Every write that sets `fire_startup`,
   `fire_reset`, `entered_this_commit` or `entry_kind` must call this, or the next tag's
   clearing pass will not reach the mode and the gate stays open. */
void micromode_mark_firing_gate_dirty(lf_micromode_program_t* program, lf_mode_t* mode);

#endif /* MICROMODE_COMMIT_H */
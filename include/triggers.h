#ifndef MICROMODE_TRIGGER_H
#define MICROMODE_TRIGGER_H

#include "micromode.h"

/* Seed `was_effectively_active` on every slot in the program. Run once, from validation,
   every later refresh is incremental, via micromode_settle_gate_changes. */
void micromode_snapshot_gates(lf_micromode_program_t* program);

/* Refresh `was_effectively_active` on every slot of every mode whose gate changed this
   commit, then empty the list. Restores the invariant the trigger lifecycle depends on:
   outside lf_micromode_on_tag_final, a slot's was_effectively_active equals its mode's
   effectively_active. */
void micromode_settle_gate_changes(lf_micromode_program_t* program);

/* Suspend, reset or resume the triggers of every mode left or entered this commit. */
void micromode_apply_trigger_lifecycle(lf_micromode_program_t* program);

#endif /* MICROMODE_TRIGGER_H */
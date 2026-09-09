#ifndef MICROMODE_TRIGGER_H
#define MICROMODE_TRIGGER_H

#include "micromode/micromode.h"

/** @brief Seed `was_effectively_active` on every slot in the program. Run once,
 *  from validation, every later refresh is incremental, inside
 *  micromode_apply_trigger_lifecycle. */
void micromode_snapshot_gates(lf_micromode_program_t* program);

/** @brief Apply the lifecycle of the triggers of every mode left or entered this
 *  commit, and empty the gate-change list, refreshing `was_effectively_active` on
 *  the way. That last part restores the invariant the lifecycle itself depends
 *  on: outside this call, a slot's `was_effectively_active` equals its mode's
 *  `effectively_active`. */
void micromode_apply_trigger_lifecycle(lf_micromode_program_t* program);

#endif /* MICROMODE_TRIGGER_H */
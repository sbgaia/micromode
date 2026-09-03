#ifndef MICROMODE_CORE_H
#define MICROMODE_CORE_H

#include "micromode.h"

/* The commit path's recompute: skips any state whose `active` set and whose parent gate
   are both unchanged since the last recompute. Same result as lf_micromode_recompute_gates, which stays a full pass for
   callers that mutate `active` directly. */
void micromode_recompute_gates_incremental(lf_micromode_program_t* program);

/* Re-derive `mode->is_pending` from its two lifecycle flags and keep the owning state's
   `npending`/`pending_one` in step. Must be called after every write to `needs_startup`
   or `needs_reset`. */
void micromode_update_pending(lf_mode_t* mode);

#endif /* MICROMODE_CORE_H */
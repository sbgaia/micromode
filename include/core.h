#ifndef MICROMODE_CORE_H
#define MICROMODE_CORE_H

#include "micromode/micromode.h"

#include "reactor-uc/connection.h"

/** @brief Recompute one mode's gate and enqueue it if it moved. NULL is accepted
 *  so callers can pass a state's `just_left`/`just_entered` without checking. */
static inline void micromode_recompute_one(lf_micromode_program_t* program, lf_mode_t* mode, bool parent_ok) {
  if (mode == NULL) {
    return;
  }
  const bool was = mode->effectively_active;
  mode->effectively_active = mode->active;
  if (!parent_ok) {
    mode->effectively_active = false;
  }
  // Only a mode that crossed the boundary can interest the trigger lifecycle, and
  // only such a mode's slots go stale against the invariant the settle pass restores.
  // The list is emptied at the end of the commit.
  if (mode->effectively_active != was && !mode->in_gate_changed_list) {
    mode->in_gate_changed_list = true;
    mode->gate_changed_next = program->gate_changed_head;
    program->gate_changed_head = mode;
  }
}

/** @brief Recompute one state's gates, enclosing-before-enclosed. The caller owes
 *  that order: a state's parent must have been recomputed already, or the parent
 *  gate read here is stale. Both callers walk `program->states`, which
 *  lf_micromode_validate_all enforces to be a preorder, so both get it for free.
 *
 *  If `incremental` is set, the pass skips a state whose `active` set and whose
 *  parent gate are both unchanged since the last recompute. Only the commit path
 *  may pass true. For callers that mutate `active` directly, the public entry
 *  point `lf_micromode_recompute_gates` promises a full recompute, so it passes
 *  false. */
static inline void micromode_recompute_state(lf_micromode_program_t* program, lf_mode_state_t* state,
                                             bool incremental) {
  bool parent_ok = true;
  if (state->parent_mode != NULL) {
    parent_ok = state->parent_mode->effectively_active;
  }

  // A gate is `active && parent_ok`, so with neither input changed nothing here can have
  // moved.
  if (incremental && parent_ok == state->last_parent_ok) {
    // With `parent_ok` unchanged only a mode whose own `active` moved can have changed,
    // and micromode_apply recorded the at most two it touched.
    if (state->applied_this_commit) {
      micromode_recompute_one(program, state->just_left, parent_ok);
      micromode_recompute_one(program, state->just_entered, parent_ok);
    }
    return;
  }
  state->last_parent_ok = parent_ok;
  for (size_t i = 0; i < state->nmodes; i++) {
    micromode_recompute_one(program, state->modes[i], parent_ok);
  }
}

/** @brief Re-derive `mode->is_pending` from its two lifecycle flags and keep the
 *  owning state's `npending`/`pending_one` in step. Must be called after every write
 *  to `needs_startup` or `needs_reset`. */
void micromode_update_pending(lf_mode_t* mode);

/** @brief The history storage of a mode that has any, else NULL. */
static inline lf_history_slot_t* micromode_hslots(lf_mode_t* mode) {
  return mode->kind == LF_MODE_KIND_HISTORY ? ((lf_history_mode_t*)mode)->hslots : NULL;
}

/** @brief The most events `trig` can have pending at once, and so the bound on the
 *  take that banks them. The per-slot capacity is computed by the max_pending_events
 *  of the trigger, so that we have enough storage to hold every event the trigger
 *  can ever have pending when the mode is suspended. */
static inline size_t micromode_saved_bound(const Trigger* trig) {
  validate(trig->type == TRIG_ACTION || trig->type == TRIG_CONN_DELAYED);
  return trig->type == TRIG_ACTION ? ((const Action*)trig)->max_pending_events
                                   : ((const DelayedConnection*)trig)->max_pending_events;
}

#endif /* MICROMODE_CORE_H */
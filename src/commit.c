#include "commit.h"
#include "core.h"
#include "triggers.h"

#include <string.h>

/* The deferred commit pass. lf_micromode_set_mode only records. */
void lf_micromode_set_mode(lf_mode_state_t* state, lf_mode_t* target, lf_mode_change_t kind) {
  validate(state);
  validate(target);
  validate(target->owner == state); // a non-sibling target is a bug
  validate(kind == LF_MODE_RESET || kind == LF_MODE_HISTORY);
  // Record only. Last call within a tag wins, per reactor. The commit happens in
  // on_tag_final. Nothing here may touch a gate, or a switch would take effect
  // mid-tag and Scheduler_run_timestep's stability assert would fire.
  state->next = target;
  state->next_change = kind;
}

/**
 * @brief Apply one state's pending change, or the cascade forced by an enclosing reset.
 *
 * Not recursive: on_tag_final walks `program->states` top down, so an enclosing mode has
 * already stamped its `entered_this_commit` and `entry_kind` before a contained state is
 * reached. The cascade is read UPWARD through `parent_mode`.
 */
static void micromode_apply(lf_micromode_program_t* program, lf_mode_state_t* state) {
  lf_mode_t* target = state->next;
  lf_mode_change_t kind = state->next_change;

  // A reset cascades into everything the entered mode contains, with the contained state
  // forced to its own initial mode, which is why an outer transition beats a
  // child's own lf_set_mode at the same tag. A history entry does not cascade: the
  // child keeps whatever configuration it had, and applies its own pending change instead.
  if (state->parent_mode != NULL && state->parent_mode->entered_this_commit &&
      state->parent_mode->entry_kind == LF_MODE_RESET) {
    target = state->initial;
    kind = LF_MODE_RESET;
  }

  state->next = NULL;
  state->next_change = LF_MODE_NONE;
  state->applied_this_commit = (target != NULL);
  state->just_left = NULL;
  state->just_entered = NULL;
  if (target == NULL) {
    return;
  }

  const bool is_reset = (kind == LF_MODE_RESET);
  lf_mode_t* leaving = state->current;
  if (leaving != NULL && leaving != target) {
    leaving->active = false;
    state->just_left = leaving;
  }

  target->active = true;
  state->just_entered = target;

  // Record what happened to `target` in THIS pass. The trigger lifecycle keys
  // on these two fields. Nothing else can distinguish "reset-entered now" from
  // "carries a sticky needs_reset from an entry made while its parent was
  // inactive", or represent a history entry at all.
  target->entered_this_commit = true;
  target->entry_kind = kind;
  micromode_mark_firing_gate_dirty(program, target);
  state->current = target;
  if (!target->had_startup) {
    target->needs_startup = true;
  }
  target->needs_reset = is_reset;
  micromode_update_pending(target);
}

/* Record that `mode` now carries firing-gate state, so the next tag knows to clear it
   without walking the program. Idempotent within a tag: a mode entered and then given a
   fire_startup in the same commit is enqueued once. */
void micromode_mark_firing_gate_dirty(lf_micromode_program_t* program, lf_mode_t* mode) {
  if (mode->in_dirty_list) {
    return;
  }
  mode->in_dirty_list = true;
  mode->dirty_next = program->dirty_head;
  program->dirty_head = mode;
}

/* Runs on every tag. A firing gate is written after tag t, read at every enqueue during
   t+1, and cleared here after t+1, never mid-tag. */
static void micromode_clear_firing_gates(lf_micromode_program_t* program) {
  lf_mode_t* mode = program->dirty_head;
  program->dirty_head = NULL;
  while (mode != NULL) {
    lf_mode_t* next = mode->dirty_next;
    mode->fire_startup = false;
    mode->fire_reset = false;
    mode->entered_this_commit = false;
    mode->entry_kind = LF_MODE_NONE;
    mode->in_dirty_list = false;
    mode->dirty_next = NULL;
    mode = next;
  }
}

static void micromode_apply_reset_vars(lf_micromode_program_t* program) {
  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    lf_mode_state_t* state_iter = program->states[state_idx];
    if (state_iter->npending == 0) {
      continue;
    }
    if (state_iter->npending == 1) {
      lf_mode_t* mode = state_iter->pending_one;
      if (mode != NULL && mode->needs_reset && mode->effectively_active) {
        for (size_t reset_idx = 0; reset_idx < mode->nreset_vars; reset_idx++) {
          memcpy(mode->reset_vars[reset_idx].target, mode->reset_vars[reset_idx].source,
                 mode->reset_vars[reset_idx].size);
        }
      }
      continue;
    }
    for (size_t mode_idx = 0; mode_idx < state_iter->nmodes; mode_idx++) {
      lf_mode_t* mode = state_iter->modes[mode_idx];
      if (!mode->needs_reset || !mode->effectively_active) {
        continue;
      }
      for (size_t reset_idx = 0; reset_idx < mode->nreset_vars; reset_idx++) {
        memcpy(mode->reset_vars[reset_idx].target, mode->reset_vars[reset_idx].source,
               mode->reset_vars[reset_idx].size);
      }
    }
  }
}

/**
 * @brief Fire a mode's pending startup or reset, scheduling its +1-microstep activation.
 *
 * The pass that calls this is conditional on a mode actually having a pending flag:
 * scheduling unconditionally would add a microstep reactor-c does not. The condition is over
 * `effectively_active` and not "changed this tag", because a mode entered while its
 * enclosing mode was inactive keeps its flag until the parent is re-entered.
 */
static void micromode_activate_one(lf_micromode_program_t* program, lf_mode_t* mode) {
  if (!mode->effectively_active || !(mode->needs_startup || mode->needs_reset)) {
    return;
  }
  mode->fire_startup = mode->needs_startup;
  mode->fire_reset = mode->needs_reset;
  micromode_mark_firing_gate_dirty(program, mode);
  if (mode->needs_startup) {
    mode->had_startup = true; // set here, not in a reaction body: a mode with no
                              // reaction(startup) still counts as having started.
  }
  mode->needs_startup = false;
  mode->needs_reset = false;
  micromode_update_pending(mode);

  if (mode->activation != NULL) {
    lf_ret_t ret = mode->activation->super.schedule(&mode->activation->super, 0, NULL);
    validate(ret == LF_OK || ret == LF_AFTER_STOP_TAG);
  }
}

/* A preorder walk over `program->states`, because the activation order is ancestor-first.
 * How much of each state is looked at varies: nothing at all when it has no pending mode, one mode when it has exactly
 * one, and the full index-ordered scan when it has more. */
static void micromode_schedule_activation_if_needed(lf_micromode_program_t* program) {
  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    lf_mode_state_t* state_iter = program->states[state_idx];
    if (state_iter->npending == 0) {
      continue;
    }
    if (state_iter->npending == 1) {
      lf_mode_t* mode = state_iter->pending_one;
      if (mode != NULL) {
        micromode_activate_one(program, mode);
      }
      continue;
    }
    for (size_t mode_idx = 0; mode_idx < state_iter->nmodes; mode_idx++) {
      micromode_activate_one(program, state_iter->modes[mode_idx]);
    }
  }
}

void lf_micromode_on_tag_final(void* ctx) {
  lf_micromode_program_t* program = (lf_micromode_program_t*)ctx;
  validate(program->nstates > 0);

  // Above the early return: clearing last tag's fire_startup/fire_reset must happen on
  // EVERY tag, not only on tags with a commit, or those gates stay open and the mode's
  // startup or reset reactions re-fire at every subsequent tag that triggers them.
  micromode_clear_firing_gates(program);

  bool any_pending = false;
  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    if (program->states[state_idx]->next != NULL) {
      any_pending = true;
      break;
    }
  }
  if (!any_pending) {
    return;
  }

  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    micromode_apply(program, program->states[state_idx]);
  }

  micromode_recompute_gates_incremental(program);
  micromode_apply_trigger_lifecycle(program);
  micromode_settle_gate_changes(program);
  micromode_apply_reset_vars(program);
  micromode_schedule_activation_if_needed(program);
}

void lf_micromode_on_tag_complete(void* state, const LfExtensionTagContext* context) {
  validate(state);
  validate(context);
  lf_micromode_program_t* program = (lf_micromode_program_t*)state;

  validate(program->nstates > 0);
  validate(program->states[0]->env == context->environment);
  if (context->is_shutdown) {
    // A shutdown reaction may request a transition, but shutdown has no following logical
    // tag at which its activation could run. Keep that transition deliberately uncommitted.
    return;
  }
  lf_micromode_on_tag_final(program);
}

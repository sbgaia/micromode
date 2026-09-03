#include "core.h"
#include "commit.h"
#include "triggers.h"

#include "reactor-uc/connection.h"
#include "reactor-uc/timer.h"

lf_ret_t lf_micromode_abi_check(void) { return LF_OK; }

const LfExtensionDescriptor lf_micromode_extension_descriptor = {
    .api_version = LF_RUNTIME_EXTENSION_API_VERSION,
    .struct_size = sizeof(LfExtensionDescriptor),
    .required_capabilities = LF_EXTENSION_CAP_TAG_COMPLETE | LF_EXTENSION_CAP_SHUTDOWN |
                             LF_EXTENSION_CAP_TRIGGER_BINDING | LF_EXTENSION_CAP_COMPOSABLE_GATES,
    .name = "org.lf-lang.micromode",
    .on_tag_complete = lf_micromode_on_tag_complete,
    .on_shutdown = lf_micromode_on_shutdown,
};

void lf_micromode_state_ctor(lf_mode_state_t* self, Environment* env, lf_mode_t** modes, size_t nmodes,
                             lf_mode_t* initial, lf_mode_t* parent_mode) {
  validate(env);
  validate(modes);
  validate(nmodes > 0);
  validate(initial);
  self->env = env;
  self->modes = modes;
  self->nmodes = nmodes;
  self->initial = initial;
  self->current = initial;
  self->next = NULL;
  self->next_change = LF_MODE_NONE;
  self->applied_this_commit = false;
  self->last_parent_ok = false;
  self->just_left = NULL;
  self->just_entered = NULL;
  self->npending = 0;
  self->pending_one = NULL;
  self->parent_mode = parent_mode;
}

void lf_micromode_mode_ctor(lf_mode_t* self, lf_mode_state_t* owner, Trigger** triggers, size_t ntriggers,
                            lf_trigger_slot_t* slots, const lf_reset_var_t* reset_vars, size_t nreset_vars,
                            LogicalAction* activation, Reaction** reactions, size_t nreactions) {
  validate(owner);
  self->owner = owner;
  self->active = (owner->initial == self);
  self->effectively_active = false;
  self->had_startup = false;
  self->needs_startup = false;
  self->needs_reset = false;
  self->fire_startup = false;
  self->fire_reset = false;
  self->entered_this_commit = false;
  self->entry_kind = LF_MODE_NONE;
  self->in_dirty_list = false;
  self->dirty_next = NULL;
  self->in_gate_changed_list = false;
  self->gate_changed_next = NULL;
  self->is_pending = false;
  self->triggers = triggers;
  self->ntriggers = ntriggers;
  self->slots = slots;
  // Only the volatile half. `saved` / `saved_cap` are caller-owned storage wired by codegen.
  if (slots != NULL) {
    for (size_t i = 0; i < ntriggers; i++) {
      slots[i].remaining = 0;
      slots[i].suspended_at = 0;
      slots[i].suspended = false;
      slots[i].was_effectively_active = false;
      slots[i].nsaved = 0;
      LfExtensionBinding_ctor(&slots[i].binding);
    }
  }
  self->reset_vars = reset_vars;
  self->nreset_vars = nreset_vars;
  self->activation = activation;
  self->reactions = reactions;
  self->nreactions = nreactions;
}

/* Recompute one mode's gate and enqueue it if it moved. NULL is accepted so callers can
   pass a state's `just_left`/`just_entered` without checking. */
static void micromode_recompute_one(lf_micromode_program_t* program, lf_mode_t* mode, bool parent_ok) {
  if (mode == NULL) {
    return;
  }
  const bool was = mode->effectively_active;
  mode->effectively_active = mode->active;
  if (!parent_ok) {
    mode->effectively_active = false;
  }
  if (mode->effectively_active != was && !mode->in_gate_changed_list) {
    mode->in_gate_changed_list = true;
    mode->gate_changed_next = program->gate_changed_head;
    program->gate_changed_head = mode;
  }
}

/**
 * @brief Recompute `effectively_active` for every mode of every state in `program`.
 *
 * Enclosing-before-enclosed, so a parent's value is final before a child reads it. The
 * `states` array supplies that order and lf_micromode_validate_all asserts it, which is
 * what lets a gate be one cached bool instead of a walk up the containment chain.
 */
static void micromode_recompute_gates_impl(lf_micromode_program_t* program, bool incremental) {
  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    lf_mode_state_t* state = program->states[state_idx];
    bool parent_ok = true;
    if (state->parent_mode != NULL) {
      parent_ok = state->parent_mode->effectively_active;
    }

    // A gate is `active && parent_ok`, so with neither input changed nothing here can have
    // moved. Only the commit path may skip: the public entry point promises to recompute
    // everything, for callers that set `active` directly. Both paths write `last_parent_ok`.
    if (incremental && parent_ok == state->last_parent_ok) {
      // With `parent_ok` unchanged only a mode whose own `active` moved can have changed,
      // and micromode_apply recorded the at most two it touched.
      if (state->applied_this_commit) {
        micromode_recompute_one(program, state->just_left, parent_ok);
        micromode_recompute_one(program, state->just_entered, parent_ok);
      }
      continue;
    }
    state->last_parent_ok = parent_ok;
    for (size_t i = 0; i < state->nmodes; i++) {
      lf_mode_t* mode = state->modes[i];
      const bool was = mode->effectively_active;
      mode->effectively_active = mode->active;
      if (!parent_ok) {
        mode->effectively_active = false;
      }
      // Only a mode that crossed the boundary can interest the trigger lifecycle, and only
      // such a mode's slots go stale against the invariant the settle pass restores.
      // Enqueue it once; the list is emptied at the end of the commit.
      if (mode->effectively_active != was && !mode->in_gate_changed_list) {
        mode->in_gate_changed_list = true;
        mode->gate_changed_next = program->gate_changed_head;
        program->gate_changed_head = mode;
      }
    }
  }
}

void lf_micromode_recompute_gates(lf_micromode_program_t* program) { micromode_recompute_gates_impl(program, false); }

void micromode_recompute_gates_incremental(lf_micromode_program_t* program) {
  micromode_recompute_gates_impl(program, true);
}

void micromode_update_pending(lf_mode_t* mode) {
  lf_mode_state_t* state = mode->owner;
  const bool pending = (mode->needs_startup || mode->needs_reset) != 0;
  if (pending == mode->is_pending) {
    return;
  }
  mode->is_pending = pending;
  if (pending) {
    state->npending++;
    state->pending_one = mode; // meaningful only while npending == 1
  } else {
    validate(state->npending > 0);
    state->npending--;
    if (state->pending_one == mode) {
      state->pending_one = NULL;
    }
  }
  // Recover the singleton when the count falls back to 1 from above: the two consumers
  // read `pending_one` only at npending == 1, so it has to be right there and nowhere else.
  if (state->npending == 1 && state->pending_one == NULL) {
    for (size_t i = 0; i < state->nmodes; i++) {
      if (state->modes[i]->is_pending) {
        state->pending_one = state->modes[i];
        break;
      }
    }
  }
}

lf_ret_t lf_micromode_validate_all(lf_micromode_program_t* program, Environment* env) {
  validate(program);
  validate(env);
  validate(program->states);
  validate(program->nstates > 0);
  // Seeded here
  program->dirty_head = NULL;
  program->gate_changed_head = NULL;

  // Validate the extension registration
  validate(program->ext.descriptor == &lf_micromode_extension_descriptor);
  validate(program->ext.state == program);

  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    lf_mode_state_t* state = program->states[state_idx];
    validate(state != NULL);
    validate(state->env == env);

    // Checks duplicates
    for (size_t other = state_idx + 1; other < program->nstates; other++) {
      validate(program->states[other] != state);
    }

    // The array must be a preorder  walk of the instantiation tree
    if (state->parent_mode != NULL) {
      const lf_mode_state_t* enclosing = state->parent_mode->owner;
      bool enclosing_on_rightmost_path = false;
      if (state_idx > 0) {
        const lf_mode_state_t* walk = program->states[state_idx - 1];
        for (size_t hops = 0; walk != NULL && hops <= state_idx; hops++) {
          if (walk == enclosing) {
            enclosing_on_rightmost_path = true;
            break;
          }
          if (walk->parent_mode == NULL) {
            break;
          }
          walk = walk->parent_mode->owner;
        }
      }
      validate(enclosing_on_rightmost_path);
    }

    validate(state->modes != NULL);
    validate(state->nmodes > 0);
    validate(state->initial != NULL);
    validate(state->current != NULL);
    validate(state->next == NULL);
    validate(state->next_change == LF_MODE_NONE);

    bool found_initial = false;
    bool found_current = false;
    for (size_t i = 0; i < state->nmodes; i++) {
      lf_mode_t* mode = state->modes[i];
      validate(mode != NULL);
      validate(mode->owner == state);
      if (mode == state->initial) {
        found_initial = true;
      }
      if (mode == state->current) {
        found_current = true;
      }
      for (size_t other = i + 1; other < state->nmodes; other++) {
        validate(state->modes[other] != mode);
      }
      validate(mode->nreset_vars == 0 || mode->reset_vars != NULL);
      for (size_t reset = 0; reset < mode->nreset_vars; reset++) {
        validate(mode->reset_vars[reset].target != NULL);
        validate(mode->reset_vars[reset].source != NULL);
        validate(mode->reset_vars[reset].size > 0);
      }

      // Without this, a mode wired with triggers but no slot array faults inside the
      // trigger lifecycle on some later tag, far from the wiring bug that caused it.
      if (mode->ntriggers > 0) {
        validate(mode->triggers != NULL);
        validate(mode->slots != NULL);
      }
      for (size_t ti = 0; ti < mode->ntriggers; ti++) {
        validate(mode->triggers[ti] != NULL);
        validate(mode->triggers[ti]->parent != NULL);
        validate(mode->triggers[ti]->parent->env == state->env);
        // Binding also establishes single ownership: a mode's list may carry its own
        // triggers and those of a contained non-modal reactor, never a contained modal
        // reactor's, whose triggers a history entry must not resume. A trigger claimed by
        // two modes (or listed twice) hits the duplicate-descriptor guard here.

        // Re-binding the same slot to the same trigger and mode is idempotent.
        validate(Trigger_bind_extension(mode->triggers[ti], &mode->slots[ti].binding,
                                        &lf_micromode_extension_descriptor, mode) == LF_OK);
        // Both kinds can hold events across tags, so both need suspension storage. A timer
        // does not: Timer_suspend owns a one-element buffer of its own.
        if (mode->triggers[ti]->type == TRIG_ACTION || mode->triggers[ti]->type == TRIG_CONN_DELAYED) {
          // Shared across both kinds: a slot that cannot be suspended at all is refused
          // regardless of what it holds.
          validate(mode->slots[ti].saved != NULL);

          // The bound is `max_pending_events`. An`event_bound` of 0 means
          // unbounded (SIZE_MAX) and is refused here, since no static buffer
          // can satisfy it.
          size_t bound = (mode->triggers[ti]->type == TRIG_ACTION)
                             ? ((Action*)mode->triggers[ti])->max_pending_events
                             : ((DelayedConnection*)mode->triggers[ti])->max_pending_events;
          validate(mode->slots[ti].saved_cap >= bound);
          // A mode's actions must be logical and gated.
          if (mode->triggers[ti]->type == TRIG_ACTION) {
            validate(((Action*)mode->triggers[ti])->type == LOGICAL_ACTION);
            validate(((Action*)mode->triggers[ti])->schedule == lf_micromode_action_schedule);
          } else {
            validate(((DelayedConnection*)mode->triggers[ti])->type == LOGICAL_CONNECTION);
          }
        } else {
          validate(mode->triggers[ti]->type == TRIG_TIMER);
          validate(LfGate_contains(&((Timer*)mode->triggers[ti])->gate, &mode->effectively_active));
        }
      }

      // Every reaction written inside a mode must be dispatched through one of THAT mode's
      // own four gate words.
      if (mode->nreactions > 0) {
        validate(mode->reactions != NULL);
      }
      for (size_t ri = 0; ri < mode->nreactions; ri++) {
        validate(mode->reactions[ri] != NULL);
        const LfGate* gate = &mode->reactions[ri]->gate;
        // An empty gate means "always enabled", which for a mode-scoped reaction is a
        // silent failure.
        validate(LfGate_contains(gate, &mode->effectively_active) || LfGate_contains(gate, &mode->had_startup) ||
                 LfGate_contains(gate, &mode->fire_startup) || LfGate_contains(gate, &mode->fire_reset));
      }
    }
    validate(found_initial);
    validate(found_current);
  }

  lf_micromode_recompute_gates(program);
  // Seed the slot invariant across the WHOLE program once. Every later refresh is
  // incremental, and a mode the first recompute did not change never reaches the
  // gate-change list, so its slots would otherwise never be initialized.
  micromode_snapshot_gates(program);
  program->gate_changed_head = NULL;
  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    lf_mode_state_t* state = program->states[state_idx];
    for (size_t i2 = 0; i2 < state->nmodes; i2++) {
      state->modes[i2]->in_gate_changed_list = false;
      state->modes[i2]->gate_changed_next = NULL;
    }
  }

  // Seed the startup lifecycle before Environment_start. An effectively active mode has
  // already started: its startup reactions hang off the environment's startup chain and fire
  // at (0,0), so it owes no activation.
  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    lf_mode_state_t* state = program->states[state_idx];
    for (size_t i = 0; i < state->nmodes; i++) {
      lf_mode_t* mode = state->modes[i];
      if (mode->effectively_active) {
        mode->fire_startup = true;
        micromode_mark_firing_gate_dirty(program, mode);
        mode->had_startup = true;
        mode->needs_startup = false;
        mode->needs_reset = false;
        micromode_update_pending(mode);
        continue;
      }
      mode->needs_startup = false;
      mode->needs_reset = false;
      micromode_update_pending(mode);
      // A mode that is not effectively active has its timers left unarmed by
      // Environment_start, so the lifecycle must treat it as already suspended. A timer
      // that was never armed has no recorded remaining delay, so it falls back to its
      // offset.
      for (size_t ti = 0; ti < mode->ntriggers; ti++) {
        mode->slots[ti].suspended = true;
        if (mode->triggers[ti]->type == TRIG_TIMER) {
          mode->slots[ti].remaining = ((Timer*)mode->triggers[ti])->offset;
        }
      }
    }
  }

  return Environment_register_extension(env, &program->ext);
}

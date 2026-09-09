#include "core.h"
#include "commit.h"
#include "triggers.h"

#include "reactor-uc/connection.h"
#include "reactor-uc/timer.h"

#include <stddef.h>

_Static_assert(offsetof(lf_history_mode_t, super) == 0,
               "lf_history_mode_t must begin with its lf_mode_t, or micromode_hslots' cast is undefined");

lf_ret_t lf_micromode_abi_check(void) { return LF_OK; }

const LfExtensionDescriptor lf_micromode_extension_descriptor = {
    .api_version = LF_RUNTIME_EXTENSION_API_VERSION,
    .struct_size = sizeof(LfExtensionDescriptor),
    .required_capabilities = LF_EXTENSION_CAP_TAG_COMPLETE | LF_EXTENSION_CAP_TAG_START | LF_EXTENSION_CAP_SHUTDOWN |
                             LF_EXTENSION_CAP_TRIGGER_BINDING | LF_EXTENSION_CAP_COMPOSABLE_GATES |
                             LF_EXTENSION_CAP_MICROSTEP_REQUEST,
    .name = "org.lf-lang.micromode",
    .on_tag_start = lf_micromode_on_tag_start,
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
  // All three stamped by lf_micromode_validate_all, the mandatory init entry 
  // point: none of them is knowable from a single state, and the constructor 
  // is not given the program.
  self->program = NULL;
  self->index = 0;
  self->subtree_end = 0;
}

void lf_micromode_mode_ctor(lf_mode_t* self, lf_mode_state_t* owner, Trigger** triggers, size_t ntriggers,
                            lf_trigger_slot_t* slots, const lf_reset_var_t* reset_vars, size_t nreset_vars,
                            LogicalAction* activation, Reaction** reactions, size_t nreactions) {
  validate(owner);
  self->kind = LF_MODE_KIND_PLAIN;
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
  if (slots != NULL) {
    for (size_t i = 0; i < ntriggers; i++) {
      slots[i].suspended = false;
      slots[i].was_effectively_active = false;
      LfExtensionBinding_ctor(&slots[i].binding);
    }
  }
  self->reset_vars = reset_vars;
  self->nreset_vars = nreset_vars;
  self->activation = activation;
  self->reactions = reactions;
  self->nreactions = nreactions;
}

void lf_micromode_history_mode_ctor(lf_history_mode_t* self, lf_mode_state_t* owner, Trigger** triggers,
                                    size_t ntriggers, lf_trigger_slot_t* slots, lf_history_slot_t* hslots,
                                    const lf_reset_var_t* reset_vars, size_t nreset_vars, LogicalAction* activation,
                                    Reaction** reactions, size_t nreactions) {
  lf_micromode_mode_ctor(&self->super, owner, triggers, ntriggers, slots, reset_vars, nreset_vars, activation,
                         reactions, nreactions);
  self->super.kind = LF_MODE_KIND_HISTORY;
  self->hslots = hslots;
  // Only the volatile half. `saved` is caller-owned storage wired by codegen.
  if (hslots != NULL) {
    for (size_t i = 0; i < ntriggers; i++) {
      hslots[i].remaining = 0;
      hslots[i].suspended_at = 0;
      hslots[i].nsaved = 0;
    }
  }
}

void lf_micromode_recompute_gates(lf_micromode_program_t* program) {
  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    micromode_recompute_state(program, program->states[state_idx], false);
  }
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
  program->pending_lo = program->nstates;
  program->pending_hi = 0;

  // Validate the extension registration
  validate(program->ext.descriptor == &lf_micromode_extension_descriptor);
  validate(program->ext.state == program);

  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    lf_mode_state_t* state = program->states[state_idx];
    validate(state != NULL);
    validate(state->env == env);
    // Stamped here rather than trusted from lf_micromode_state_ctor, which is not given the
    // program: a state constructed for one program and listed in another would otherwise
    // widen the wrong program's commit range.
    state->program = program;
    state->index = (uint32_t)state_idx;
    state->subtree_end = (uint32_t)(state_idx + 1); // widened below by everything it encloses

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
      // trigger lifecycle on some later tag, far from the wiring bug that caused it. The
      // third array is required of a history-enterable mode for the same reason. A plain
      // one has no field to hold it, so micromode_hslots returns NULL.
      lf_history_slot_t* hslots = micromode_hslots(mode);
      if (mode->ntriggers > 0) {
        validate(mode->triggers != NULL);
        validate(mode->slots != NULL);
        validate(mode->kind == LF_MODE_KIND_PLAIN || hslots != NULL);
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
        // Both trigger kinds can hold events across tags, so in a history-enterable mode
        // both need suspension storage. A timer does not.
        if (mode->triggers[ti]->type == TRIG_ACTION || mode->triggers[ti]->type == TRIG_CONN_DELAYED) {
          if (hslots != NULL) {
            // A trigger this mode's history entry would have to restore, but that its
            // suspension has no storage for, is a wiring bug. The trigger's own state 
            // is not enough.
            validate(hslots[ti].saved != NULL);

            // The bound on the take is `max_pending_events`. An `event_bound` of 0 means
            // unbounded (SIZE_MAX) and is refused here, since no static buffer can
            // satisfy it.
            validate(micromode_saved_bound(mode->triggers[ti]) != SIZE_MAX);
          }
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

      // Every reaction written inside a mode must be dispatched through one of that
      // mode's own four gate words.
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

  // Subtree extents. The array is a preorder (asserted above) so a state's descendants
  // occupy a contiguous run behind it, and the run ends at the highest index of 
  // any state that names it somewhere up its parent chain. Walking up from each 
  // state and stretching every ancestor to cover it is O(states * depth), paid 
  // once, at init.
  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    for (const lf_mode_t* enclosing = program->states[state_idx]->parent_mode; enclosing != NULL;
         enclosing = enclosing->owner->parent_mode) {
      enclosing->owner->subtree_end = (uint32_t)(state_idx + 1);
    }
  }

  lf_micromode_recompute_gates(program);
  // Seed the slot invariant across the whole program once. Every later refresh is
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

  // Seed the startup lifecycle before Environment_start. An effectively active mode 
  // has already started: its startup reactions hang off the environment's startup 
  // chain and fire at (0,0), so it owes no activation.
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
      lf_history_slot_t* hslots = micromode_hslots(mode);
      for (size_t ti = 0; ti < mode->ntriggers; ti++) {
        mode->slots[ti].suspended = true;
        if (hslots != NULL && mode->triggers[ti]->type == TRIG_TIMER) {
          hslots[ti].remaining = ((Timer*)mode->triggers[ti])->offset;
        }
      }
    }
  }

  return Environment_register_extension(env, &program->ext);
}

#include "triggers.h"

#include "reactor-uc/environment.h"
#include "reactor-uc/logging.h"
#include "reactor-uc/timer.h"

/* What happens to a mode's own timers and actions when it is left and re-entered.
 *
 * Two passes, with different keys and different domains.
 *
 *   SUSPEND runs over every mode, keyed on the effectively_active true -> false edge.
 *   That edge is the only record that a mode deactivated transitively was left 
 *   at all. Recompute_gates writes nothing else.
 *
 *   ENTER runs over modes with entered_this_commit && effectively_active, keyed on
 *   entry_kind, over that mode's own triggers only. Ownership, not the activation edge: a
 *   history entry of an enclosing mode must not resume a contained modal reactor's
 *   mode-local timers, which become active again without a transition of their own. A
 *   contained non-modal reactor's triggers live in the entered mode's list and are resumed.
 *
 *     entry_kind        timers                            actions
 *     ----------------  --------------------------------  ----------------------------
 *     LF_MODE_RESET     purge, then arm at `offset`       purge, incl. slot->saved
 *     LF_MODE_HISTORY   resume at the recorded remaining  restore slot->saved, shifted
 *                       (never armed: its own offset)     by now - slot->suspended_at
 */

/** The logical instant of the commit. */
static instant_t micromode_commit_time(const lf_mode_state_t* state) {
  Environment* env = state->env;
  return env->get_logical_time(env);
}

/**
 * @brief Refresh `was_effectively_active` on the slots of every mode whose gate moved this
 * commit, then empty the list. The pre-change value has to be stored rather than derived:
 * once the recompute has run, the old one is gone.
 */
void micromode_settle_gate_changes(lf_micromode_program_t* program) {
  lf_mode_t* mode = program->gate_changed_head;
  program->gate_changed_head = NULL;
  while (mode != NULL) {
    lf_mode_t* next = mode->gate_changed_next;
    for (size_t trig_idx = 0; trig_idx < mode->ntriggers; trig_idx++) {
      mode->slots[trig_idx].was_effectively_active = mode->effectively_active;
    }
    mode->in_gate_changed_list = false;
    mode->gate_changed_next = NULL;
    mode = next;
  }
}

/* The one-time whole-program form, run from validation. It is what establishes the
   invariant in the first place, for modes the first recompute never changed and which
   therefore never reach the gate-change list. */
void micromode_snapshot_gates(lf_micromode_program_t* program) {
  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    lf_mode_state_t* state_iter = program->states[state_idx];
    for (size_t mode_idx = 0; mode_idx < state_iter->nmodes; mode_idx++) {
      lf_mode_t* mode = state_iter->modes[mode_idx];
      for (size_t trig_idx = 0; trig_idx < mode->ntriggers; trig_idx++) {
        mode->slots[trig_idx].was_effectively_active = mode->effectively_active;
      }
    }
  }
}

/** Take every trigger of every just-left mode out of the event queue.
 *
 *  Walks the gate-change list rather than every mode in the program. Equivalent, given the
 *  invariant the settle pass maintains: outside this pass a slot's
 *  `was_effectively_active` equals its mode's `effectively_active`, so the key below can only match a mode whose gate this
 *  commit's recompute actually moved, and those are exactly the modes on the list. */
static void micromode_suspend_left_modes(lf_micromode_program_t* program) {
  for (lf_mode_t* mode = program->gate_changed_head; mode != NULL; mode = mode->gate_changed_next) {
    {
      lf_mode_state_t* state_iter = mode->owner;
      if (mode->effectively_active) {
        continue;
      }
      if (mode->ntriggers == 0) {
        continue;
      }
      instant_t now = micromode_commit_time(state_iter);
      for (size_t trig_idx = 0; trig_idx < mode->ntriggers; trig_idx++) {
        lf_trigger_slot_t* slot = &mode->slots[trig_idx];
        if (!slot->was_effectively_active) {
          // Already suspended on some earlier tag, or never active at all.
          continue; 
        }
        if (slot->suspended) {
          // Already suspended on an earlier tag: preserve that state, do not re-take.
          continue;
        }
        Trigger* trig = mode->triggers[trig_idx];
        if (trig->type == TRIG_TIMER) {
          // Timer_suspend takes AND discards in one call. 
          interval_t remaining = NEVER;
          lf_ret_t ret = Timer_suspend((Timer*)trig, &remaining);
          validate(ret == LF_OK || ret == LF_EVENT_NOT_FOUND);
          slot->remaining = remaining;
        } else {
          // A slot too small for the mode's pending events is a wiring bug, hence a
          // validate rather than an assert. lf_micromode_validate_all already refuses an
          // action slot with no storage at all, which is the common case.
          lf_ret_t ret = Trigger_take_pending(trig, slot->saved, slot->saved_cap, &slot->nsaved);
          validate(ret == LF_OK);
        }
        slot->suspended_at = now;
        slot->suspended = true;
      }
    }
  }
}

/**
 * @brief A reset-kind entry: purge, then arm.
 *
 * The purge matters for a SELF-reset, which targets an already-active mode. The suspend
 * pass never ran for it, so its events are still queued.
 */
static void micromode_reset_entry(Trigger* trig, lf_trigger_slot_t* slot) {
  // Order matters: a slot can hold BOTH events saved by an earlier suspension and events
  // still sitting in the event queue. Discard the saved pool first.
  if (slot->nsaved > 0) {
    (void)Trigger_discard_pending(trig, slot->saved, slot->nsaved);
    slot->nsaved = 0;
  }
  if (trig->type == TRIG_TIMER) {
    Timer* timer = (Timer*)trig;
    interval_t discarded = NEVER; // Timer_suspend's take-and-discard.
    lf_ret_t ret = Timer_suspend(timer, &discarded);
    validate(ret == LF_OK || ret == LF_EVENT_NOT_FOUND);
    Timer_arm(timer, timer->offset);
  } else {
    size_t taken = 0;
    lf_ret_t ret = Trigger_take_pending(trig, slot->saved, slot->saved_cap, &taken);
    validate(ret == LF_OK);
    (void)Trigger_discard_pending(trig, slot->saved, taken);
  }
  slot->suspended = false;
}

/** A history-kind entry: put back what was taken, at the delay it had left. */
static void micromode_history_entry(Trigger* trig, lf_trigger_slot_t* slot, instant_t now) {
  if (!slot->suspended) {
    return;
  }
  if (trig->type == TRIG_TIMER) {
    // For a mode that has never been effectively active we arm the timer at its own offset.
    if (slot->remaining != NEVER) {
      Timer_arm((Timer*)trig, slot->remaining);
    }
  } else if (slot->nsaved > 0) {
    // due + (now - suspended_at) == now + remaining, i.e. LF history semantics. 
    (void)Trigger_restore_pending(trig, slot->saved, slot->nsaved, now - slot->suspended_at);
    slot->nsaved = 0;
  }
  slot->suspended = false;
}

/** Give every just-entered mode back its own triggers, per entry_kind.
 *
 *  Walks the firing-gate dirty list rather than every mode in the program. Equivalent
 *  here: the only condition this pass acts on is `entered_this_commit`, which is set
 *  exclusively by micromode_apply and cleared again every tag, so it is never 
 *  sticky, and a mode absent from the list provably has it false. 
 *  (The reset-var pass deliberately does NOT do this: it keys on `needs_reset`,
 *  which IS sticky across tags for a mode entered while its parent was inactive, 
 *  so a list built from this tag's entries would miss it.) */
static void micromode_resume_entered_modes(lf_micromode_program_t* program) {
  for (lf_mode_t* mode = program->dirty_head; mode != NULL; mode = mode->dirty_next) {
    {
      lf_mode_state_t* state_iter = mode->owner;
      if (!mode->entered_this_commit || !mode->effectively_active) {
        continue;
      }
      if (mode->ntriggers == 0) {
        continue;
      }
      instant_t now = micromode_commit_time(state_iter);
      for (size_t trig_idx = 0; trig_idx < mode->ntriggers; trig_idx++) {
        Trigger* trig = mode->triggers[trig_idx];
        lf_trigger_slot_t* slot = &mode->slots[trig_idx];
        if (mode->entry_kind == LF_MODE_RESET) {
          micromode_reset_entry(trig, slot);
        } else {
          micromode_history_entry(trig, slot, now);
        }
      }
    }
  }
}

void micromode_apply_trigger_lifecycle(lf_micromode_program_t* program) {
  micromode_suspend_left_modes(program);
  micromode_resume_entered_modes(program);
}

static const lf_mode_t* micromode_owning_mode(const Action* action) {
  return (const lf_mode_t*)Trigger_extension_state(&action->super, &lf_micromode_extension_descriptor);
}

lf_ret_t lf_micromode_action_schedule(Action* self, interval_t offset, const void* value) {
  validate(self->type == LOGICAL_ACTION);

  const lf_mode_t* mode = micromode_owning_mode(self);
  validate(mode != NULL);

  if (mode->effectively_active) {
    return Action_schedule(self, offset, value);
  }

  LF_WARN(TRIG, "Dropping a schedule on mode-local action %p: its mode is not active", (void*)self);
  return LF_OK;
}

void lf_micromode_on_shutdown(void* state, Environment* environment) {
  validate(state);
  lf_micromode_program_t* program = (lf_micromode_program_t*)state;
  validate(program->nstates > 0);
  validate(program->states[0]->env == environment);

  for (size_t state_idx = 0; state_idx < program->nstates; state_idx++) {
    lf_mode_state_t* state_iter = program->states[state_idx];
    for (size_t mode_idx = 0; mode_idx < state_iter->nmodes; mode_idx++) {
      lf_mode_t* mode = state_iter->modes[mode_idx];
      for (size_t trigger_idx = 0; trigger_idx < mode->ntriggers; trigger_idx++) {
        lf_trigger_slot_t* slot = &mode->slots[trigger_idx];
        if (slot->nsaved > 0) {
          validate(Trigger_discard_pending(mode->triggers[trigger_idx], slot->saved, slot->nsaved) == LF_OK);
          slot->nsaved = 0;
        }
        slot->suspended = false;
      }
    }
  }
}

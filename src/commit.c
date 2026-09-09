#include "commit.h"
#include "core.h"
#include "triggers.h"

#include "reactor-uc/environment.h"
#include "reactor-uc/reaction.h"
#include "reactor-uc/scheduler.h"

#include <string.h>

// The deferred commit pass. lf_micromode_set_mode only records.
void lf_micromode_set_mode(lf_mode_state_t* state, lf_mode_t* target, lf_mode_change_t kind) {
  validate(state);
  validate(target);
  validate(target->owner == state); // a non-sibling target is a bug
  validate(kind == LF_MODE_RESET || kind == LF_MODE_HISTORY);
  // A history transition into a mode with no history storage would silently
  // restore nothing (This should be a compile error, here just to be safe).
  validate(kind != LF_MODE_HISTORY || target->kind == LF_MODE_KIND_HISTORY);
  // lf_micromode_validate_all was never run over this state
  validate(state->program != NULL);

  // Widen the commit's walk to this state's whole subtree, which is the entire reach
  // of the reset cascade this may turn into. Idempotent, so "last call wins" needs
  // no special case.
  lf_micromode_program_t* program = state->program;
  if (state->index < program->pending_lo) {
    program->pending_lo = state->index;
  }
  if (state->subtree_end > program->pending_hi) {
    program->pending_hi = state->subtree_end;
  }
  state->next = target;
  state->next_change = kind;
}

/**
 * @brief Apply one state's pending change, or the cascade forced by an enclosing reset.
 *
 * Not recursive: on_tag_final walks `program->states` top down, so an enclosing mode has
 * already stamped its `entered_this_commit` and `entry_kind` before a contained state is
 * reached. The cascade is read upward through `parent_mode`.
 */
static void micromode_apply(lf_micromode_program_t* program, lf_mode_state_t* state) {
  lf_mode_t* target = state->next;
  lf_mode_change_t kind = state->next_change;

  // A reset cascades into everything the entered mode contains, with the contained state
  // forced to its own initial mode, which is why an outer transition beats a
  // child's own lf_set_mode at the same tag. A history entry does not cascade: the
  // child keeps whatever configuration it had, and applies its own pending
  // change instead.
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

  // Record what happened to `target` in this pass. The trigger lifecycle keys
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

// Mark a mode as having a dirty firing gate, so it will be cleared on the next tag.
void micromode_mark_firing_gate_dirty(lf_micromode_program_t* program, lf_mode_t* mode) {
  if (mode->in_dirty_list) {
    return;
  }
  mode->in_dirty_list = true;
  mode->dirty_next = program->dirty_head;
  program->dirty_head = mode;
}

// Runs on every tag, after the commit pass, to clear the firing-gate flags of every
// mode that was left or entered this tag.
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

// Restore the values of every variable into its reset value.
static void micromode_reset_vars_of(lf_mode_t* mode) {
  if (!mode->needs_reset || !mode->effectively_active) {
    return;
  }
  for (size_t reset_idx = 0; reset_idx < mode->nreset_vars; reset_idx++) {
    memcpy(mode->reset_vars[reset_idx].target, mode->reset_vars[reset_idx].source, mode->reset_vars[reset_idx].size);
  }
}

// Apply the reset values of every variable in every mode that has a pending reset. The
// pass that calls this is conditional on a mode actually having a pending reset: applying
// the reset values unconditionally would be a waste of time, and would also overwrite
// values that a mode's own reset reactions may have written.
static void micromode_apply_reset_vars(lf_mode_state_t* state) {
  if (state->npending == 0) {
    return;
  }
  if (state->npending == 1) {
    if (state->pending_one != NULL) {
      micromode_reset_vars_of(state->pending_one);
    }
    return;
  }
  for (size_t mode_idx = 0; mode_idx < state->nmodes; mode_idx++) {
    micromode_reset_vars_of(state->modes[mode_idx]);
  }
}

/**
 * @brief Fire a mode's pending startup or reset, arranging the +1-microstep tag it lands on.
 *
 * The pass that calls this is conditional on a mode actually having a pending flag.
 * The condition is over `effectively_active` and not "changed this tag", because a
 * mode entered while its enclosing mode was inactive keeps its flag until the parent
 * is re-entered.
 */
static void micromode_activate_one(lf_micromode_program_t* program, lf_mode_t* mode, bool* needs_microstep,
                                   bool* scheduled) {
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
    if (mode->activation->super.effects.size > 0) {
      // Carries a contained non-modal reactor's lifecycle reactions. 
      lf_ret_t ret = mode->activation->super.schedule(&mode->activation->super, 0, NULL);
      validate(ret == LF_OK || ret == LF_AFTER_STOP_TAG);
      if (ret == LF_OK) {
        *scheduled = true;
      }
    } else {
      // Effect-free: nothing is delivered through it but it still creates the 
      // +1-microstep tag. The scheduler will not create a microstep if there is no
      // effect-free activation scheduled, so this is necessary to advance the scheduler 
      // to the next microstep. The +1-microstep tag is created only once per tag, even 
      // if multiple modes schedule their effect-free activation, so this is idempotent.
      *needs_microstep = true;
    }
  }
}

/**
 * @brief The main function for activating a mode state.
 */
static void micromode_activate_state(lf_micromode_program_t* program, lf_mode_state_t* state, bool* needs_microstep,
                                     bool* scheduled) {
  if (state->npending == 0) {
    return;
  }
  if (state->npending == 1) {
    if (state->pending_one != NULL) {
      micromode_activate_one(program, state->pending_one, needs_microstep, scheduled);
    }
    return;
  }
  for (size_t mode_idx = 0; mode_idx < state->nmodes; mode_idx++) {
    micromode_activate_one(program, state->modes[mode_idx], needs_microstep, scheduled);
  }
}

/**
 * @brief Apply every recorded mode change.
 *
 * ONE walk over `program->states`, not five. Apply, recompute, reset-vars and activation used
 * to be four separate whole-program passes behind a fifth that scanned for anything to do, so
 * a committing tag touched every state four to five times over. They fuse because each step
 * needs only its OWN state's earlier steps, plus its PARENT's -- and the array is a preorder,
 * so a parent is finished before its children are reached either way:
 *
 *   apply        reads the parent MODE's `entered_this_commit` / `entry_kind`  (parent applied)
 *   recompute    reads the parent MODE's `effectively_active`             (parent recomputed)
 *   reset-vars   reads its own modes' `needs_reset` / `effectively_active`  (own recompute)
 *   activation   reads the same two, and clears `needs_*`                    (own reset-vars)
 *
 * The trigger lifecycle stays a pass of its own: it walks the two intrusive lists, which are
 * complete only once the loop is.
 *
 * ONE ORDERING CONSEQUENCE, and it is an improvement. Both the apply step and the activation
 * step push onto the firing-gate dirty list, which lf_micromode_on_tag_start reads back in
 * reverse to get descendant-first delivery. Fusing interleaves those pushes by state index
 * instead of running all of one pass's before all of the other's, so the list is now an exact
 * reverse preorder where it used to be reverse preorder within each pass. For a cascade -- the
 * shape the guarantee is written for, and the shape every test pins -- the two are identical,
 * since apply pushes every mode the cascade enters and activation finds them already listed.
 * They differ only between states with no containment relation, where descendant-first
 * constrains nothing and the array order is the better answer of the two.
 */
/**
 * @brief Whether the fused walk has anything to do for this state.
 *
 * Three ways a state can be involved in a commit, and no fourth:
 *
 *   1. it has a transition of its own recorded by lf_micromode_set_mode;
 *   2. the mode enclosing it was RESET-entered, which forces this state to its initial mode
 *      whatever it wanted -- the cascade, read upward exactly as micromode_apply reads it;
 *   3. the gate of the mode enclosing it moved, so every mode here changes
 *      `effectively_active` with it.
 *
 * Everything else the commit does is downstream of one of those. In particular a STICKY
 * `needs_startup`/`needs_reset` -- carried by a mode entered while its parent was inactive,
 * and fired later at a tag where its own state has no transition (oracle C10) -- cannot be
 * missed: a mode becomes effectively active only when its own `active` moves (case 1 or 2) or
 * when its parent's gate moves (case 3), so the tag that finally owes it an activation is
 * always a tag this answers true on.
 *
 * The parent's three fields are all final by the time a child is asked about, because
 * `program->states` is a preorder and the walk is forward.
 *
 * This does NOT make the walk O(states affected) -- it still loads every state to ask. What it
 * removes is the work for a state that has nothing to do, which was ~28 Ir and is now ~5.
 */

 /**
 * @brief Whether the walk has anything to do for this state.
 * Three ways a state can be involved in a commit:
 *   1. it has a transition of its own recorded by lf_micromode_set_mode;
 *   2. the mode enclosing it was RESET-entered, which forces this state to its initial
 *      mode;
 *   3. the gate of the mode enclosing it moved, so every mode here change 
 *      `effectively_active` with it.
 * Everything else the commit does is downstream of one of those. 
 */
static inline bool micromode_state_has_work(const lf_mode_state_t* state) {
  if (state->next != NULL) {
    return true;
  }
  const lf_mode_t* parent = state->parent_mode;
  if (parent == NULL) {
    return false;
  }
  if (parent->entered_this_commit && parent->entry_kind == LF_MODE_RESET) {
    return true;
  }
  return parent->effectively_active != state->last_parent_ok;
}

void lf_micromode_on_tag_final(void* ctx) {
  lf_micromode_program_t* program = (lf_micromode_program_t*)ctx;
  validate(program->nstates > 0);

  // Above the early return: clearing last tag's fire_startup/fire_reset must happen on
  // every tag, not only on tags with a commit, or those gates stay open and the mode's
  // startup or reset reactions re-fire at every subsequent tag that triggers them.
  micromode_clear_firing_gates(program);

  if (program->pending_lo >= program->pending_hi) {
    return;
  }

  bool needs_microstep = false;
  bool scheduled = false;
  for (size_t state_idx = program->pending_lo; state_idx < program->pending_hi; state_idx++) {
    lf_mode_state_t* state = program->states[state_idx];
    if (!micromode_state_has_work(state)) {
      continue;
    }
    micromode_apply(program, state);
    micromode_recompute_state(program, state, true);
    micromode_apply_reset_vars(state);
    micromode_activate_state(program, state, &needs_microstep, &scheduled);
  }

  program->pending_lo = program->nstates;
  program->pending_hi = 0;

  // Suspends what was left, resumes what was entered, and restores the slot invariant.
  micromode_apply_trigger_lifecycle(program);

  // An effect-carrying activation already put an event at (t, N+1), so the tag exists and
  // asking for it again would be redundant.
  if (needs_microstep && !scheduled) {
    Scheduler* scheduler = program->states[0]->env->scheduler;
    lf_ret_t ret = scheduler->request_next_microstep(scheduler);
    validate(ret == LF_OK || ret == LF_AFTER_STOP_TAG);
  }
}

/** @brief Deliver this tag's entry reactions, innermost mode first. 
 *  
 *  The firing-gate dirty list is descendant-first, so the walk is in the order 
 *  of the reactions themselves. The reactions are dispatched through the gate words, 
 *  so a reaction is delivered only if the gate that opened it is still open. A mode's
 *  gate may have closed since the commit, so a reaction that was scheduled through 
 *  it may not be delivered. 
 */
void lf_micromode_on_tag_start(void* state, const LfExtensionTagContext* context) {
  lf_micromode_program_t* program = (lf_micromode_program_t*)state;
  Scheduler* sched = context->environment->scheduler;

  for (lf_mode_t* mode = program->dirty_head; mode != NULL; mode = mode->dirty_next) {
    if (!mode->fire_startup && !mode->fire_reset) {
      continue;
    }

    const bool startup_open = mode->fire_startup;
    const bool reset_open = mode->fire_reset;
    for (size_t ri = 0; ri < mode->nreactions; ri++) {
      Reaction* reaction = mode->reactions[ri];
      if (!(startup_open && LfGate_contains(&reaction->gate, &mode->fire_startup)) &&
          !(reset_open && LfGate_contains(&reaction->gate, &mode->fire_reset))) {
        continue;
      }
      if (!Reaction_may_enqueue(reaction)) {
        continue;
      }
      validate(sched->add_to_reaction_queue(sched, reaction) == LF_OK);
    }
  }
}

void lf_micromode_on_tag_complete(void* state, const LfExtensionTagContext* context) {
  validate(state);
  validate(context);
  lf_micromode_program_t* program = (lf_micromode_program_t*)state;

  validate(program->nstates > 0);
  validate(program->states[0]->env == context->environment);
  if (context->is_shutdown) {
    // A shutdown reaction may request a transition, but shutdown has no following logical
    // tag at which its activation could run. Keep that transition deliberately 
    // uncommitted.
    return;
  }
  lf_micromode_on_tag_final(program);
}

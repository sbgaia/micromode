/**
 * @file micromode.h
 * @brief LF modal models for reactor-uc, implemented as a client of the generic
 * `LfExtension` mechanism. The runtime owns a gate; this library owns the policy.
 *
 * Built only when the `LF_MODAL_MODELS` option is on, which also requires reactor-uc's
 * `LF_RUNTIME_EXTENSIONS`.
 */
#ifndef MICROMODE_MICROMODE_H
#define MICROMODE_MICROMODE_H

#include "reactor-uc/error.h"
#include "reactor-uc/action.h"
#include "reactor-uc/extension.h"
#include "reactor-uc/tag.h"
#include "reactor-uc/trigger.h"

/** @brief Returns LF_OK. Exists so a test can prove the library was compiled and linked. */
lf_ret_t lf_micromode_abi_check(void);

typedef struct lf_mode lf_mode_t;
typedef struct lf_mode_state lf_mode_state_t;
typedef enum { LF_MODE_NONE = 0, LF_MODE_RESET, LF_MODE_HISTORY } lf_mode_change_t;

/** @brief One field of a mode's saved/restored state. Only the reset half is implemented:
 *  a reset entry copies `source` to `target`, a history entry does nothing. */
typedef struct {
  void* target;
  const void* source;
  size_t size;
} lf_reset_var_t;

/** @brief One trigger the mode owns, parallel to `lf_mode_t.triggers`: everything
 *  the trigger lifecycle needs to suspend that trigger and put it back.
 */
typedef struct {
  interval_t remaining;   // captured at suspension. For a timer never armed, its offset.
  instant_t suspended_at; // logical time of the suspension
  bool suspended;
  bool was_effectively_active; // captured at suspension, for a trigger that was effectively active but not armed
  size_t nsaved;               // number of saved events
  Event* saved;                // pointer to the saved events
  size_t saved_cap;            // max number of saved events (must be at least the trigger's `max_pending_events`)
  LfExtensionBinding binding;  // the binding that owns this trigger
} lf_trigger_slot_t;

struct lf_mode {
  bool effectively_active; // active AND every enclosing mode active.
  bool had_startup;        // ever activated.
  bool fire_startup;       // this mode's startup reactions run at the current tag
  bool fire_reset;         // this mode's reset reactions run at the current tag

  bool active;
  bool needs_startup;
  bool needs_reset;
  // Commit scratch, cleared for every mode at the top of each pass.
  bool entered_this_commit;
  lf_mode_change_t entry_kind;
  Trigger** triggers;
  size_t ntriggers;
  lf_trigger_slot_t* slots;
  const lf_reset_var_t* reset_vars;
  size_t nreset_vars;
  LogicalAction* activation; // per-mode 0-delay action driving startup/reset
  // Every reaction written inside this mode, for the init-time gate check in
  // lf_micromode_validate_all.
  Reaction** reactions;
  size_t nreactions;
  lf_mode_state_t* owner;

  /** @name Firing-gate dirty list
   *  Intrusive list of the modes a commit left with `fire_startup`, `fire_reset`,
   *  `entered_this_commit` or `entry_kind` set. Those four are the only per-tag state that
   *  has to be cleared afterwards, and only a handful of modes ever carry them, so the
   *  clearing pass walks this list instead of every mode in the program.  */
  bool in_dirty_list;
  lf_mode_t* dirty_next;

  /** @name Gate-change list
   *  Intrusive list of the modes whose `effectively_active` the current commit's gate
   *  recomputation actually changed. The trigger lifecycle acts only on modes that crossed
   *  the active/inactive boundary, and only those need their slots'
   *  `was_effectively_active` refreshed afterwards, so both walk this list rather than
   *  every mode in the program.*/
  bool in_gate_changed_list;
  lf_mode_t* gate_changed_next;

  // Whether this mode is counted in `lf_mode_state_t::npending`, i.e. whether it carries
  // `needs_startup` or `needs_reset`.
  bool is_pending;
};

struct lf_mode_state {
  lf_mode_t** modes;
  size_t nmodes;
  lf_mode_t* initial;
  lf_mode_t* current;
  lf_mode_t* next;
  lf_mode_change_t next_change;
  /** Whether the last commit actually entered a mode in this state, and what
   *  `parent_mode->effectively_active` was when this state's gates were last recomputed.
   *  Together they let the commit-path recompute skip a state's mode loop entirely: a
   *  mode's gate is `active && parent_ok`, so with neither input changed no gate in the
   *  state can have moved. */
  bool applied_this_commit;
  bool last_parent_ok;
  /** The two modes whose `active` the last commit actually moved.
   *  With `parent_ok` unchanged, they are the ONLY modes in this state whose
   *  gate can have moved, so recompute touches them instead of every mode here. Both NULL
   *  when the commit entered nothing. */
  lf_mode_t* just_left;
  lf_mode_t* just_entered;
  /** @name Pending-lifecycle bookkeeping
   *  How many of this state's modes carry `needs_startup` or `needs_reset`, and which one.
   *  The flags are STICKY across tags, which is why this is state the commit
   *  maintains rather than a list rebuilt per tag.
   */
  size_t npending;
  lf_mode_t* pending_one;
  lf_mode_t* parent_mode;
  Environment* env;
};

/** @brief One program's mode states, plus the single `LfExtension` that drives them.
 *
 *  `states` must be a PREORDER of the instantiation tree: each state immediately followed
 *  by everything it encloses, before any sibling.
 *
 *  Storage is caller-owned and declared by codegen, so there is no compile-time
 *  maximum on the number of modal reactors. `ext` must be constructed over
 *  `&lf_micromode_extension_descriptor` and over the program itself before `validate_all`.
 *  The callbacks cast their `void*` straight back to `lf_micromode_program_t*`.
 */
typedef struct {
  lf_mode_state_t** states;
  size_t nstates;
  LfExtension ext;
  /** Heads of the two intrusive mode lists, seeded by `lf_micromode_validate_all`. */
  lf_mode_t* dirty_head;
  lf_mode_t* gate_changed_head;
} lf_micromode_program_t;

/** @brief One `lf_set_mode` call, bound by `LF_SCOPE_MODE`: which reactor's
 *  mode state, which target mode within it, and which kind of transition. */
typedef struct {
  lf_mode_state_t* state;
  lf_mode_t* mode;
  lf_mode_change_t kind;
} lf_mode_target_t;

extern const LfExtensionDescriptor lf_micromode_extension_descriptor;

void lf_micromode_mode_ctor(lf_mode_t* self, lf_mode_state_t* owner, Trigger** triggers, size_t ntriggers,
                            lf_trigger_slot_t* slots, const lf_reset_var_t* reset_vars, size_t nreset_vars,
                            LogicalAction* activation, Reaction** reactions, size_t nreactions);
void lf_micromode_state_ctor(lf_mode_state_t* self, Environment* env, lf_mode_t** modes, size_t nmodes,
                             lf_mode_t* initial, lf_mode_t* parent_mode);

/** @brief Init-time validation over the whole micromode program. */
lf_ret_t lf_micromode_validate_all(lf_micromode_program_t* program, Environment* env);

/** @brief Recompute `effectively_active` for every mode of every state in `program`,
 *  enclosing-before-enclosed. */
void lf_micromode_recompute_gates(lf_micromode_program_t* program);

/** @brief Record a pending mode change for `state`. The next `lf_micromode_on_tag_final`
 *  applies it. Last call within one tag wins, per reactor, but a reset cascading down
 *  from an enclosing state overwrites a contained state's pending target unconditionally,
 *  so that rule does not hold across a containment boundary ("outer wins").
 *
 *  @param state The mode state the switch is a transition of.
 *  @param target The mode to switch to. Must satisfy `target->owner == state`. A
 *         non-sibling target is a bug.
 *  @param kind LF_MODE_RESET or LF_MODE_HISTORY. Never LF_MODE_NONE. */
void lf_micromode_set_mode(lf_mode_state_t* state, lf_mode_t* target, lf_mode_change_t kind);

/** @brief Apply every recorded mode change. `ctx` must be the `lf_micromode_program_t*`
 *  the extension was constructed over: the pass commits exactly the states that program
 *  lists, so a partial one produces a partial commit. */
void lf_micromode_on_tag_final(void* ctx);

/** Reactor-UC extension lifecycle callbacks. `state` is the `lf_micromode_program_t*`. */
void lf_micromode_on_tag_complete(void* state, const LfExtensionTagContext* context);
void lf_micromode_on_shutdown(void* state, Environment* environment);

/** @brief Binds one mode-transition target under its LF name, so a reaction body can write
 * `lf_set_mode(B)`. Emitted by ulfc beside the LF_SCOPE_* lines that bring
 * the reaction's ports, actions and timers into scope.
 *
 * The transition kind comes from the effect declaration (`-> reset(B)`), not the call, so
 * the binding is emitted here, before the opaque reaction body. */
#define LF_SCOPE_MODE(ModeName, State, Mode, Kind)                                                                     \
  lf_mode_target_t ModeName = {(State), (Mode), (Kind)};                                                               \
  (void)ModeName;

#define lf_set_mode(m) lf_micromode_set_mode((m).state, (m).mode, (m).kind)

/** @brief The `Action.schedule` replacement that `LF_MICROMODE_ACTION_GATE` installs on a
 *  mode-local action.
 *
 *  Drops the schedule when the owning mode is not effectively. Otherwise
 *  delegates to `Action_schedule` unchanged.
 *  Physical actions are rejected. */
lf_ret_t lf_micromode_action_schedule(Action* self, interval_t offset, const void* value);

/** @brief Install the mode gate on a mode-local action. Goes after `LF_INITIALIZE_ACTION`.
 *
 *  `Action.schedule` is a PER-INSTANCE function pointer, assigned by `Action_ctor`, so this
 *  one line intercepts `lf_schedule`, `lf_schedule_array` and asynchronous schedules
 *  uniformly. */
#define LF_MICROMODE_ACTION_GATE(ActionName) self->ActionName.super.super.schedule = lf_micromode_action_schedule

/** @brief Gate a trigger or reaction that lives in a CONTAINED reactor, addressed by a full
 *  path rather than by a name under `self`. */
#define LF_MICROMODE_CHILD_GATE(Path, GatePtr)                                                                         \
  validate(LfGate_add(&(Path).super.gate, &(Path).super._primary_gate, (GatePtr)) == LF_OK)

/** @brief Install the mode gate on a logical action that lives in a CONTAINED reactor.*/
#define LF_MICROMODE_CHILD_ACTION_GATE(Path) (Path).super.super.schedule = lf_micromode_action_schedule

#endif /* MICROMODE_MICROMODE_H */

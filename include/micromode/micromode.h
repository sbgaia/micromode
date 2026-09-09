/**
 * @file micromode.h
 * @brief LF modal models for reactor-uc, implemented as a client of the generic
 * `LfExtension` mechanism.
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

#include <stdint.h>

/** @brief Returns LF_OK. Exists so a test can prove the library was compiled and linked. */
lf_ret_t lf_micromode_abi_check(void);

typedef struct lf_mode lf_mode_t;
typedef struct lf_mode_state lf_mode_state_t;
typedef struct lf_micromode_program lf_micromode_program_t;
typedef enum { LF_MODE_NONE = 0, LF_MODE_RESET, LF_MODE_HISTORY } lf_mode_change_t;

/** @brief One field of a mode's saved/restored state. Only the reset half is implemented:
 *  a reset entry copies `source` to `target`, a history entry does nothing. */
typedef struct {
  void* target;
  const void* source;
  size_t size;
} lf_reset_var_t;

/** @brief The kind of a mode, which determines what storage it carries and
 *  what transitions it can be entered by. A plain mode has no history storage
 *  and can only be entered by a reset. A history mode has storage for each
 *  trigger, and can be entered by a reset or a history entry.
 */
typedef enum { LF_MODE_KIND_PLAIN = 0, LF_MODE_KIND_HISTORY } lf_mode_kind_t;

/** @brief Mode-owned trigger extension bindings. One per trigger, parallel to
 * `lf_mode_t.triggers`. The trigger lifecycle uses the `suspended` flag to key
 *  on the effectively_active true -> false edge, and the `was_effectively_active`
 *  flag to distinguish a trigger that was effectively active but not armed
 *  from one that was never active at all.
 */
typedef struct {
  LfExtensionBinding binding; // the binding that owns this trigger
  bool suspended;
  bool was_effectively_active; // captured at suspension, for a trigger that was effectively active but not armed
} lf_trigger_slot_t;

/** @brief The history state of a mode: where a suspended trigger's events go and
 *  how much delay each had left. A history mode has one of these per trigger.
 *  We dont model here a capacity field, but we constrain the number of saved events
 *  to the trigger's own `max_pending_events` (which is also the worst-case size
 *  codegen uses to allocate the `saved` array).
 */
typedef struct {
  interval_t remaining;   // captured at suspension. For a timer never armed, its offset.
  instant_t suspended_at; // logical time of the suspension
  size_t nsaved;          // number of saved events
  Event* saved;           // caller-owned storage, wired by codegen
} lf_history_slot_t;

struct lf_mode {
  // The mode's kind determines what storage it carries and what transitions
  // it can be entered by.
  lf_mode_kind_t kind;

  bool effectively_active; // active and every enclosing mode active.
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
   *  `entered_this_commit` or `entry_kind` set. Those are the only per-tag state
   *  variables that a commit writes, and the commit pass has to clear them all
   *  before the next tag.  */
  bool in_dirty_list;
  lf_mode_t* dirty_next;

  /** @name Gate-change list
   *  Intrusive list of the modes whose `effectively_active` the current commit's
   *  gate recomputation actually changed. The trigger lifecycle acts only on modes
   *  that crossed the active/inactive boundary, and only those need their slots'
   *  `was_effectively_active` refreshed afterwards, so both walk this list rather than
   *  every mode in the program.*/
  bool in_gate_changed_list;
  lf_mode_t* gate_changed_next;

  // Whether this mode is counted in `lf_mode_state_t::npending`, i.e. whether it carries
  // `needs_startup` or `needs_reset`.
  bool is_pending;
};

/** @brief A mode that stores the state of its triggers on suspension, allowing
 *  `history(m)` transitions, which restore the triggers' state to the stored
 *  one. `lf_history_slot_t` is the per-trigger storage, parallel to
 *  `lf_mode_t.triggers` and `lf_mode_t.slots`.
 */
typedef struct {
  lf_mode_t super;
  lf_history_slot_t* hslots;
} lf_history_mode_t;

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
   *  With `parent_ok` unchanged, they are the only modes in this state whose
   *  gate can have moved, so recompute touches them instead of every mode here. Both NULL
   *  when the commit entered nothing. */
  lf_mode_t* just_left;
  lf_mode_t* just_entered;
  /** @name Pending-lifecycle bookkeeping
   *  How many of this state's modes carry `needs_startup` or `needs_reset`, and
   *  which one. The flags are STICKY across tags, which is why this is state the commit
   *  maintains rather than a list rebuilt per tag.
   */
  size_t npending;
  lf_mode_t* pending_one;
  lf_mode_t* parent_mode;
  Environment* env;

  /** The program this state belongs to. This lets the commit pass reach the program's
   *  states, allowing `lf_micromode_set_mode` to widen the commit's index range from a call site that is handed only
   * the state. */
  lf_micromode_program_t* program;
  /** @name This state's own position in `program->states`, and one past the last
   *  state it encloses. The array is a preorder, so everything a state contains
   *  is contiguous, and `[index, subtree_end)` is the entire reach of
   *  a reset cascade rooted here (which is what lets a commit bound its walk instead of
   *  covering the program).
   */
  uint32_t index;
  uint32_t subtree_end;
};

/** @brief The micromode program extension state, which contains the mode states of
 *  every modal reactor in the program, plus the single `LfExtension` that drives
 *  them. The mode states must be a preorder of the instantiation tree: each state
 *  immediately followed by everything it encloses, before any sibling. Storage is
 *  provided externally (e.g., by the code generator), so there is no compile-time
 *  maximum on the number of modal reactors.
 */
typedef struct lf_micromode_program {
  lf_mode_state_t** states;
  size_t nstates;
  LfExtension ext;
  // Heads of the two intrusive mode lists, seeded by `lf_micromode_validate_all`.
  lf_mode_t* dirty_head;
  lf_mode_t* gate_changed_head;

  /** @name The commit's index range Half-open, over `states`. `lf_micromode_set_mode`
   *  widens it to cover the transitioning state's whole subtre
   *  `lf_micromode_on_tag_final` walks exactly it and then empties it again by
   *  setting `pending_lo = nstates, pending_hi = 0`, which is also how an empty
   *  pass is represented.
   */
  size_t pending_lo;
  size_t pending_hi;
} lf_micromode_program_t;

/** @brief One `lf_set_mode` call, bound by `LF_SCOPE_MODE`: which reactor's
 *  mode state, which target mode within it, and which kind of transition. */
typedef struct {
  lf_mode_state_t* state;
  lf_mode_t* mode;
  lf_mode_change_t kind;
} lf_mode_target_t;

extern const LfExtensionDescriptor lf_micromode_extension_descriptor;

/** @brief Construct a basic mode, which carries no suspend/restore. */
void lf_micromode_mode_ctor(lf_mode_t* self, lf_mode_state_t* owner, Trigger** triggers, size_t ntriggers,
                            lf_trigger_slot_t* slots, const lf_reset_var_t* reset_vars, size_t nreset_vars,
                            LogicalAction* activation, Reaction** reactions, size_t nreactions);

/** @brief Construct a history mode which carries suspend/restore storage and support
 *  for history transitions. */
void lf_micromode_history_mode_ctor(lf_history_mode_t* self, lf_mode_state_t* owner, Trigger** triggers,
                                    size_t ntriggers, lf_trigger_slot_t* slots, lf_history_slot_t* hslots,
                                    const lf_reset_var_t* reset_vars, size_t nreset_vars, LogicalAction* activation,
                                    Reaction** reactions, size_t nreactions);
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
 *  the extension was constructed over. The pass commits exactly the states that program
 *  lists, so a partial one produces a partial commit. */
void lf_micromode_on_tag_final(void* ctx);

/** Reactor-UC extension lifecycle callbacks. `state` is the `lf_micromode_program_t*`. */
void lf_micromode_on_tag_start(void* state, const LfExtensionTagContext* context);
void lf_micromode_on_tag_complete(void* state, const LfExtensionTagContext* context);
void lf_micromode_on_shutdown(void* state, Environment* environment);

/** @brief Binds one mode-transition target under its LF name, so a reaction body
 *  can write `lf_set_mode(B)`. Emitted by ulfc beside the LF_SCOPE_* lines that
 *  bring the reaction's ports, actions and timers into scope.
 *
 * The transition kind comes from the effect declaration (`-> reset(B)`). */
#define LF_SCOPE_MODE(ModeName, State, Mode, Kind)                                                                     \
  lf_mode_target_t ModeName = {(State), (Mode), (Kind)};                                                               \
  (void)ModeName;

/** @brief Binds a history transition target. Takes the `lf_history_mode_t` and
 *  reaches its base itself, so a mode the generator classified plain (which has
 *  no `.super`) fails to compile here instead of aborting at the first transition. */
#define LF_SCOPE_MODE_HISTORY(ModeName, State, HistoryMode)                                                            \
  lf_mode_target_t ModeName = {(State), &(HistoryMode)->super, LF_MODE_HISTORY};                                       \
  (void)ModeName;

/** @brief The mode-state bookkeeping every modal reactor's generated struct carries: the
 *  state itself and the array of pointers to its modes, which codegen populates in
 *  declaration order before LF_INITIALIZE_MODE_STATE runs. */
#define LF_MODE_BOOKKEEPING_INSTANCES(NumModes)                                                                        \
  lf_mode_state_t _mode_state;                                                                                         \
  lf_mode_t* _modes[NumModes]

/** @brief Constructs the mode state over the `_modes` array the generator has already
 *  populated. `Initial` is the address of the initial mode, `ParentMode` is NULL for a
 *  top-level modal reactor. */
#define LF_INITIALIZE_MODE_STATE(Initial, ParentMode)                                                                  \
  lf_micromode_state_ctor(&self->_mode_state, self->super.env, self->_modes,                                           \
                          sizeof(self->_modes) / sizeof(self->_modes[0]), (Initial), (ParentMode))

#define lf_set_mode(m) lf_micromode_set_mode((m).state, (m).mode, (m).kind)

/** @brief The `Action.schedule` replacement that `LF_MICROMODE_ACTION_GATE` installs
 *  on a mode-local action.
 *
 *  Drops the schedule when the owning mode is not effectively active. Otherwise
 *  delegates to `Action_schedule` unchanged. Currently physical actions are rejected. */
lf_ret_t lf_micromode_action_schedule(Action* self, interval_t offset, const void* value);

/** @brief Install the mode gate on a mode-local action. Goes after `LF_INITIALIZE_ACTION`.
 *
 *  `Action.schedule` is a per-instance function pointer, assigned by `Action_ctor`,
 *  so this one line intercepts `lf_schedule`, `lf_schedule_array` and
 *  asynchronous schedules uniformly. */
#define LF_MICROMODE_ACTION_GATE(ActionName) self->ActionName.super.super.schedule = lf_micromode_action_schedule

/** @brief Gate a trigger or reaction that lives in a contained reactor, addressed
 *  by a full path rather than by a name under `self`. */
#define LF_MICROMODE_CHILD_GATE(Path, GatePtr)                                                                         \
  validate(LfGate_add(&(Path).super.gate, &(Path).super._primary_gate, (GatePtr)) == LF_OK)

/** @brief Install the mode gate on a logical action that lives in a CONTAINED reactor.*/
#define LF_MICROMODE_CHILD_ACTION_GATE(Path) (Path).super.super.schedule = lf_micromode_action_schedule

#endif /* MICROMODE_MICROMODE_H */

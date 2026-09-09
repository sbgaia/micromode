# micromode

LF modal models for [reactor-uc](https://github.com/lf-lang/reactor-uc), implemented as a
client of the generic `LfExtension` mechanism. 

MicroMode performs no dynamic allocation. Mode state, trigger bindings, gate conditions,
reset images and suspended-event buffers are all embedded in generated reactor instances or
supplied by the caller with fixed capacity.

## Build

```bash
export REACTOR_UC_PATH=<path-to-reactor-uc>
make test
```

## Integrating it

Link consumers to the namespaced targets `reactor-uc::runtime` and `micromode::micromode`.
The unnamespaced names remain available for compatibility.

A consumer that adds both projects itself must `add_subdirectory` reactor-uc **before**
micromode, and must configure it with `LF_RUNTIME_EXTENSIONS`.
Micromode is designed as a client of reactor-uc's `LfExtension` mechanism, and
cannot compile without it.

## Behaviour notes

Behaviour was settled by measuring reactor-c on probe programs.
Three constructs diverge from reactor-c deliberately, each with a refusal or 
a comment explaining why:

- A **physical action** inside a mode is refused at init rather than gated.
- A **physical delayed connection** in a mode's trigger list is likewise 
refused, since restoring one of its events after a history entry has no defined 
meaning.
- a schedule onto a **mode-local action of an inactive mode** is dropped 
outright, where reactor-c admits the event, warns, and then never delivers it.
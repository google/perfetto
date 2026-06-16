# Declarative Slice Formatting from Typed Args

**Authors:** @LalitMaganti
**Status:** Draft
**PR:** N/A

## Problem

Trace producers frequently want to attach *dynamic context* to a slice's
display in the UI. A motivating example from Android's animation framework is a
slice that should read something like:

```
animator::View(0/content)::translationX
```

Here `animator` is the conceptually-static name of the trace point, while
`View`, `0/content` and `translationX` are runtime values describing *which*
view and *which* property is being animated.

Today producers achieve this by formatting the dynamic values directly into the
slice name string and emitting the result as the `TrackEvent` name. This
"JustWorks" visually but is actively harmful to everything downstream of
visualization. We refer to this as the **mangling problem**.

### 1. Analytics degradation (the mangling problem)

When the dynamic values are baked into the name, the name stops being a stable
identifier:

* SQL queries can no longer `GROUP BY name` or filter on a fixed string — every
  animated view produces a distinct name.
* Aggregations (e.g. "total time spent in `animator`") fragment into thousands
  of singleton buckets.
* Code search for the trace point breaks, because the literal that appears in
  the trace (`animator::View(0/content)::translationX`) does not appear anywhere
  in the source.

The correct mental model is that **the slice name must remain a static, fixed
identifier** (`animator`), and the dynamic values must live as structured
*arguments* (typed proto fields, surfaced as args in Trace Processor). Producers
are already encouraged to do exactly this via out-of-tree `TrackEvent` proto
extensions (see [RFC-0017][rfc0017]): instead of stringifying, you define a
proto message, emit typed fields, and Trace Processor turns them into queryable
args.

The tension is purely about **display**: keeping the name static means the UI no
longer shows the helpful dynamic context. We need a way to recover the rich
display *without* sacrificing the static identifier.

### 2. The UI-plugin bottleneck

Trace Processor already keeps the name static and exposes the dynamic values as
args. The UI *can* recombine them: `SliceTrackAttrs` exposes `sliceName(row)`
and `sliceSubtitle(row)` callbacks
(`ui/src/components/tracks/slice_track.ts`), and plugins like the CPU scheduling
track already hand-roll display strings from columns
(`ui/src/plugins/dev.perfetto.Sched/cpu_slice_track.ts`).

But this does not scale. It requires a developer to write and land custom
TypeScript **every time they add a new proto message**. Given the explicit goal
of RFC-0017 — letting teams add trace points by editing a `.proto` in their own
repo, with no Perfetto-side changes — requiring a matching UI plugin PR for each
one re-introduces exactly the friction RFC-0017 removed. The formatting needs to
be **declarative and seamless**: authored once, alongside the proto, with zero
per-message UI work.

### 3. Loss of argument provenance

A natural solution is to annotate the proto descriptor itself — e.g. a
message-level option such as:

```proto
message AnimatorInfo {
  option (perfetto.slice_print_format) = "{view}({target})::{property}";
  optional string view = 1;
  optional string target = 2;
  optional string property = 3;
}
```

The UI could then render the dynamic string automatically by applying the format
string to the args. The blocker is that **Trace Processor does not record which
proto descriptor produced a given arg set.**

Concretely:

* A slice's typed args are flattened into an arg set by the generic
  `ProtoToArgsParser`, and the resulting `arg_set_id` is a **content hash** with
  no provenance (`global_args_tracker.h`). It deliberately carries nothing about
  the originating message type.
* This is survivable on **process / async tracks**, where event sources stay
  reasonably segregated. It breaks on **thread tracks**, which multiplex
  `atrace`, `systrace`, `ftrace` and `TrackEvent` slices into a single stream.
  Given an arbitrary thread-track slice, there is currently no reliable way to
  say "these args came from `AnimatorInfo`" and therefore no way to know *which*
  format string to apply.

Recovering that descriptor → slice association is the core enabling work of this
RFC.

## Goal

Allow a trace producer to declare, **once, on their proto message**, how a slice
carrying that message should be formatted for display — with no per-message UI
code and no degradation of the underlying static name or args.

Non-goals:

* Changing the slice `name` column or the args. Both remain exactly as today;
  this is a **display-only** feature layered on top.
* Custom C++ parsing in Trace Processor (same out-of-scope stance as RFC-0017).

## Design

### The annotation

We add a **message-level** option to the proto descriptor language used by
out-of-tree extensions:

```proto
message AnimatorInfo {
  option (perfetto.slice_print_format) = "{view}({target})::{property}";
  optional string view = 1;
  optional string target = 2;
  optional string property = 3;
}
```

Rules for the format string:

* **`{key}` syntax.** Braces reference fields of the annotated message by name.
  We deliberately avoid `printf`-style `%d`/`%s`: positional specifiers require
  every referenced field to be present and ordered, whereas `{key}` degrades
  gracefully when an optional field is absent and is self-documenting.
* **Optional scalar leaf fields only.** A `{key}` may reference only an
  `optional` scalar field defined directly on the annotated message. Repeated
  fields and nested sub-messages are out of scope (their rendering semantics —
  joins, separators, recursion — are unbounded and not worth the complexity for
  v1).
* **No ambiguity within a message.** Because the option is attached to a
  specific message, `{view}` unambiguously means "field `view` of *this*
  message". The cross-message collisions one might worry about only arise when a
  single slice carries multiple annotated messages (see below).

### Display composition

The formatted string is rendered as a **grey subtitle alongside the slice**
(exact placement — beside vs. underneath the static name — is an implementation
detail to settle during build-out). The static `name` (`animator`) remains the
visually primary, unmodified title. Net display:

```
animator   View(0/content)::translationX      <- second part dimmed
```

Crucially, neither the `name` column nor the arg set is mutated. Analytics see
the static identifier and the structured args exactly as before.

### Multiple annotated messages on one slice

A single `TrackEvent` can carry more than one extension field, and more than one
of those payloads may declare `slice_print_format`. When that happens we make a
**deterministic single choice: the message reachable via the lowest extension
field number wins.** We do not concatenate — concatenation produces unbounded,
ill-ordered display strings and re-opens the mangling problem we are trying to
close. The losing messages' formats are simply ignored for display (their args
remain fully queryable).

### Retaining message options in Trace Processor

There is a concrete Trace Processor prerequisite: today the descriptor pool
**keeps field options but drops message options** (`util/descriptors.cc` retains
`FieldDescriptor::options_` as raw bytes; there is no equivalent for messages).
`slice_print_format` is a *message* option, so the pool must start retaining
message options (at least this one) when ingesting a `FileDescriptorSet`
delivered via `TracePacket.extension_descriptor` (RFC-0017 / the extensions
mechanism).

There is good precedent for plumbing option metadata through to the consumer:
field options already surface in `protozero_to_json` as `__field_options` (e.g.
`unit: ms_smallerIsBetter`).

### Recording descriptor provenance per slice

This is the heart of the work and the main **open question** (below). To apply
the right format string, something must record — at parse time, where the
extension descriptor is in hand (`track_event_event_importer`) — which message
type produced the typed args for each slice, so that the multiplexed-thread-track
case becomes resolvable. The candidate mechanisms (a column on the slice/event
row, a reserved arg, or a side table) are discussed under Open Questions; we
intentionally do **not** pick one in this RFC.

### Division of labour: Trace Processor vs UI

The descriptor pool — and therefore the format strings — live inside Trace
Processor (in the Wasm module the UI embeds). The UI cannot interpret a format
string on its own; it must go through TP. The leaning is that **Trace Processor
performs the full interpolation** and exposes the finished display string via a
SQL helper (e.g. `format_slice_display(<slice>)`), keeping all descriptor logic
in C++ and minimising UI code. Whether the UI instead needs the *structured*
pieces (e.g. to make `View` independently clickable) rather than a flat string is
an open question.

## Alternatives considered

### Stringify into the name (status quo)

Producers format dynamic values directly into the slice name.

**Pro:** Zero new machinery; works today.

**Con:** The mangling problem — destroys grouping, aggregation and code search.
This is precisely what the RFC exists to eliminate.

### UI plugin per message (status quo for "good" producers)

Keep the name static; write a `sliceName`/`sliceSubtitle` plugin to recombine
args for display.

**Pro:** Maximum flexibility; no proto/TP changes.

**Con:** Does not scale — a TypeScript PR per proto message, directly negating
the friction reduction RFC-0017 set out to achieve.

### `printf`-style format (`%d`/`%s`)

**Con:** Positional specifiers assume all referenced fields are present and
ordered; they degrade badly when an `optional` field is unset. `{key}` is more
robust and self-describing.

### Concatenate all annotated messages on a slice

**Con:** Unbounded, ill-ordered display strings; re-opens the mangling problem.
Lowest-field-number-wins is predictable and bounded.

## Open questions

* **(A) How does Trace Processor record per-slice descriptor provenance?**
  `arg_set_id` is a content hash and cannot carry it. Candidates: a new column on
  the slice/event row (written in `track_event_event_importer` where the
  descriptor is known); a reserved injected arg; or a side table
  `arg_set_id → message_type`. The same arg-set *content* can in principle come
  from two different messages, which argues against hanging it off the arg set —
  but this is explicitly left undecided.
* **(B) TP ↔ UI division of labour.** Does TP return just the format string, the
  format string plus resolved values, or the fully-interpolated display string?
  The leaning is full interpolation in TP, but the UI may need structured pieces
  (e.g. for linkable sub-parts).
* **Option naming.** `slice_print_format` vs `slice_print_arg` (the original
  strawman) vs something else.
* **Format-string error handling.** Missing/unset referenced field, type
  formatting (numbers, durations, units — note `unit` field options already
  exist), and how malformed format strings surface (silently dropped vs. stat /
  diagnostic).
* **Where the option is defined.** The `.proto` defining `slice_print_format`
  must be reachable by out-of-tree extension authors; how it composes with the
  RFC-0017 extension registry needs spelling out.

[rfc0017]: ./0017-out-of-tree-protos.md

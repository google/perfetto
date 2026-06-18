# Perfetto UI Review Antipatterns

This is a canonical catalogue of UI antipatterns and the preferred alternatives,
**mined from real code-review feedback** that maintainers repeatedly gave on 
`ui/src/` pull requests authored by other contributors.

**Purpose:** it is intended to be read by an AI agent (or human) *at code-review
or code-authoring time* so we can pre-empt the mistakes and invalid assumptions
that recur across contributions, instead of relitigating them PR by PR.

How to use it:

- Each item is phrased as **❌ antipattern → ✅ fix**, with a one-line *why* and
  one or more provenance PR numbers (`#NNNN`) you can look up for full context.
- 🔁 marks rules that recurred across **many** PRs — weight these most heavily;
  they are the maintainers' strongest, most-repeated preferences.
- This complements (and goes deeper than) the "Code Review Pet Peeves" section
  in [`AGENTS-ui.md`](AGENTS-ui.md). When they overlap, both are authoritative.

> Meta-principle that underlies almost everything below: **don't over-engineer,
> reuse what exists, keep state in one place, and make failures visible.** When
> in doubt, prefer the simpler, smaller, more boring change.

---

## 1. Mithril & Rendering

- 🔁 **❌ Calling `m.redraw()` / `raf.scheduleFullRedraw()` manually → ✅ rely on
  autoredraw.** Mithril automatically schedules a redraw after (1) DOM event
  handlers it registered and (2) trace-processor queries completing. Manual
  redraws are only needed for genuinely out-of-band async (`setTimeout`,
  `fetch`, `yield`). The *vast majority* of manual redraws in submitted code are
  unnecessary — delete them. If you think you need one, first find out *why* the
  UI isn't updating; it usually points to a real bug elsewhere, not a missing
  redraw. When you do need it, call `m.redraw()` at the point the event happens
  (it doesn't depend on `app`), not `app.raf.scheduleFullRedraw()`.
  *(#4769, #4464, #4357, #4226, #3999, #3693, #3124, #2615, #1097, #5595, #5436, #5284, #5243)*

- 🔁 **❌ Caching `attrs`, settings, or query results in private member fields →
  ✅ read them fresh.** Mithril passes `attrs` to `oncreate`/`onupdate`/`view`;
  use them from there. The settings store is already cache-backed — read the
  value every access so it can't go stale when the user changes it. A
  presentation component must tolerate its data source changing every frame.
  *(#3857, #6031, #4737)*

- **❌ `requestAnimationFrame` / `setTimeout` timing hacks hoping the DOM is
  ready "next frame" → ✅ use a proper post-render callback.** rAF to dodge a
  `ResizeObserver` loop warning is a code smell. If you need to act after render,
  expose an `onReady` callback invoked from both `oncreate` and `onupdate`.
  *(#4761, #4146)*

- **❌ Passing `m.Children` (vnodes) into a long-lived / one-shot API (e.g. a
  modal) → ✅ pass a function returning `m.Children`.** A stored vnode gets
  reused across render cycles, which is undefined behaviour in Mithril. Pass
  `() => m.Children` so it's re-invoked each render. Likewise, don't store vnodes
  in an immer-controlled data model (immer doesn't support vnodes; it breaks
  undo/redo). *(#4192, #3999)*

- **❌ Doing init / async loads in lifecycle hooks (`oncreate`, `onPopupMount`,
  constructor) when `view()` would do → ✅ compute in `view()`, init once where
  it belongs.** `onPopupMount` is only for when you genuinely need the popup's
  DOM element. Constructors run on every remount, so an async load there can fire
  multiple times — load once in the plugin's `onTraceLoad()` and pass the data in
  via `attrs`. *(#3817, #1389)*

- **❌ Ternary returning `undefined` for conditional children → ✅ short-circuit
  `cond && m(...)`.** Mithril ignores falsy children. *(#2021, #1389)*

- **❌ Unnecessary wrapper `div`s / empty `{}` attrs or `{style:{}}` objects → ✅
  apply the class to the existing element and omit empty objects.** *(#5073, #4993, #2039, #1406)*

- **❌ `stopPropagation()` *and* `preventDefault()` together "to be safe" → ✅
  pick the one you actually need** and justify how it interacts with other
  handlers. *(#3707)*

---

## 2. State, Data Model & Lifecycle

- 🔁 **❌ Module-level globals / static fields / new singletons for "convenient
  access" → ✅ inject the dependency.** Global mutable state breaks the fact that
  **multiple UI instances can run on one page**, and creates ordering/maintenance
  headaches. Plumb state through the `trace`/`app` objects (model it like tabs
  are modeled), or pass a context/settings object. A plugin class is already
  effectively a singleton — use `this.member`, not a `static` field.
  *(#1097, #2036, #5436, #5284, #5286)*

- 🔁 **❌ Caching the `Trace` object on a singleton plugin → ✅ pass `Trace` as a
  function argument.** A cached trace becomes the *wrong* trace after a second
  trace loads; an in-flight async function then resolves against a stale trace.
  Initialize per-trace data in `onTraceLoad()` and pass it as `Readonly` args, so
  you also avoid optional members and `!` assertions everywhere. To attach
  per-trace data without growing the `Trace` API, use a `WeakMap<Trace, T>`.
  *(#5284, #2900)*

- 🔁 **❌ Hand-rolled loading/in-flight booleans and manual change-detection for
  async data → ✅ use `QuerySlot` (and `AsyncLimiter`).** Selection-driven queries
  fired straight off events cause unbounded head-of-line blocking when the user
  clicks faster than queries run. Use a `QuerySlot` (declare a key, poll it each
  render, render stale data with `retainOn`), or load inside
  `TrackEventDetailsPanel.load()`, which auto-cancels obsolete loads. Guard
  concurrency with `AsyncLimiter.isRunning` rather than a bespoke flag. (Note:
  `isFresh` is always `true` unless you use `retainOn` keys.)
  *(#4464, #4582, #4737, #5436, #4192)*

- **❌ Keeping derived state that must be manually kept in sync → ✅ derive it from
  the single source of truth.** Don't reverse-engineer state by matching against
  options — persist the actual choice (e.g. the preset *name*) and re-apply it.
  Use one unified selection list for heterogeneous items rather than parallel
  selection state. *(#3693, #3780, #3999)*

- 🔁 **❌ Storing UI timestamps/durations as JS `number` → ✅ use `bigint`.** JS
  numbers are doubles and lose precision over the large dynamic range of trace
  time. Type SQL time columns as `LONG`/`LONG_NULL` so they materialize as
  `bigint` (no manual `BigInt()` / no precision loss), and use `Time.fromRaw()`
  rather than casts. *(#2503, #4767, #4769)*

- **❌ Querying rows by human-readable display name → ✅ query by stable typed
  columns** (e.g. `type = 'suspend_resume'`, not `name = "Suspend/Resume
  Latency"`). Display strings are fragile. *(#2149)*

- **❌ Forgetting to await for async functions calls in `onActivate()`,
  `onTraceLoad()` or other lifecycle methods.** It's extremely rare to have a
  legitimate reason for calling an async method without await or without a .then(),
  as that creates fire-and-forget semantics that cause races.
  If really needed, it must be documented with a comment. *(#2353, #1758)*

- **❌ Ad-hoc `localStorage` keys for dismissible hints / persisted layout → ✅ use
  the settings system** (and parse unknown persisted objects with Zod, making
  newly-added fields optional for backward compatibility). Make banners
  permanently dismissable; pair a one-time hint with a permanent discoverable
  button rather than a recurring intrusive banner. *(#5761, #2772, #3660, #3185)*

---

## 3. Architecture & Layering

- 🔁 **❌ Vendor / product / plugin-specific code in `src/core` or the `public/`
  API surface → ✅ push specifics into a plugin; keep the core interface
  generic.** This is wrong layering. The same applies to chrome: don't add a
  plugin-specific section to the shared sidebar — keep navigation inside the
  plugin's own page via subtabs/submenu. Keep plugin-specific constants
  (track kinds), factories, and schemas inside the owning plugin. *(#5284, #5153, #1624, #1241)*

- **❌ Pure, general-purpose library code (track impls, panels) wrapped as a
  plugin → ✅ put it in `ui/src/components`.** Conversely, code in its own folder
  should generally *be* a `PerfettoPlugin`; if it isn't, reconsider its
  placement. Genuinely reusable widgets go in `widgets/`; *(#4767, #5284, #1097)*

- **❌ Hidden / incidental cross-plugin dependencies (working "by chance" via load
  order) → ✅ declare dependencies explicitly**, or move the command into the
  plugin it conceptually belongs to. Depend on the plugin that *creates* a track
  before consuming it. Don't take a heavy plugin dependency just to reuse one
  shared resource — extract it (e.g. `dev.perfetto.StandardGroups`). *(#4613, #2666, #2137)*

- **❌ Inheritance / abstract base hierarchies, and one-method "manager" classes →
  ✅ prefer composition and standalone functions.** A class with one method and no
  state should just be a function. Remove thin wrappers that only re-export a base
  helper. *(#4844, #4184)*

---

## 4. Plugin & API Design

- 🔁 **❌ Plugin with no / placeholder `description` → ✅ add a real
  `static readonly description`.** It surfaces on the plugins page and tells users
  what the plugin does and why they'd enable it. Not a TODO. *(#4716, #2394, #5436, #5284)*

- **❌ Manually parsing `location.hash` in a plugin → ✅ use the route-args
  mechanism** — plugin-prefixed URL params are decoded and passed as the second
  arg to `onActivate()`. Design URL/query-param formats with standard conventions
  (repeated keys or comma-separated) and standard percent-encoding; once external
  users adopt a bespoke format it's an interface you can't change. *(#5019, #2471)*

- **❌ Reaching into internal structures (e.g. adding tracks directly to the
  pinned-tracks node) → ✅ use the high-level API** (`TrackNode.pin()`). Add tracks
  to the normal workspace then pin, so unpinning leaves them in the tree. Filter
  tracks by tags (e.g. the `trackIds` tag), don't hand-construct/guess track URIs.
  *(#4814, #4820, #4524)*

- **❌ Reusing an internal core schema for a wire/persisted format → ✅ define the
  external schema locally.** Reusing it means an unrelated change to the core type
  silently mutates a contract that's supposed to be stable. *(#4192)*

- **❌ Unused abstraction interfaces / speculative callback props → ✅ remove them
  (YAGNI).** Don't introduce indirection nobody implements, or expose options for
  unsupported formats and then throw. *(#4767, #5065, #1565)*

- **❌ Settings that only take effect after restart with no signal → ✅ set
  `requiresReload: true`** so the user is prompted to reload. *(#2394, #1298)*

- **❌ `has()/get()`-then-mutate, iterate-to-find-first, separate accessors always
  used together → ✅ use the provided helpers.** `getOrCreate(map, k, () => [])`,
  `result.maybeFirstRow({...})` (undefined for 0 rows), and fold paired accessors
  into one function. *(#5284, #4039, #2036)*

- **❌ Assuming a stdlib table is available → ✅ add the `INCLUDE PERFETTO MODULE`**
  the query depends on (e.g. `slices.with_context`). *(#5392)*

---

## 5. Widgets & Reuse

> 🔁 **Search the widget library (`ui/src/widgets/`) before building anything.**
> "The vast majority of this file could disappear if you use the existing widgets
> and theme variables." This is the single most common reuse note.

- **Use the existing widget instead of raw HTML or hand-rolled markup:**
  `Button`/`SegmentedButton` (not `<button>` or CSS-styled buttons — set
  `variant`/`intent` for emphasis), `Select` (not `<select>`), `Anchor` (not
  `<a>`; use `Icons.ExternalLink` for external), `DetailsShell` for details
  panels, `Tabs` (not `TabStrip` or bespoke tabs — it retains each tab's
  component so data isn't reloaded on switch), `DataGrid` (the deprecated
  `PivotTable` is being removed), `DownloadToFileButton` (for download feedback).
  *(#5153, #2394, #2772, #4582, #4464, #5276, #4226, #4481)*

- **❌ Re-implementing a behaviour the widget already provides → ✅ use its prop.**
  `fillHeight` (on `Editor`, `NodeGraph`, …) instead of `height: 100%` CSS;
  `closeOnOutsideClick`/`closeOnEscape` or explicit open-state instead of popups
  that dismiss each other; mount overlays to an `OverlayContext` in the scrolling
  container instead of hand-rolling anchor/visibility logic. *(#4993, #4027, #3124, #3711)*

- **❌ Overriding a widget's internal classes / re-implementing its spacing from
  outside → ✅ wrap your content in a container you control, or use a `Stack`.**
  Styling widget internals is fragile and breaks when the widget changes; fix
  sizing in the widget's own SCSS. Don't add per-consumer CSS workarounds for a
  bug that belongs in the shared widget. *(#4027, #2615, #2394)*

- **❌ Subclassing a track/widget class just to set static config → ✅ instantiate
  it with options** (`new CounterTrack({trace, uri, sqlSource})`). *(#5284)*

- **Provide `title`/tooltip text on icon-only controls** so their purpose is
  discoverable; prefer a tooltip over a popup for lightweight help. *(#3185, #3124)*

- **When you add a new widget or widget option, add a demo to the widgets page**
  and test it against edge cases (e.g. nested tree nodes). *(#2383, #5659)*

---

## 6. CSS & Styling

- 🔁 **❌ Inline `style={...}` → ✅ put styles in the component's `.scss` file.**
  Static, unchanging styles always belong in the stylesheet. *(#4737, #3185, #2394, #2615)*

- 🔁 **❌ Unprefixed / generic class names (`.row`, `.selected`, `.ai`) → ✅ `pf-`
  prefix + BEM.** Generic selectors collide with shared widgets (a real cause of
  failing integration tests) and break when Perfetto is embedded. Use
  `.pf-component`, `.pf-component__element`, `.pf-component--modifier`, scope rules
  under their owner (`.pf-node .pf-show-on-hover`), and use SCSS nesting
  (`&--horizontal`). *(#2615, #1097, #2394, #3687, #3955, #1406, #2093, #5761, #2039)*

- 🔁 **❌ Hard-coded colors / magic pixel values / invented CSS variables → ✅ theme
  variables from `theme_provider.scss` / shared tokens.** Don't borrow a variable
  from an unrelated domain (track colors for non-track UI) just because it looks
  right — semantics matter. Specify background colors only when absolutely
  necessary, to keep the UI theme-able. *(#2615, #5153, #4991, #4185, #1097)*

- **❌ `classNames` built by string concatenation / template literals → ✅ the
  `classNames()` utility.** *(#3693, #2615, #1406)*

- **❌ Redundant declarations (font-family/size, `cursor`, global element resets
  for `pre`/`code`/`button`) → ✅ let them inherit / delete the duplicate.** Don't
  set `font-family` on page elements — let it inherit from root so theming works.
  *(#5222, #3121, #2826, #2394)*

- **❌ Single-use CSS variables / piling feature styles into a shared global SCSS
  file → ✅ inline the value; start a dedicated root SCSS with an aggregating
  import.** *(#1097, #5243)*

- **Misc layout craft:** `align-items: baseline` to align widget text baselines;
  `align-self: center` on a misaligned icon (don't change the container);
  `box-shadow` for focus outlines (avoids the layout shift of a thicker border);
  nest `<input>` inside its `<label>` for native a11y/click behaviour; fixed-height
  scrollable menus to avoid flicker on async load; avoid `<strong>` and
  translate/transform hover tricks for emphasis. *(#5222, #3121, #3946, #3844, #3817, #2615, #2093)*

---

## 7. TypeScript & Types

- 🔁 **❌ `null` → ✅ `undefined`.** The codebase is rigorously consistent here; use
  `?:` optional syntax. The *only* sanctioned `null` is a SQL `NULL` in query
  results — and even then prefer mapping to `undefined` in UI code. *(#4192, #3599, #3188, #1389)*

- 🔁 **❌ Type assertions (`as`) and non-null `!` → ✅ let TS narrow, or assert the
  invariant loudly.** TS narrows after `.filter`, `Object.entries`, etc.; don't
  assert types that are `unknown` anyway. Replace clusters of assertions on
  untyped data with Zod parsing. Use `assertExists(x)` instead of `x!` so
  developer-error nulls fail immediately. Use `LONG`/`LONG_NULL` + `Time.fromRaw()`
  instead of casting SQL values. Pass the concrete impl type (`TraceImpl`) where
  available to avoid downstream casts. Don't wrap lookups in `Object(...)` — it
  erases the type to `any`; type the lookup table instead. *(#4767, #4890, #4769, #4653, #4613, #3331, #2580)*

- 🔁 **❌ Flat interfaces with many always-optional fields whose valid combinations
  aren't enforced → ✅ discriminated unions** keyed on a `kind`/`status`/`type`
  tag (intersected with shared fields). Let the type system enforce which fields
  are required per variant. *(#4192, #3331)*

- 🔁 **❌ Mutable array params → ✅ `readonly T[]`** (including in getters/
  setters) for collections you don't mutate. Mark interface attrs `readonly`.
  *(#3188, #3058, #2039)*

- **❌ `assertUnreachable()` missing from exhaustive `switch` defaults → ✅ add it**
  so the compiler errors when a union case is added. *(#2615, #1565)*

- **❌ Duplicating an existing type with a new wrapper interface, or routing data
  through a wide/generic interface then re-parsing → ✅ return the existing/correct
  specific type directly** (e.g. return `QueryResult`; return
  `AndroidInputEventSource` instead of generic args you parse back). Drop branded
  types (`SliceSqlId`) on APIs that also accept plain `number`. *(#4844, #4767, #4729)*

- **❌ `== null` comparisons / `==` generally / early coalescing to a sentinel → ✅
  `if (!x)`, `x ?? 0`, the nullish operator.** Beware the JS falsy trap: `0` and
  `null`/`undefined` are both falsy — carry `null` through `(number|null)[]` and
  display real `0`s explicitly rather than rendering them blank. *(#1582, #1389, #1376)*

- **Smaller type hygiene:** don't mark a function `async` with no `await`; add the
  `declare global { interface Window {...} }` augmentation to inject onto `window`
  (don't cast); add explicit annotations to intermediate variables in
  type-heavy code; don't force interface properties to be getters (use plain
  `readonly`); don't redundantly init a field that the constructor always sets;
  prefer explicit `return undefined` over a naked `return` when other branches
  return values; avoid round-tripping numeric keys through strings; don't widen a
  member's visibility (`private`→`public`) incidentally; don't add optional params
  that are always provided. Zod schemas must mirror the actual nested data shape.
  *(#4278, #4192, #2644, #2615, #1763, #2897, #5222, #3058)*

---

## 8. Naming & Conventions

- 🔁 **IDs follow a settled convention.** Settings/registry IDs: reverse-DNS,
  `dev.perfetto.*` for core-owned, the contributor org's namespace
  (`com.meta.*`, `com.android.*`) for externally-owned plugins. Command IDs: omit
  the redundant plugin name and use a stable namespaced verb form so commands can
  be moved between plugins. **Don't tie a setting's ID to a plugin name** — a
  plugin-independent ID lets it be relocated without users losing saved values.
  Don't over-namespace things registered only within your own plugin. *(#5787, #5761, #3362, #3259, #2036, #4653, #4767, #5436)*

- 🔁 **camelCase variables/members; `snake_case` file names; no Hungarian
  notation.** No `snake_case` vars (`trace_processor_id`), no abbreviation
  prefixes (`uMachineIds` → `uniqueMachineIds`), no leading-underscore on private
  methods, and underscore-prefix a param *only* when it is intentionally unused.
  *(#2615, #1155, #1913, #1389, #1565, #4737)*

- **Name things for what they are / do, at the right altitude.** A function that
  only shows (never toggles) is `showX`, not `toggleX`. Name one-shot init
  functions `load`/`initialize`, not `build` (which reads like a per-frame Mithril
  helper). Name a widget by the standard term (`treemap`, `scatterplot`), not
  `treemap_chart`. Rename a widget when its scope broadens. Drop redundant
  qualifiers when everything in context already has them. Don't use two synonyms
  in one identifier (`canvas`/`container`). Name booleans for what they guard
  (`isProcessingQueue`, not `isProcessing`). Name fields for the general concept
  if reuse is anticipated. Make command names self-explanatory/discoverable.
  *(#3169, #4192, #4761, #4729, #4278, #1406, #1302, #4380, #5566)*

- **Mechanical conventions:** copyright header on every new file (current year for
  new files; don't touch years on edits); relative import specifiers for in-repo
  modules; `//` line comments above/right of the line, not `/* */`; run
  `ui/format-sources` (don't hand-format); check new default hotkeys for
  collisions with existing global shortcuts. *(#2394, #4582, #4769)*

---

## 9. Error Handling

- 🔁 **❌ Swallowing errors (empty `catch`, `console.warn`, returning `undefined`)
  → ✅ make failures visible.** Console messages are ignored in practice. For
  **developer errors, let it crash** (preserving stacktrace + analytics); for
  **expected failures, show the user a message** (e.g. `showModal`). An empty
  catch silently eats typos introduced during refactors. *(#4582, #4344, #4192)*

- **❌ Catching exceptions where they're raised → ✅ bubble up to the right
  granularity.** Let a caller's loop catch and skip a failing item rather than
  swallowing per-item inside a helper. Consider returning `Result<T>`
  (StatusOr-style) so callers must handle failure explicitly. *(#4192)*

- **❌ Wide `try` blocks → ✅ wrap only the call that can actually throw**, not
  surrounding setup. *(#4192)*

- 🔁 **❌ Non-null `!` / silent fallback to a global default → ✅ `assertExists()`
  for developer-error invariants.** e.g. `assertExists(canvasElement)` — never
  fall back to `document` by accident; `assertExists(queue.shift())` rather than a
  defensive `if (!blob) continue` for an impossible state. *(#4994, #4761, #2580, #1302)*

- **❌ Network fetches with no timeout / blocking core flows → ✅ `fetchWithTimeout`
  / `orTimeout` and surface a soft error.** Don't rely on the browser's ~5-min
  default; fine-grained deferred fetches on core paths risk "the UI silently stops
  working mid-flow". (And don't set timeouts *too* aggressively — 10s+, people
  tether.) *(#4192)*

- **❌ `try/catch` with no clear thing being handled, or warning on an
  expected-empty condition → ✅ be explicit.** Don't warn that first-run
  `localStorage` state is missing. Don't *remove* existing try/catch guards around
  may-fail queries without confirming the failure path is handled (it can break
  non-Google environments). *(#2615, #4115, #1513)*

- **❌ Throwing aborts a whole batch when one row is bad → ✅ skip/discard the row
  and continue.** Ensure a handler can't leave shared state inconsistent if it
  throws mid-processing. *(#2897, #1302)*

---

## 10. Performance

- **Virtualization:** enable it on height-limited grids that don't need
  variable-height rows; keep the grid's own scroll wrapper (`pf-sql-table`) so it
  can virtualize — don't let the whole details shell scroll. If virtual scrolling
  over a query is slow, materialize a backing table first (the datagrid
  materializes nothing itself). *(#3455, #3423, #5276)*

---

## 11. PR Hygiene & Scope

- 🔁 **❌ Bundling unrelated refactors, drive-by renames, stray fields, whitespace
  churn into a feature PR → ✅ keep the diff minimal and scoped to its purpose.**
  Unrelated changes add noise and make regressions hard to spot. Aim for the MVP;
  split refactors and large mocks into separate PRs. Don't rename
  variables/identifiers in a file that didn't otherwise change. *(#5222, #4641, #3736, #2666, #5649, #3170)*

- 🔁 **❌ Dead/speculative code → ✅ YAGNI.** Remove unused functions, params, loop
  counters, and "just in case" lines (verify by deleting and confirming behaviour
  is unchanged). Remove leftover `console.log()` — delete the code rather than
  logging "not implemented". *(#5222, #5436, #1389, #1097)*

- **Add an `OWNERS` file with a contact when adding a new plugin/component.** *(#5284)*


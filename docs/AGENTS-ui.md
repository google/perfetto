# Perfetto UI Development for AI Agents

Perfetto UI is a Single Page Web Application written in TypeScript using the Mithril framework. It lives in `ui/` and powers ui.perfetto.dev. The UI embeds TraceProcessor via WebAssembly.

The patterns in this document are consistently enforced during code review. Adhering to them will significantly speed up the review process.

> **See also: [UI Review Antipatterns](ui-review-antipatterns.md)** — a deeper,
> categorized catalogue of antipatterns mined from real maintainer review
> feedback on contributor PRs (Mithril/rendering, state, layering, plugin/API
> design, widgets, CSS, types, error handling, performance, PR scope). Consult it
> when authoring or reviewing UI changes to pre-empt recurring mistakes.

## General Principles

- **Don't over-engineer** - Solve the problem at hand, not hypothetical future problems.
- **Prefer simpler approaches** - If there's a simple solution and a complex one, choose simple.
- **Search before creating** - Always search for existing utilities before writing new ones.
- **Be consistent** - Follow the patterns established in the surrounding code.
- **Prefer immutability** - We like immutable, `readonly` data; it makes the code easier to debug. See [Readonly by default](#readonly-by-default).
- **Flag dependency changes** - Never add or bump `ui/package.json` dependencies without explicitly calling it out; new packages are currently hard to import internally.

## Directory Structure

The UI codebase is organized as follows:

```text
ui/src/
├── base/           # Core utilities (time, color, arrays, logging, disposables)
├── widgets/        # Reusable UI components (Button, Menu, Modal, Popup, etc.)
├── components/     # Higher-level components (aggregation panels, query tables)
├── core/           # Core application logic and managers
├── public/         # Public API surface for plugins
├── plugins/        # Optional third-party/external plugins
├── core_plugins/   # Essential core plugins (cannot be disabled)
├── frontend/       # Main frontend rendering code
├── trace_processor/# Engine communication layer (query results, SQL utilities)
├── test/           # Playwright integration tests
└── assets/         # SCSS stylesheets and static assets
```

When possible (if the API surface allows) feature functionality should be encapsulated in a plugin in src/plugins.
- `core_plugins/` (e.g., `dev.perfetto.CoreCommands`, `dev.perfetto.Notes`) contain essential functionality. They cannot be disabled by users and are always active.
- `plugins/` (e.g., `dev.perfetto.Sched`, `com.android.AndroidStartup`) are optional. Users can enable/disable them via feature flags. These are organized by reverse-DNS naming (e.g., `com.android.*`, `dev.perfetto.*`, `org.chromium.*`).
- This distinction is mostly historical. These days in 90% of cases things can (and should) go only inside plugins/
- Look at /docs/contributing/ui-plugins.md as it has extra useful content for plugin authors.

## Building and Running the UI

To build and serve the UI for development:

```sh
# From the repository root
ui/build    # Builds the UI.
ui/build --typecheck # Run tsc --noEmit, doesn't bundle (faster).
ui/run-dev-server    # Starts the development server with live reload.
```

The UI uses:

- **TypeScript** for type safety
- **Mithril** as the UI framework
- **Vite** for bundling
- **pnpm** for package management
- **ESLint** for linting (based on Google style)
- **Playwright** for integration tests

### Type checking when a build is already running

Each build claims a lockfile as it works, unless --no-build option is passed. If
you try to run a build but encounter a failure due to one of those lockfiles
being present, you can try just checking types using the following command 
without interfering with the current build.

```sh
ui/build --typecheck --no-build
```

## Plugin Architecture

Plugins are the primary extension mechanism for the UI. They follow this structure:

```typescript
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {App} from '../../public/app';

export default class MyPlugin implements PerfettoPlugin {
  // Unique reverse-DNS identifier
  static readonly id = 'com.example.MyPlugin';

  // Optional: Human-readable description
  static readonly description = 'Does something useful';

  // Optional: Declare dependencies on other plugins
  static readonly dependencies = [OtherPlugin];

  // Called when the plugin is activated (before trace load)
  static onActivate(app: App): void {
    // Register commands, sidebar items, pages and settings that don't need a trace
  }

  // Called when a trace is loaded
  async onTraceLoad(trace: Trace): Promise<void> {
    // Register tracks, tabs, commands that need trace data
    // Query the trace processor, add tracks to the workspace
  }
}
```

**Plugin Lifecycle:**

1. `onActivate()` - Called when the plugin is enabled, before any trace is loaded. Use for registering global commands, pages, sidebar items, and settings. Settings must be registered here (not in `onTraceLoad()`), otherwise they don't exist before a trace is loaded and deep links like `#!/settings/<id>` break.
2. `onTraceLoad()` - Called when a trace is loaded. Use for registering tracks, tabs, and commands that depend on trace data.
3. `trace.onTraceReady` event - Fired after all plugins have finished `onTraceLoad()`. Use for automations that need all tracks to be available.

Only the lifecycle methods need to be public. Make plugin helper methods `private`.

**Key APIs available to plugins:**

- `trace.engine` - Run SQL queries against TraceProcessor
- `trace.tracks` - Register and find tracks
- `trace.selection` - Manage selection state
- `trace.commands` - Register commands
- `trace.tabs` - Register tabs in the details panel
- `trace.timeline` - Access timeline state
- `trace.workspaces`, `trace.currentWorkspace` - Manage the track tree structure

Try to use the `Trace` object as much as possible - plumb it through the hierarchy wherever needed.

## Mithril Patterns and Best Practices

The UI uses Mithril.js. Follow these patterns:

**Component Structure:**
```typescript
import m from 'mithril';

interface MyComponentAttrs {
  readonly value: string;
  readonly onChange: (newValue: string) => void;
}

export class MyComponent implements m.ClassComponent<MyComponentAttrs> {
  // Local state
  private expanded = false;

  view({attrs}: m.CVnode<MyComponentAttrs>): m.Children {
    return m('.pf-my-component',
      m(Button, {label: attrs.value, onclick: () => this.expanded = !this.expanded}),
      this.expanded && m('.pf-my-component__details', 'Expanded content'),
    );
  }
}
```

### Mithril Rules

- No need to call `m.redraw()` most of the time. We automatically schedule redraws: (1) in Mithril's DOM event handlers; (2) after trace processor queries complete. But NOT after manually registered JS event handlers.
  - The engine schedules a redraw on **every** query response, and the `await` continuation that stores the result runs before that frame. So `m.redraw()` after `await engine.query(...)` is always redundant. Navigation also redraws via the router.
  - **Never schedule redraws with `setTimeout`.** Besides being redundant, the timer can fire after a unittest's jsdom has been torn down, causing flaky failures.
  - The only legitimate case is async work that isn't a query (image decoding, blobs, `fetch`).
- Use `constructor` for initialization if no DOM access is needed, or `oncreate` if DOM is needed. Create long-lived helpers (data sources, memos, counters) in the constructor or component closure rather than `oninit`, so their fields are never `| null` and need no guards. Dispose helpers that hold resources in `onremove` (for `AsyncMemo` this is optional, see below).
- Apply state derived from attrs in `onbeforeupdate` or `view()`, **never in `onupdate`**. `onupdate` runs after the frame has been drawn, so you get one stale frame (flicker) and possibly a wasted query.
- Function members of attrs should be readonly arrow-typed fields (`readonly onClick: (x: X) => void;`), not method shorthand (`onClick(x: X): void;`), which can't be marked `readonly`.
- Put static classes in the selector: `m('.pf-a.pf-b')`, `m('span.pf-x')`, not `m('div', {class: 'pf-a pf-b'})`. Use `className`/`classNames()` only for dynamic classes.
- Mithril flattens nested child arrays, so don't spread them: `m(Menu, a, b, renderItems())`, not `m(Menu, a, b, ...renderItems())`.
- Pass presentation callbacks (renderers, tooltips, icons, colours) as attrs that the parent rebuilds in every `view()`, so they can capture the parent's current state. Don't bake them into long-lived objects (data sources, fetchers, config objects passed to a constructor) or cache their results. Keep cached data separate from presentation, as `DataGrid` does with its data source vs `schema`/cell renderers.
- Don't pass raw DOM events across component boundaries. The widget turns the event into an intent (e.g. `onCollapseToggle(alt: boolean)` rather than `onclick(e: MouseEvent)` with the caller reading `e.shiftKey`).
- Don't pass through callbacks for things the caller can do directly (e.g. call `trace.navigate()` instead of injecting a `navigateCallback`).
- Use plain class methods, not arrow-function properties (`navigate = () => {...}`). Bind at the call site (`onclick: () => session.navigate(x)`).
- **Hot module reloading:** a component module is only hot-reloadable if its only runtime exports are Mithril components with an explicit type annotation (`implements m.ClassComponent<...>`, `: m.Component<...>`). Type-only exports are fine. Put helper functions and constants in a separate module. See `pluginMithrilHmr` in `ui/vite.config.mjs`.

**Don't use `oncreate`/lifecycle hooks for things that can be done in `view()`:**
```typescript
// Bad - splitting code across lifecycle methods hurts readability.
oncreate() {
  this.computedValue = inexpensiveComputation();
}

// Good - compute in view. If expensive initialize in the constructor.
view() {
  const computedValue = inexpensiveComputation();
  return m('div', computedValue);
}
```

### Pages, routing and loading

- Use `location.replace()` (not `trace.navigate()`) when rewriting the URL for something the user didn't do: consuming one-shot URL params, or redirecting to a canonical URL (e.g. filling in a default id). `trace.navigate()` adds a history entry, so Back lands on a URL that redirects forward again and the user gets stuck.
- Put page state that the user would want to share or go Back to (active sub-tab, selected process) in the route, not in component-local state. Links into another page should include the target entity rather than relying on what that page currently has selected.
- Load view-specific data in the component that displays it (with `AsyncMemo`), not up front for the whole page. (Trace-wide data that doesn't depend on view state can still be loaded once in `onTraceLoad()` and passed in via attrs.)
- Use `Tabs` with `lazy: true` for tabs with expensive content. By default every tab's content is mounted (and its queries run) on first render; lazy tabs mount on first open and then stay mounted.

### Conditional Rendering with State Preservation

Use the `Gate` component when you need to conditionally show/hide content while preserving component state:
```typescript
import {Gate} from '../base/mithril_utils';

m(Gate, {open: this.isVisible}, m(ExpensiveComponent));
```

### Declarative Data Loading (`AsyncMemo`)

UI components in Mithril render synchronously, but often depend on asynchronous data (e.g. SQL queries). **Never hand-roll data fetching in lifecycle hooks (`oninit`/`onupdate`) using manual `loading` booleans, sequence counters (`fetchSeq`), or `prevId` tracking.** Likewise, **avoid initiating data fetching directly inside DOM event handlers (`onclick`, `onkeydown`, etc.)**. Loading should be a declarative product of state, which could be triggered from many different places (e.g. keyboard shortcuts, external selection, deep links). Update the state in the event handler and let the redraw mechanism handle data loading automatically.

#### Using `AsyncMemo`

`AsyncMemo<T>` provides declarative, keyed async fetching directly inside `view()`:

```typescript
import m from 'mithril';
import {AsyncMemo, TASK_CANCELLED} from '../base/async_memo';

export function MyComponent(): m.Component<MyComponentAttrs> {
  // 1. Instantiate once per component (closure or class field), NOT inside view(), as this is where the cache is stored.
  const dataMemo = new AsyncMemo<MyData>();

  return {
    view({attrs}) {
      // 2. Declare dependencies via `key`. compute() runs automatically when key changes.
      const result = dataMemo.use({
        key: {traceId: attrs.trace.id, filter: attrs.filter},
        compute: async (signal) => {
          const summary = await querySummary(attrs.trace.engine, attrs.filter);

          // Optional: check cancellation to bail out early if superseded or disposed.
          if (signal.isCancelled) return TASK_CANCELLED;

          const details = await queryDetails(attrs.trace.engine, attrs.filter);
          return {summary, details};
        },
        // Optional: show stale data while fetching when only certain keys change
        retainOn: ['filter'],
      });

      if (result.isPending) {
        return m(Spinner);
      }

      return m('.pf-my-component', renderData(result.data));
    },
    onremove() {
      // 3. Optional: dispose the memo on unmount to cancel pending tasks or clean up resources early
      dataMemo.dispose();
    },
  };
}
```

**Key behaviors of `AsyncMemo`:**
- **Never instantiate `AsyncMemo` inside `view()`**: It must be created in the component setup closure or class constructor/field so its cache persists across render cycles.
- **Keys are compared by value (structural equality)**: The `key` can be any JSON-compatible structure (primitives, objects, arrays, bigints). Keys are serialized via `stringifyJsonWithBigints` and compared by value rather than object reference, so passing an object literal created during render (e.g. `key: {traceId: attrs.trace.id, filter: attrs.filter}`) will only trigger a re-fetch if its contents actually change. When the key changes, any pending task is superseded ("latest wins").
- **Automatic Redraw**: `AsyncMemo` automatically calls `m.redraw()` when `compute` finishes. Never call `m.redraw()` manually inside `compute`.
- **Concurrency Control**: Tasks are executed serially via an internal `AtomicTaskQueue`, preventing interleaved queries against shared resources (like temporary tables). Multiple memos can share an `AtomicTaskQueue` if needed.
- **Cancellation**: `compute` receives a `CancellationSignal`. Long tasks can check `signal.isCancelled` and return `TASK_CANCELLED` to avoid caching stale results.
- **Stale Transitions (`retainOn`)**: If you specify `retainOn: ['pagination']`, changing pagination will continue returning the previous `result.data` with `result.isPending = true`, avoiding visual flicker while fetching.
- **Automatic Resource Disposal (`AsyncDisposable`)**: If the returned value is disposable (implements `AsyncDisposable`), it will automatically be disposed when no longer required—specifically, when a new value replaces it after a key change, when `memo.invalidate()` is called, or when the memo itself is disposed in `onremove()`. Disposal is coordinated through the task queue so it stays synchronized with in-flight work.
- **Disposing the memo is optional**: You can call `.dispose()` in `onremove()` to cancel pending tasks and clean up cached disposable resources eagerly when the component unmounts.
- **`compute` functions should be side-effect free**: A `compute` function should only derive data or manage cached SQL structures. Do not mutate external state inside `compute`. The only allowed side effects are temporary SQL entities (tables, views, indexes)—and these **must be dropped in the returned disposable** (`AsyncDisposable`) so they are cleaned up automatically when evicted or invalidated.
- **Don't work around `AsyncMemo`**: Don't add hand-rolled loading/pinning state around a memo (e.g. a `loadingSqlSource` field, or `enabled: table !== undefined`). Express the behaviour through keys, `retainOn`, or chained memos. If `AsyncMemo` genuinely can't express it, consider raising a bug against `AsyncMemo` rather than working around it in the caller. Remember, `AsyncMemo` is meant to remove the boilerplate of asynchronously loading data, not make it more complex.
- **Scope keys to the entity being viewed**: Key per-process / per-dump data by that entity (e.g. `upid`, dump `ts`), not once for the whole trace. A trace can contain several processes or dumps, and trace-wide sets keyed by display name give false positives.

#### Helpers with async state: `use(inputs)`

Apply the same idea to your own helpers. A helper that owns async state should expose a `use(inputs)` method called from `view()` every render (with `AsyncMemo`s inside), rather than imperative `init()` / `onFiltersChanged()` calls plus manual redraws. Inputs are compared every render, and cancellation, serialization and redraws come for free.

```typescript
// Bad
counter.init();
counter.onFiltersChanged(filters);

// Good - called from view()
const count = counter.use({query, filters});
```

#### Multi-Step Operations & `AtomicTaskQueue`

When an operation requires more than one asynchronous step (e.g., creating temporary tables/views, dropping old tables, and then querying them), standard async/await code easily suffers from race conditions. If inputs change while task A is halfway through, task B might start and drop or overwrite temporary tables that task A is still querying.

`AtomicTaskQueue` runs one task at a time to completion then starts the next task. Sharing a single `AtomicTaskQueue` across multiple `AsyncMemo` instances guarantees that their multi-step queries never interleave.

#### Chaining `AsyncMemo` Instances (Multi-Tier Caching)

When some state changes infrequently (e.g., creating temporary mipmap tables or preparing views) while other derived state changes frequently (e.g., timeline pan/zoom bounds, pagination, or filters), chain two `AsyncMemo` instances together:

```typescript
import {AsyncMemo, AtomicTaskQueue} from '../base/async_memo';

class MyTrack {
  // Share an AtomicTaskQueue so table setup and table querying never race
  private readonly queue = new AtomicTaskQueue();
  private readonly tableSlot = new AsyncMemo<MipmapTables>(this.queue);
  private readonly dataSlot = new AsyncMemo<Data>(this.queue);

  render(ctx: TrackRenderContext) {
    // 1. Slow/infrequent step: create temporary tables (only re-runs if track config changes)
    const tableResult = this.tableSlot.use({
      key: {trackId: this.config.trackId},
      compute: () => this.createMipmapTables(),
    });

    // If the dependent table hasn't been created yet, return a loading spinner
    // (here we just return early / undefined for brevity).
    if (tableResult.data === undefined) return;

    // 2. Fast/frequent step: query tables for visible bounds
    const dataResult = this.dataSlot.use({
      key: {
        tableName: tableResult.data.tableName,
        start: ctx.bounds.start,
        end: ctx.bounds.end,
        resolution: ctx.bounds.resolution,
      },
      compute: async (signal) => {
        return this.fetchData(tableResult.data.tableName, ctx.bounds, signal);
      },
      retainOn: ['start', 'end', 'resolution'],
    });

    if (dataResult.data === undefined) return;
    this.renderData(ctx, dataResult.data);
  }

  private async fetchData(
    tableName: string,
    bounds: Bounds,
    signal: CancellationSignal,
  ): Promise<Data | typeof TASK_CANCELLED> {
    // Multi-step query: query summary stats first, then detail slices
    const summary = await this.engine.query(`SELECT ... FROM ${tableName} ...`);
    if (signal.isCancelled) return TASK_CANCELLED;

    const details = await this.engine.query(`SELECT ... FROM ${tableName} ...`);
    if (signal.isCancelled) return TASK_CANCELLED;

    return {summary, details};
  }

  dispose() {
    this.tableSlot.dispose();
    this.dataSlot.dispose();
  }
}
```

Key takeaways:
- **Multi-tier caching**: When the user pans or zooms, only `dataSlot` re-runs; `tableSlot` remains cached and does not re-create tables.
- **Guaranteed non-interleaving**: Notice that `fetchData` runs multiple queries across asynchronous `await` points. Because `tableSlot` and `dataSlot` share the same `AtomicTaskQueue`, it is **impossible for `createMipmapTables` (or any other task on this queue) to run in between the two queries in `fetchData`**. The queue ensures all tasks run to completion atomically and serially.

## Widgets

### Widget Library

The `ui/src/widgets/` directory contains reusable components. The widget library is comprehensive: always check here before creating new UI elements, and prefer existing widgets over new components:

- `Button`, `ButtonBar`, `ButtonGroup` - Various button styles
- `PopupMenu`, `Menu`, `MenuItem`, `MenuDivider` - Dropdown menus (adjacent `MenuDivider`s collapse into one, so no logic is needed to avoid doubles)
- `Popup` - Floating popup containers
- `Modal` - Modal dialogs
- `TextInput`, `Select`, `Checkbox`, `Switch` - Form controls
- `Tree` - Tree view component
- `DataGrid` - Tabular data grid component (in `ui/src/components/widgets/`). Use column `aggregate` (e.g. `'SUM'`) for totals rather than adding a hand-built "Total" row to the data, which gets sorted, filtered, and counted like a real row.
- `Tabs` - Tabbed interface (supports `lazy` tabs)
- `TabStrip` - Tab headers only, without managed content
- `Callout` - Highlighted info/warning box
- `Chip` - Small label/tag
- `Anchor` - Links
- `Spinner` - Loading indicator
- `EmptyState` - Empty state placeholder
- `ui/src/components/widgets/charts_svg/` - SVG charts. Prefer these over ECharts for new charts.

### Using Widgets

```typescript
import {Button, ButtonVariant} from '../widgets/button';
import {Icons} from '../base/semantic_icons';

m(Button, {
  label: 'Click me',
  icon: Icons.Search,
  variant: ButtonVariant.Filled,
  onclick: () => { /* handle click */ },
});
```

**Use the `Anchor` widget for links:**
```typescript
import {Anchor} from '../widgets/anchor';
import {Icons} from '../base/semantic_icons';

// Bad
m('a', {href: 'https://example.com', target: '_blank'}, 'Link')

// Good
m(Anchor, {href: 'https://example.com', icon: Icons.ExternalLink}, 'Link')
```

### Widget Rules

- Use the existing widget even if it doesn't look quite right, and propose restyling the shared widget in a separate PR.
- Don't change the default behaviour of a shared widget or helper to suit one consumer; that's a breaking change for other callers. Add an option instead. Optional or noisy features should be off by default.
- To add a widget modifier, add an optional, commented `readonly` boolean attr (default `false`) that maps to a BEM modifier class (`pf-<widget>--<modifier>`) via `classNames()`, with an `&--<modifier>` rule in the widget's `.scss`.
- Reusable components shouldn't contain context-specific sizing (e.g. a fixed `max-width` that only makes sense in one host). Fill the parent and let the host constrain it. Size icons next to text with relative units (`em`), not `px`.
- Use existing extension points before adding new ones (e.g. the status bar, `TrackNode` chips, track `tags`). Don't add a new extension point for a single plugin.
- Use `Icons.*` from `base/semantic_icons` rather than raw Material icon names (`Icons.Warning`, not `'warning'`).
- After using a widget feature, verify it in the running UI, not just with the type checker (e.g. a data source may not implement aggregates, leaving them spinning forever).

## TypeScript Code Style

Follow these guidelines for TypeScript code:

- **Avoid `any` as much as you can**: Use `@typescript-eslint/no-explicit-any` rule if you really need it. In most cases it's enough to use `unknown` and type guards instead.
- **Unused variables**: Prefix with underscore (`_unused`) to satisfy `@typescript-eslint/no-unused-vars`.
- **Strict boolean expressions**: Don't use numbers or strings in boolean contexts implicitly.
- **Use existing utilities**: Check `ui/src/base/` for utilities before writing your own:
  - `time.ts` - Time handling
  - `assert.ts` - `assertTrue()`, `assertFalse()`, `assertExists()`, `assertDefined()`, `ensureDefined()`, `assertUnreachable()`
  - `disposable_stack.ts` - Resource cleanup
  - `deferred.ts` - Promise utilities
  - `string_utils.ts` - String manipulation
  - `array_utils.ts` - Array helpers
  - `math_utils.ts` - Numeric helpers (e.g. `clamp()`)
- **Never use sentinel values for missing data**: Represent "maybe missing" in the type (`number | undefined`) instead of coercing to `0` or `''`, which downstream code then has to treat as missing (and which gets displayed as e.g. "pid 0").
- **Build objects in one go**: Construct data objects (e.g. query result rows) fully and make them `readonly`, rather than creating them partially and patching them afterwards. Only fields that are genuinely filled in later should be mutable.
- **No late-initialized optional fields**: Don't keep `private foo?: X` fields that are set in `onTraceLoad()` and read back via `assertExists(this.foo)`. Pass the value as a local or capture it in a closure.

### Readonly by default

Use `readonly` for interface properties (including component attrs) and function parameters to prevent accidental mutation. Use `readonly T[]` for arrays that shouldn't be modified:
```typescript
// Bad
function process(items: string[]): void { ... }

// Good
function process(items: readonly string[]): void { ... }
```

### Prefer `undefined` over `null`

```typescript
// Bad
function getValue(): string | null { return null; }

// Good
function getValue(): string | undefined { return undefined; }
```

### Use `assertUnreachable()` in switch default cases

```typescript
import {assertUnreachable} from '../base/assert';

switch (value) {
  case 'a': return handleA();
  case 'b': return handleB();
  default:
    assertUnreachable(value); // TypeScript will error if cases aren't exhaustive
}
```

### Null safety with `noUncheckedIndexedAccess`

`noUncheckedIndexedAccess` is enabled, so indexing an array or record yields `T | undefined`. Handle it properly rather than silencing it. As a rule, don't use `!`: it costs nothing at runtime but hides bugs. The only exceptions are the hot-loop and parallel-array cases below, and they need a comment explaining why the access is safe.

- **Prefer `for...of` / `arr.entries()`** over index loops, so there's nothing to be undefined.
- **Hot loops**: `!` is acceptable where the index is provably in range (e.g. `i < arr.length`).
- **Parallel arrays**: assert the lengths match once before the loop, or iterate one and use `!` on the other with a comment saying the lengths match.
- **Read then check, don't check `length` then index:**
  ```typescript
  // Bad
  if (arr.length === 0) return undefined;
  return arr[0]!.name;

  // Good
  const first = arr[0];
  if (first === undefined) return undefined;
  return first.name;
  ```
- **Record / index-signature lookups**: use `?? default`, or narrow the key to a literal union so the lookup is total.
- **`split()` and regex destructures** may be shorter than expected; treat elements as optional unless the format has already been validated.
- Use `str.charAt(i)` rather than `str[i]!`. Use tuple types (`[number, number]`) for fixed-length data.
- When an invariant depends on external or untrusted data, use `assertDefined(x)` (narrows) or `ensureDefined(x)` (inline) from `base/assert`. Use the `*Exists()` variants only if `null` must also be rejected.
- **Removing a `!` must not change behaviour.** e.g. replacing `x!` with `x ?? null` turns `undefined` into a SQL `NULL` in `DataGrid`. Prefer existing helpers (`clamp()`, `charAt()`) over clever rewrites.

## Working with TraceProcessor

Plugins query data using SQL through the TraceProcessor engine:

```typescript
async onTraceLoad(trace: Trace): Promise<void> {
  const result = await trace.engine.query(`
    SELECT ts, dur, name
    FROM slice
    WHERE name LIKE '%mySlice%'
    LIMIT 100
  `);

  // Use typed iteration
  const iter = result.iter({
    ts: LONG,      // bigint
    dur: LONG,     // bigint
    name: STR,     // string
  });

  for (; iter.valid(); iter.next()) {
    console.log(iter.ts, iter.dur, iter.name);
  }
}
```

### Prefer `NUM` (number) over `LONG` (bigint) for TraceProcessor ID fields

When pulling out TraceProcessor-assigned ID columns (e.g. `track_id`, `upid`, `utid`, `slice.id`), use `NUM`/`NUM_NULL` and `number` instead of `LONG`/`LONG_NULL` and `bigint`:

```typescript
const iter = result.iter({
  track_id: NUM,   // GOOD: number — preferred for IDs
  // track_id: LONG, // BAD: bigint — avoid for IDs
});
```

TraceProcessor IDs are assigned sequentially by the engine, so they are small integers guaranteed to fit well within the 2^53 limit of JS `number`s (the largest safe integer). Using `number` avoids the awkwardness of `bigint` arithmetic and comparisons (`1n !== 1`), the need for `Number()`/`BigInt()` conversions at boundaries (e.g. track tags, URLs, JSON), and avoids the performance hit of `bigint` operations, which are significantly slower than number ops.

### Prefer `LONG` (bigint) over `NUM` (number) for timestamps and durations

Timestamps (`ts`) and durations (`dur`) in nanoseconds should use `LONG`/`bigint`, as they can genuinely exceed 2^53 — e.g. a ns timestamp overflows once the trace spans ~104 days (2^53 ns), and arithmetic on ns values (e.g. sums) can exceed it well before that. `bigint` is the only safe representation for those values.

### Keep times and durations as `Time`/`Duration` where possible

The UI has first-class types for these values in `ui/src/base/time.ts`: the branded `time` type (via the `Time` class) and `duration`, which is a type alias for `bigint` (via the `Duration` class). When pulling `ts`/`dur` (or any ns timestamp/duration) out of a query result, immediately convert to these types rather than passing raw `bigint`s around:

```typescript
const iter = result.iter({
  ts: LONG,
  dur: LONG,
});

for (; iter.valid(); iter.next()) {
  const start = Time.fromRaw(iter.ts);      // time (branded bigint)
  const dur = Duration.fromRaw(iter.dur);   // duration
}
```

Because `time` is branded, TypeScript will reject passing a `duration` or plain `bigint` where a `time` is expected (and vice versa), catching time/duration mixups at compile time. Both classes also provide helpers for arithmetic and formatting (e.g. `Time.add`/`Time.clamp`, `Duration.humanise`/`Duration.format`, `Timecode`). Only hold raw `bigint`s at the query boundary; convert to `Time`/`Duration` as soon as you leave the query iteration.

## Track creation

Rarely you need to create a new Track from scratch.
In most cases you can use higher level components in ui/src/components/tracks/, especially SliceTrack (examples in /docs/contributing/ui-plugins.md).
Look at those examples first and keep creating a track via trace.tracks.registerTrack as a last-resort.

- Set `SliceTrack`'s `rootTableName` deliberately. It controls whether flows are drawn and whether deep links to slices work; leaving it out silently disables both.
- Keep a reference to a `TrackNode` you just created rather than looking it up again (e.g. with `getTrackByUri()`). Build child tracks inline right after the parent.
- `sortOrder` / `addChildInOrder()` only matter when siblings come from different places. In a group whose children all come from your own code, use `addChild()` without `sortOrder`.
- Plugins should only modify `TrackNode`s they own.

## CSS/SCSS Conventions

Stylesheets live in `ui/src/assets/` and component-specific `.scss` files alongside components.

- Use CSS custom properties (variables) defined in `theme_provider.scss` for colors
- Support both light and dark themes using semantic color variables
- Never `@use` a Sass file that emits raw CSS: every file that uses it gets its own copy. Keep mixins and variables in pure-Sass files (e.g. `theme.scss`, `common.scss`), and import CSS-emitting files once from an entry point. Shared partials go in the existing `common.scss` rather than a new file.

**All CSS classes must have the `pf-` prefix and follow BEM-like naming** (`.pf-component`, `.pf-component__element`, `.pf-component--modifier`):
```scss
// Bad
.my-component { ... }
.row { ... }

// Good
.pf-my-component { ... }
.pf-my-component__row { ... }
```

**Never use inline styles - use stylesheets:**
```typescript
// Bad
m('div', {style: {color: 'red', padding: '10px'}}, 'content')

// Good
m('.pf-my-component', 'content') // with styles in .scss file
```

**Never hard-code colors - use theme variables:**
```scss
// Bad
.pf-my-component {
  color: #333;
  background: white;
}

// Good
.pf-my-component {
  color: var(--pf-color-text);
  background: var(--pf-color-background);
}
```

**Use `classNames()` utility for building dynamic CSS class strings:**
```typescript
import {classNames} from '../base/classnames';

// Bad
const cls = 'pf-row' + (isSelected ? ' pf-row--selected' : '') + (isDisabled ? ' pf-row--disabled' : '');

// Good
const cls = classNames('pf-row', isSelected && 'pf-row--selected', isDisabled && 'pf-row--disabled');
```

## Security

- **Never put untrusted strings into HTML sinks**: `m.trust()`, `innerHTML`, or third-party HTML string APIs (e.g. ECharts tooltip `formatter`). Untrusted includes trace data, permalink/JSON state, extension payloads and LLM output. Build the content with Mithril vnodes instead and, if a raw DOM node is needed, render it with `m.render()`. Mithril escapes strings as long as you avoid `m.trust()`.
- **Validate untrusted JSON fully with Zod**: no casts, no `z.any()`, no `z.array(z.unknown())`, no hand-written type predicates. For node-based state (graphs etc.), each node type should register its own schema.

**Use Zod for parsing objects of unknown types:**
```typescript
import {z} from 'zod';

// Bad - unsafe type assertion
const config = JSON.parse(data) as MyConfig;

// Good - validated parsing
const ConfigSchema = z.object({
  name: z.string(),
  value: z.number(),
});
const config = ConfigSchema.parse(JSON.parse(data));
```

## Canvas and environment values

Track values driven by the environment (device pixel ratio, canvas size) with listeners (`ResizeObserver`, a `matchMedia` DPR watcher) and store them when they change, rather than reading or passing them every frame, which can skew size against DPR. Wrap such listeners in a small helper that invokes the callback once on registration and returns a `Disposable`.

## Naming Conventions

**Variables should be camelCase:**
```typescript
// Bad
const trace_processor_id = 123;

// Good
const traceProcessorId = 123;
```

**Settings/flags should use reverse-DNS format:**
```typescript
// Bad
const settingId = 'trackHeightMinPx';

// Good
const settingId = 'dev.perfetto.TrackHeightMinPx';
```

**Command IDs should be descriptive but omit redundant plugin name:**
```typescript
// Bad (if plugin is com.android.OrganizeNestedTracks)
const commandId = 'com.android.OrganizeNestedTracks#organizeNestedTracks';

// Good
const commandId = 'com.android.OrganizeNestedTracks';
```

**Copyright years should be current when creating new files:**
But don't touch years when editing existing files.
```typescript
// Bad (if current year is 2025)
// Copyright (C) 2024 The Android Open Source Project

// Good
// Copyright (C) 2025 The Android Open Source Project
```

## Testing

### UI Unit Tests

Unit tests are run with:
```sh
ui/run-unittests
```

TypeScript unit tests follow the pattern `*_unittest.ts` and use Vitest.

Write tests that exercise real behaviour. Don't write tests that mock everything important and only check that your code calls your code (no tests is better than that). Use Playwright tests for rendering behaviour. Simple wrapper components don't need unit tests. Hardware-dependent changes (e.g. DPR handling) must be verified manually on real hardware.

### UI Integration Tests

Integration tests use Playwright:
```sh
ui/run-integrationtests
```

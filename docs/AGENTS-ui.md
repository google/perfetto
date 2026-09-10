# Perfetto UI Development for AI Agents

Perfetto UI is a Single Page Web Application written in TypeScript using the Mithril framework. It lives in `ui/` and powers ui.perfetto.dev. The UI embeds TraceProcessor via WebAssembly.

## General Principles

- **Don't over-engineer** - Solve the problem at hand, not hypothetical future problems.
- **Prefer simpler approaches** - If there's a simple solution and a complex one, choose simple.
- **Search before creating** - Always search for existing utilities before writing new ones.
- **Be consistent** - Follow the patterns established in the surrounding code.
- **Prefer interfaces with immutable readonly members** - We like immutability, makes the code easier to debug.

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
- **Rollup** for bundling
- **pnpm** for package management
- **ESLint** for linting (based on Google style)
- **Playwright** for integration tests

## Type checking when a build is already running

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
    // Register commands, sidebar items, pages that don't need a trace
  }

  // Called when a trace is loaded
  async onTraceLoad(trace: Trace): Promise<void> {
    // Register tracks, tabs, commands that need trace data
    // Query the trace processor, add tracks to the workspace
  }
}
```

**Plugin Lifecycle:**

1. `onActivate()` - Called when the plugin is enabled, before any trace is loaded. Use for registering global commands, pages, and sidebar items.
2. `onTraceLoad()` - Called when a trace is loaded. Use for registering tracks, tabs, and commands that depend on trace data.
3. `trace.onTraceReady` event - Fired after all plugins have finished `onTraceLoad()`. Use for automations that need all tracks to be available.

**Key APIs available to plugins:**

- `trace.engine` - Run SQL queries against TraceProcessor
- `trace.tracks` - Register and find tracks
- `trace.selection` - Manage selection state
- `trace.commands` - Register commands
- `trace.tabs` - Register tabs in the details panel
- `trace.timeline` - Access timeline state
- `trace.workspace` - Manage the track tree structure

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
    return m('.my-component',
      m(Button, {label: attrs.value, onclick: () => this.expanded = !this.expanded}),
      this.expanded && m('.details', 'Expanded content'),
    );
  }
}
```

**Mithril Rules:**

- No need to call`m.redraw()` most of the times. We automatically schedules redraws: (1) in Mithril's DOM event handlers; (2) after trace processor queries complete. But NOT after manually registered JS event handlers.
- Use `constructor` for initialization if no DOM access is needed, or `onCreate` if DOM is needed.
- Prefer using the existing widget library (`ui/src/widgets/`) over creating new components.
- Use `readonly` for attrs properties to prevent accidental mutation. We like things to be immutable.

**Conditional Rendering with State Preservation:**
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

      return m('.my-component', renderData(result.data));
    },
    onremove() {
      // 3. Optional: dispose the memo on unmount to cancel pending tasks or clean up resources early
      dataMemo.dispose();
    },
  };
}
```

**Key behaviors of `AsyncMemo`:**
- **Keys are compared by value (structural equality)**: The `key` can be any JSON-compatible structure (primitives, objects, arrays, bigints). Keys are serialized via `stringifyJsonWithBigints` and compared by value rather than object reference, so passing an object literal created during render (e.g. `key: {traceId: attrs.trace.id, filter: attrs.filter}`) will only trigger a re-fetch if its contents actually change. When the key changes, any pending task is superseded ("latest wins").
- **Automatic Redraw**: `AsyncMemo` automatically calls `m.redraw()` when `compute` finishes. Never call `m.redraw()` manually inside `compute`.
- **Concurrency Control**: Tasks are executed serially via an internal `AtomicTaskQueue`, preventing interleaved queries against shared resources (like temporary tables). Multiple memos can share an `AtomicTaskQueue` if needed.
- **Cancellation**: `compute` receives a `CancellationSignal`. Long tasks can check `signal.isCancelled` and return `TASK_CANCELLED` to avoid caching stale results.
- **Stale Transitions (`retainOn`)**: If you specify `retainOn: ['pagination']`, changing pagination will continue returning the previous `result.data` with `result.isPending = true`, avoiding visual flicker while fetching.
- **Automatic Resource Disposal (`AsyncDisposable`)**: If the returned value is disposable (implements `AsyncDisposable`), it will automatically be disposed when no longer required—specifically, when a new value replaces it after a key change, when `memo.invalidate()` is called, or when the memo itself is disposed in `onremove()`. Disposal is coordinated through the task queue so it stays synchronized with in-flight work.
- **`compute` functions should be side-effect free**: A `compute` function should only derive data or manage cached SQL structures. Do not mutate external state inside `compute`. The only allowed side effects are temporary SQL entities (tables, views, indexes)—and these **must be dropped in the returned disposable** (`AsyncDisposable`) so they are cleaned up automatically when evicted or invalidated.

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


### Widget Library

The `ui/src/widgets/` directory contains reusable components. Always check here before creating new UI elements:

- `Button`, `ButtonBar`, `ButtonGroup` - Various button styles
- `PopupMenu`, `Menu`, `MenuItem`, `MenuDivider` - Dropdown menus
- `Popup` - Floating popup containers
- `Modal` - Modal dialogs
- `TextInput`, `Select`, `Checkbox`, `Switch` - Form controls
- `Tree` - Tree view component
- `DataGrid` - Tabular data grid component
- `Tabs` - Tabbed interface
- `Spinner` - Loading indicator
- `EmptyState` - Empty state placeholder

**Using Widgets:**

```typescript
import {Button, ButtonVariant} from '../widgets/button';
import {Popup} from '../widgets/popup';

m(Button, {
  label: 'Click me',
  icon: 'search',
  variant: ButtonVariant.Filled,
  onclick: () => { /* handle click */ },
});
```

## TypeScript Code Style

Follow these guidelines for TypeScript code:

- **Avoid `any` as much as you can**: Use `@typescript-eslint/no-explicit-any` rule if you really need it. In most cases it's enough to use `unknown` and type guards instead.
- **Unused variables**: Prefix with underscore (`_unused`) to satisfy `@typescript-eslint/no-unused-vars`.
- **Strict boolean expressions**: Don't use numbers or strings in boolean contexts implicitly.
- **Readonly by default**: Use `readonly` for interface properties and function parameters.
- **Use existing utilities**: Check `ui/src/base/` for utilities before writing your own:
  - `time.ts`, `duration.ts` - Time handling
  - `logging.ts` - `assertTrue()`, `assertExists()`, `assertFalse()`
  - `disposable_stack.ts` - Resource cleanup
  - `deferred.ts` - Promise utilities
  - `string_utils.ts` - String manipulation
  - `array_utils.ts` - Array helpers

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

When pulling out TraceProcessor ID columns (e.g. `track_id`, `upid`, `utid`, `slice.id`, `process_id`), use `NUM`/`NUM_NULL` and `number` instead of `LONG`/`LONG_NULL` and `bigint`:

```typescript
const iter = result.iter({
  track_id: NUM,   // number — preferred for IDs
  // track_id: LONG, // bigint — avoid for IDs
});
```

TraceProcessor IDs are assigned sequentially by the engine, so they are small integers guaranteed to fit well within the 2^53 limit of JS `number`s (the largest safe integer). Using `number` avoids the awkwardness of `bigint` arithmetic and comparisons (`1n !== 1`), the need for `Number()`/`BigInt()` conversions at boundaries (e.g. track tags, URLs, JSON), lets the query result decode into a `Float64Array` instead of a `BigInt64Array`, and avoids the performance hit of `bigint` operations, which are significantly slower than native number ops.

**When to use `LONG`**: timestamps (`ts`) and durations (`dur`) in nanoseconds should use `LONG`/`bigint`, as they can genuinely exceed 2^53 — e.g. a timestamp in ns overflows once the trace clock passes ~262 days, and large `dur` values or arithmetic on ns timestamps (deltas, sums) can exceed it well before that. `bigint` is the only safe representation for those values.

### Keep times and durations as `Time`/`Duration` where possible

The UI has first-class types for these values in `ui/src/base/time.ts`: the branded `time` type (via the `Time` class) and `duration` (via the `Duration` class). When pulling `ts`/`dur` (or any ns timestamp/duration) out of a query result, immediately convert to these types rather than passing raw `bigint`s around:

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

The branded `time` type prevents accidentally mixing up times and durations (or plain bigints) at compile time, and both types provide helpers for arithmetic, formatting, and clamping. Only hold raw `bigint`s at the query boundary; convert to `Time`/`Duration` as soon as you leave the query iteration.

## Track creation

Rarely you need to create a new Track from scratch.
In most cases you can use higher level components in ui/src/components/tracks/, especially DatasetSliceTrack (examples in /docs/contributing/ui-plugins.md).
Look at those examples first and keep creating a track via trace.tracks.registerTrack as a last-resort.

## CSS/SCSS Conventions

Stylesheets live in `ui/src/assets/` and component-specific `.scss` files alongside components.

- Use the `pf-` prefix for all CSS classes (Perfetto namespace)
- Follow BEM-like naming: `.pf-component`, `.pf-component__element`, `.pf-component--modifier`
- Use CSS custom properties (variables) defined in `theme_provider.scss` for colors
- Support both light and dark themes using semantic color variables

## Common Pitfalls to Avoid

1. **Don't create new widgets without checking existing ones** - The widget library is comprehensive.
2. **Try to use the Trace object as much as possible** - Plumb the Trace object through the hierarchy wherever needed.
3. **Don't fetch async data in `oninit`/`onupdate` or DOM events (`onclick`, `onkeydown`)** - Loading should be a declarative product of state; update state and let `AsyncMemo` in `view()` handle fetching via the redraw mechanism.
4. **Disposing `AsyncMemo` instances in `onremove()` is optional** - You can call `.dispose()` to cancel pending tasks and clean up cached disposable resources eagerly when the component unmounts.
5. **Never instantiate `AsyncMemo` inside `view()`** - It must be created in the component setup closure or class constructor/field so its cache persists across render cycles.

## Code Review Pet Peeves and Style Preferences

The following patterns are consistently enforced during code review. Adhering to these will significantly speed up the review process.

> **See also: [UI Review Antipatterns](ui-review-antipatterns.md)** — a deeper,
> categorized catalogue of antipatterns mined from real maintainer review
> feedback on contributor PRs (Mithril/rendering, state, layering, plugin/API
> design, widgets, CSS, types, error handling, performance, PR scope). Consult it
> when authoring or reviewing UI changes to pre-empt recurring mistakes.

### TypeScript/JavaScript Style

**Prefer `undefined` over `null`:**
```typescript
// Bad
function getValue(): string | null { return null; }

// Good
function getValue(): string | undefined { return undefined; }
```

**Use `readonly T[]` for arrays that shouldn't be modified:**
```typescript
// Bad
function process(items: string[]): void { ... }

// Good
function process(items: readonly string[]): void { ... }
```

**Use `classNames()` utility for building CSS class strings:**
```typescript
import {classNames} from '../base/classnames';

// Bad
const cls = 'pf-row' + (isSelected ? ' pf-row--selected' : '') + (isDisabled ? ' pf-row--disabled' : '');

// Good
const cls = classNames('pf-row', isSelected && 'pf-row--selected', isDisabled && 'pf-row--disabled');
```

**Use `assertUnreachable()` in switch default cases:**
```typescript
import {assertUnreachable} from '../base/logging';

switch (value) {
  case 'a': return handleA();
  case 'b': return handleB();
  default:
    assertUnreachable(value); // TypeScript will error if cases aren't exhaustive
}
```

**Variables should be camelCase:**
```typescript
// Bad
const trace_processor_id = 123;

// Good
const traceProcessorId = 123;
```

### CSS/SCSS Style

**Never use inline styles - use stylesheets:**
```typescript
// Bad
m('div', {style: {color: 'red', padding: '10px'}}, 'content')

// Good
m('.pf-my-component', 'content') // with styles in .scss file
```

**All CSS classes must have the `pf-` prefix:**
```scss
// Bad
.my-component { ... }
.row { ... }

// Good
.pf-my-component { ... }
.pf-my-component__row { ... }
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
  color: var(--pf-color-foreground);
  background: var(--pf-color-background);
}
```

### Mithril-Specific Rules

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

### Widget Usage

**Use the `Anchor` widget for links:**
```typescript
import {Anchor} from '../widgets/anchor';
import {Icons} from '../widgets/icons';

// Bad
m('a', {href: 'https://example.com', target: '_blank'}, 'Link')

// Good
m(Anchor, {href: 'https://example.com', icon: Icons.ExternalLink}, 'Link')
```

### Naming Conventions

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


### UI Unit Tests

Unit tests are run with:
```sh
$ui/run-unittests
```

TypeScript unit tests follow the pattern `*_unittest.ts` and use Jest.

### UI Integration Tests

Integration tests use Playwright:
```sh
ui/run-integrationtests
```

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
ui/build    # Builds the UI
ui/run-dev-server    # Starts the development server with live reload
```

The UI uses:

- **TypeScript** for type safety
- **Mithril** as the UI framework
- **Rollup** for bundling
- **pnpm** for package management
- **ESLint** for linting (based on Google style)
- **Playwright** for integration tests

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

## Track creation

Rarely you need to create a new Track from scrach.
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

## Code Review Pet Peeves and Style Preferences

The following patterns are consistently enforced during code review. Adhering to these will significantly speed up the review process.

### TypeScript/JavaScript Style

**Prefer `undefined` over `null`:**
```typescript
// Bad
function getValue(): string | null { return null; }

// Good
function getValue(): string | undefined { return undefined; }
```

**Use `ReadonlyArray<T>` for arrays that shouldn't be modified:**
```typescript
// Bad
function process(items: string[]): void { ... }

// Good
function process(items: ReadonlyArray<string>): void { ... }
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

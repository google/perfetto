# Commands Automation Reference

This page documents Perfetto UI's stable command surface specifically for
automation use cases. These commands have backwards compatibility guarantees and
can be safely used in automated workflows, startup configurations, macros, and
deep linking.

## Overview

While Perfetto UI uses commands internally for all user interactions, this
reference focuses exclusively on the subset of commands that are stable for
automation purposes. These stable automation commands are designed for:

- **Startup commands** - Automatically configure the UI when loading a trace
- **Macros** - Create reusable workflows for complex analysis tasks
- **Deep linking** - Share pre-configured views via URLs or postMessage

Commands outside this automation reference are internal implementation details
with no backwards compatibility guarantees and may change without warning.

## Backwards Compatibility Guarantees

For automation commands listed in this reference, Perfetto UI guarantees:

- **Stable command IDs** - The command identifier (e.g.,
  `dev.perfetto.RunQuery`) will not change
- **Stable required arguments** - Existing required parameters will continue to
  work with the same semantics
- **Consistent behavior** - Core functionality will be preserved across updates
- **Advance notice of changes** - Any breaking changes will be:
  - Published in the CHANGELOG
  - Announced at least 6 months before the change takes effect

Optional parameters may be added to commands without notice, but will not affect
existing usage.

## Command Reference

### Track Manipulation Commands

These commands control how tracks are displayed in the timeline view.

#### `dev.perfetto.PinTracksByRegex`

Pins tracks matching a regular expression pattern to the top of the timeline.

**Arguments:**

- `pattern` (string, required): Regular expression to match track names or paths
- `nameOrPath` (string, optional): Whether to match against track names ("name")
  or track paths ("path"). Defaults to "name"

**Track names vs paths:**

- Track name: `"RenderThread"`
- Track path: `"com.example.app > RenderThread"`

**Example:**

```json
{
  "id": "dev.perfetto.PinTracksByRegex",
  "args": [".*surfaceflinger.*"]
}
```

**Example with track path filtering:**

```json
{
  "id": "dev.perfetto.PinTracksByRegex",
  "args": [".*com\\.example\\.app.*RenderThread.*", "path"]
}
```

**Common patterns:**

- Pin CPU tracks: `".*CPU \\d+$"`
- Pin specific process: `".*com\\.example\\.app.*"`
- Pin multiple processes: `".*(system_server|surfaceflinger).*"`

---

#### `dev.perfetto.ExpandTracksByRegex`

Expands track groups matching a regular expression pattern.

**Arguments:**

- `pattern` (string, required): Regular expression to match track group names or
  paths
- `nameOrPath` (string, optional): Whether to match against track names ("name")
  or track paths ("path"). Defaults to "name"

**Track names vs paths:**

- Track name: `"RenderThread"`
- Track path: `"com.example.app > RenderThread"`

**Example:**

```json
{
  "id": "dev.perfetto.ExpandTracksByRegex",
  "args": [".*system_server.*"]
}
```

**Example with track path filtering:**

```json
{
  "id": "dev.perfetto.ExpandTracksByRegex",
  "args": [".*system_server.*RenderThread.*", "path"]
}
```

---

#### `dev.perfetto.CollapseTracksByRegex`

Collapses track groups matching a regular expression pattern.

**Arguments:**

- `pattern` (string, required): Regular expression to match track group names or
  paths
- `nameOrPath` (string, optional): Whether to match against track names ("name")
  or track paths ("path"). Defaults to "name"

**Track names vs paths:**

- Track name: `"RenderThread"`
- Track path: `"com.example.app > RenderThread"`

**Example:**

```json
{
  "id": "dev.perfetto.CollapseTracksByRegex",
  "args": ["CPU Scheduling"]
}
```

**Example with track path filtering:**

```json
{
  "id": "dev.perfetto.CollapseTracksByRegex",
  "args": [".*com\\.example\\.app.*", "path"]
}
```

**Tip:** Use `".*"` to collapse all tracks as a starting point for focused
analysis.

### Debug Track Commands

Create custom visualization tracks from SQL queries. Debug tracks are overlaid
on the timeline and update automatically when the view changes.

**Important:** If your queries use Perfetto modules (e.g., `android.screen_state`,
`android.memory.lmk`), you must first execute a `RunQuery` command with the module
include statement before creating the debug track. The module include must come
first in the command sequence.

#### `dev.perfetto.AddDebugSliceTrack`

Creates a slice track from a SQL query returning time intervals.

**Arguments:**

1. `query` (string, required): SQL query that must return:
   - `ts` (number): Timestamp in nanoseconds
   - `dur` (number): Duration in nanoseconds
   - `name` (string): Slice name to display
2. `title` (string, required): Display name for the track

**Example:**

```json
{
  "id": "dev.perfetto.AddDebugSliceTrack",
  "args": [
    "SELECT ts, dur, name FROM slice WHERE dur > 10000000 ORDER BY dur DESC LIMIT 100",
    "Long Slices (>10ms)"
  ]
}
```

---

#### `dev.perfetto.AddDebugSliceTrackWithPivot`

Creates multiple slice tracks grouped by a pivot column. Each unique value in
the pivot column gets its own track.

**Arguments:**

1. `query` (string, required): SQL query that must return:
   - `ts` (number): Timestamp in nanoseconds
   - `dur` (number): Duration in nanoseconds
   - `name` (string): Slice name to display
   - Additional column for pivoting
2. `pivotColumn` (string, required): Column name to group tracks by
3. `title` (string, required): Base title for the track group

**Example:**

```json
{
  "id": "dev.perfetto.AddDebugSliceTrackWithPivot",
  "args": [
    "SELECT ts, dur, name, IFNULL(category, '[NULL]') as category FROM slice WHERE dur > 1000000",
    "category",
    "Slices by Category"
  ]
}
```

**Note:** Use `IFNULL()` to handle NULL values in the pivot column, as NULLs
will cause the command to fail.

---

#### `dev.perfetto.AddDebugCounterTrack`

Creates a counter track from a SQL query returning time-series data.

**Arguments:**

1. `query` (string, required): SQL query that must return:
   - `ts` (number): Timestamp in nanoseconds
   - `value` (number): Counter value
2. `title` (string, required): Display name for the track

**Example:**

```json
{
  "id": "dev.perfetto.AddDebugCounterTrack",
  "args": ["SELECT ts, value FROM counter WHERE track_id = 42", "Memory Usage"]
}
```

---

#### `dev.perfetto.AddDebugCounterTrackWithPivot`

Creates multiple counter tracks grouped by a pivot column.

**Arguments:**

1. `query` (string, required): SQL query that must return:
   - `ts` (number): Timestamp in nanoseconds
   - `value` (number): Counter value
   - Additional column for pivoting
2. `pivotColumn` (string, required): Column name to group tracks by
3. `title` (string, required): Base title for the track group

**Example:**

```json
{
  "id": "dev.perfetto.AddDebugCounterTrackWithPivot",
  "args": [
    "SELECT ts, value, name FROM counter JOIN counter_track ON counter.track_id = counter_track.id",
    "name",
    "System Counters"
  ]
}
```

### Workspace Commands

Workspaces allow you to create custom views of your trace data by organizing
specific tracks together.

#### `dev.perfetto.CreateWorkspace`

Creates a new empty workspace.

**Arguments:**

- `title` (string, required): Name for the new workspace

**Example:**

```json
{
  "id": "dev.perfetto.CreateWorkspace",
  "args": ["Memory Analysis"]
}
```

---

#### `dev.perfetto.SwitchWorkspace`

Switches to an existing workspace by name.

**Arguments:**

- `title` (string, required): Name of the workspace to switch to

**Example:**

```json
{
  "id": "dev.perfetto.SwitchWorkspace",
  "args": ["Memory Analysis"]
}
```

**Note:** The workspace must exist before switching to it.

---

#### `dev.perfetto.CopyTracksToWorkspaceByRegex`

Copies tracks matching a pattern to a workspace.

**Arguments:**

1. `pattern` (string, required): Regular expression to match track names or
   paths
2. `workspaceTitle` (string, required): Target workspace name
3. `nameOrPath` (string, optional): Whether to match against track names
   ("name") or track paths ("path"). Defaults to "name"

**Track names vs paths:**

- Track name: `"RenderThread"`
- Track path: `"com.example.app > RenderThread"`

**Example:**

```json
{
  "id": "dev.perfetto.CopyTracksToWorkspaceByRegex",
  "args": ["(Expected|Actual) Timeline", "Frame Analysis"]
}
```

**Example with track path filtering:**

```json
{
  "id": "dev.perfetto.CopyTracksToWorkspaceByRegex",
  "args": [".*com\\.example\\.app.*RenderThread.*", "Frame Analysis", "path"]
}
```

---

#### `dev.perfetto.CopyTracksToWorkspaceByRegexWithAncestors`

Copies tracks matching a pattern to a workspace, including their parent track
groups for context.

**Arguments:**

1. `pattern` (string, required): Regular expression to match track names or
   paths
2. `workspaceTitle` (string, required): Target workspace name
3. `nameOrPath` (string, optional): Whether to match against track names
   ("name") or track paths ("path"). Defaults to "name"

**Track names vs paths:**

- Track name: `"RenderThread"`
- Track path: `"com.example.app > RenderThread"`

**Example:**

```json
{
  "id": "dev.perfetto.CopyTracksToWorkspaceByRegexWithAncestors",
  "args": ["RenderThread", "Rendering Analysis"]
}
```

**Example with track path filtering:**

```json
{
  "id": "dev.perfetto.CopyTracksToWorkspaceByRegexWithAncestors",
  "args": [
    ".*com\\.example\\.app.*RenderThread.*",
    "Rendering Analysis",
    "path"
  ]
}
```

### Query Commands

#### `dev.perfetto.RunQuery`

Executes a PerfettoSQL query without displaying results.

**Arguments:**

- `query` (string, required): PerfettoSQL query to execute

**Example:**

```json
{
  "id": "dev.perfetto.RunQuery",
  "args": [
    "CREATE PERFETTO FUNCTION my_func(x INT) RETURNS INT AS SELECT $x * 2"
  ]
}
```

#### `dev.perfetto.RunQueryAndShowTab`

Executes a PerfettoSQL query and displays results in a new query tab.

**Arguments:**

- `query` (string, required): PerfettoSQL query to execute

**Example:**

```json
{
  "id": "dev.perfetto.RunQueryAndShowTab",
  "args": ["SELECT ts, dur, name FROM slice LIMIT 50"]
}
```

### Macro Commands

Macros are user-defined sequences of commands that execute in order. They
provide a way to automate complex, multi-step analysis workflows.

#### User-defined Macros

Macros can be defined through the UI settings and automatically get stable
command IDs.

**Command Pattern:**

- `dev.perfetto.UserMacro.{macroName}` - Executes a user-defined macro

**Arguments:**

None (macro commands and arguments are pre-configured)

**Example:**

```json
{
  "id": "dev.perfetto.UserMacro.MyAnalysisWorkflow",
  "args": []
}
```

**Notes:**

- Each macro contains a sequence of commands that execute in order
- When used as startup commands, all commands within the macro must also be
  allowlisted
- Macros can include any stable automation command from this reference
- Failed commands within a macro are logged but don't stop execution of
  remaining commands

---

## Using Commands for Automation

These stable automation commands can be used in several contexts:

- **Startup Commands** - Automatically run when loading traces. See
  [Startup Commands](/docs/visualization/ui-automation.md#commands-system-overview)
  in the UI automation guide.
- **Macros** - Named command sequences for on-demand execution. See
  [Macros](/docs/visualization/ui-automation.md#commands-system-overview) in the
  UI automation guide.
- **URL Deep Linking** - Embed commands in URLs or postMessage. See
  [Deep Linking](/docs/visualization/deep-linking-to-perfetto-ui.md#configuring-the-ui-with-startup-commands)
  for URL patterns and postMessage integration.

For practical automation examples and recipes, see the
[UI Automation guide](/docs/visualization/ui-automation.md).

## Requesting New Stable Automation Commands

To request a command be added to the stable automation surface:

1. File an issue at https://github.com/google/perfetto/issues
2. Include:
   - The command ID you need stabilized
   - Your use case and why stability is important
   - Example usage showing how you plan to use it

Commands are prioritized based on:

- Frequency of use in automation scenarios
- Importance for common analysis workflows
- Feasibility of maintaining backwards compatibility

## See Also

- [UI Automation guide](/docs/visualization/ui-automation.md) - Practical
  recipes using these commands
- [Perfetto UI Guide](/docs/visualization/perfetto-ui.md) - General UI
  documentation including commands
- [Deep Linking](/docs/visualization/deep-linking-to-perfetto-ui.md) - Opening
  traces with pre-configured commands

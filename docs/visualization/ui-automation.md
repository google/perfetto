# UI Automation

This page covers how to automate common Perfetto UI tasks to speed up your trace
analysis workflow using commands, startup commands, and macros.

## Commands System Overview

### Commands

**Commands** are individual UI actions that can be triggered manually or
automatically. Examples include pinning tracks, running queries, or creating
debug tracks. Access commands through:

- Command palette: `Ctrl-Shift-P` (Windows/Linux) or `Cmd-Shift-P` (Mac)
- Omnibox: Type `>` to transform it into a command palette
- Commands support fuzzy matching and autocomplete

### Startup Commands

**Startup commands** run automatically every time you open any trace. Configure
them in **Settings > Startup Commands** to set up your preferred view
immediately. These affect only the UI display - the trace file itself is
unchanged.

#### JSON Schema

Startup commands must be a JSON array of command objects:

```typescript
[
  {
    "id": string,      // Command identifier
    "args": unknown[]  // Array of arguments (types depend on the commands)
  },
  ...
]
```

#### Notes

- Commands execute in the order specified
- Invalid JSON or unknown command IDs will cause errors
- These commands affect only the UI display - the trace file is unchanged

### Macros

**Macros** are named sequences of commands you trigger manually when needed.
Configure them in **Settings > Macros** and run them via the command palette
(`Ctrl-Shift-P` and then type `>macro name`). Use macros for analysis workflows
you run occasionally rather than always.

#### JSON Schema

Macros must be a JSON object with macro names as keys and command arrays as
values:

```typescript
{
  "macro_name": [
    {
      "id": string,      // Command identifier
      "args": unknown[]  // Array of arguments (types depend on the command)
    },
    ...
  ],
  ...
}
```

#### Notes

- Macro names must be valid JSON string keys. Simple names without special
  characters are recommended for easier use in the command palette.
- Run macros by typing `>macro name` in the command palette (e.g.,
  `>CPU Analysis`)
- Commands in a macro execute sequentially

### Common Issues

- **JSON syntax errors**: Missing commas, trailing commas, or unescaped quotes
- **Invalid command IDs**: Use autocomplete in the command palette to find valid
  IDs
- **Wrong argument types**: All arguments must be strings, even numbers
- **Wrong argument count**: Each command expects a specific number of arguments
- **Module dependency errors**: If your debug track query uses Perfetto modules
  (e.g., `android.screen_state`), you must include a `RunQuery` command with the
  module include statement before the debug track command. The module include
  must come first in the command sequence.

## Startup Command Examples

### Pin important tracks automatically

Add this to **Settings > Startup Commands** to always pin CPU tracks when
opening traces:

```json
[
  {
    "id": "dev.perfetto.PinTracksByRegex",
    "args": [".*CPU [0-3].*"]
  }
]
```

This runs every time you open a trace, ensuring your most important tracks are
always visible.

### Create debug tracks for custom metrics

This startup command creates a debug track showing thread scheduling latency:

```json
[
  {
    "id": "dev.perfetto.AddDebugSliceTrack",
    "args": [
      "SELECT ts, thread.name, dur FROM thread_state JOIN thread USING(utid) WHERE state = 'R' AND dur > 1000000",
      "Long Scheduling Delays (>1ms)"
    ]
  }
]
```

### Debug tracks using Perfetto modules

When your query uses Perfetto modules (like `android.screen_state` or
`android.memory.lmk`), you must include the module first as a separate command.
**Important: The module include command must come before the query that uses
it.**

```json
[
  {
    "id": "dev.perfetto.RunQuery",
    "args": ["include perfetto module android.screen_state"]
  },
  {
    "id": "dev.perfetto.AddDebugSliceTrack",
    "args": [
      "SELECT ts, dur FROM android_screen_state WHERE simple_screen_state = 'on'",
      "Screen On Events"
    ]
  }
]
```

Another example with memory LMK events:

```json
[
  {
    "id": "dev.perfetto.RunQuery",
    "args": ["include perfetto module android.memory.lmk"]
  },
  {
    "id": "dev.perfetto.AddDebugSliceTrackWithPivot",
    "args": [
      "SELECT ts, process_name as name, 0 as dur FROM android_lmk_events",
      "name",
      "LMK Events by Process"
    ]
  }
]
```

Debug tracks visualize SQL query results on the timeline. The query must return:

- `ts` (timestamp)
- For slice tracks: `dur` (duration)
- For counter tracks: `value` (the metric value)
- Optional pivot column. The query results will be grouped by the unique values
  in this column, with each group appearing in its own track.

**Command argument patterns:**

- Without pivot: `AddDebugSliceTrack` - [query, title]
- With pivot: `AddDebugSliceTrackWithPivot` - [query, pivot_column, title]

### Standard analysis setup

This comprehensive startup configuration prepares the UI for system analysis:

```json
[
  {
    "id": "dev.perfetto.CollapseTracksByRegex",
    "args": [".*"]
  },
  {
    "id": "dev.perfetto.PinTracksByRegex",
    "args": [".*CPU \\d+$"]
  },
  {
    "id": "dev.perfetto.ExpandTracksByRegex",
    "args": [".*freq.*"]
  },
  {
    "id": "dev.perfetto.AddDebugSliceTrackWithPivot",
    "args": [
      "SELECT ts, blocked_function as name, dur FROM thread_state WHERE state = 'D' AND blocked_function IS NOT NULL",
      "name",
      "Blocking Functions"
    ]
  }
]
```

## Macro Examples

### Focus on specific subsystem

Add this to **Settings > Macros** for analyzing memory when needed. This macro
creates a new workspace to isolate memory-related tracks from the main view:

```json
{
  "Memory Analysis": [
    {
      "id": "dev.perfetto.CreateWorkspace",
      "args": ["Memory Analysis"]
    },
    {
      "id": "dev.perfetto.CopyTracksToWorkspaceByRegexWithAncestors",
      "args": [".*mem.*|.*rss.*", "Memory Analysis"]
    },
    {
      "id": "dev.perfetto.SwitchWorkspace",
      "args": ["Memory Analysis"]
    },
    {
      "id": "dev.perfetto.AddDebugCounterTrackWithPivot",
      "args": [
        "SELECT ts, process.name as process, value FROM counter JOIN process_counter_track ON counter.track_id = process_counter_track.id JOIN process USING (upid) WHERE counter.name = 'mem.rss' AND value > 50000000",
        "process",
        "High Memory Processes (>50MB)"
      ]
    }
  ]
}
```

Run this macro by typing `>Memory Analysis` in the command palette when you need
to investigate memory issues.

### Latency investigation

This macro helps identify performance bottlenecks:

```json
{
  "Find Latency": [
    {
      "id": "dev.perfetto.PinTracksByRegex",
      "args": [".*CPU.*"]
    },
    {
      "id": "dev.perfetto.RunQueryAndShowTab",
      "args": [
        "SELECT thread.name, COUNT(*) as blocks, SUM(dur)/1000000 as total_ms FROM thread_state JOIN thread USING(utid) WHERE state = 'D' GROUP BY thread.name ORDER BY total_ms DESC LIMIT 10"
      ]
    },
    {
      "id": "dev.perfetto.AddDebugSliceTrackWithPivot",
      "args": [
        "SELECT ts, 'blocked' as name, thread.name as thread_name, dur FROM thread_state JOIN thread USING (utid) WHERE state IN ('R', 'D+') AND dur > 5000000",
        "thread_name",
        "Long Waits (>5ms)"
      ]
    }
  ]
}
```

## Combining with trace recording

When recording traces, you can specify startup commands that will execute when
the trace opens in the UI:

```bash
# These commands affect only the UI view when the trace opens from the script.
./record_android_trace \
  --app com.example.app \
  --ui-startup-commands '[
    {"id":"dev.perfetto.PinTracksByRegex","args":[".*CPU.*"]},
    {"id":"dev.perfetto.AddDebugSliceTrackWithPivot","args":["SELECT ts, thread.name, dur FROM thread_state JOIN thread USING(utid) WHERE state = \"R\"","thread","Runnable Time"]}
  ]'
```

## Tips for effective automation

1. **Use startup commands for always-needed views**: If you always want certain
   tracks pinned, use startup commands.

2. **Use macros for specific investigations**: Create macros for workflows you
   run occasionally (memory analysis, latency hunting, etc.).

3. **Test interactively first**: Use the command palette (`Ctrl/Cmd+Shift+P`) to
   test commands before adding to settings. Type commands to see available
   options with autocomplete.

4. **Start clean**: Begin command sequences with `CollapseTracksByRegex` using
   `".*"` to collapse all tracks first.

5. **Common regex patterns**:

   - Escape dots in package names: `"com\\.example\\.app"`
   - Match any digit: `\\d+`
   - Match beginning/end: `^` and `$`

6. **Debug tracks need good queries**: Ensure your SQL returns `ts` and either
   `dur` (for slices) or `value` (for counters) columns. Use the pivot commands
   to split into multiple tracks. For Android use cases, see
   [Android Trace Analysis Cookbook](/docs/getting-started/android-trace-analysis.md)
   for examples of common queries used by Android engineers.

## See Also

- [Commands Automation Reference](/docs/visualization/commands-automation-reference.md) -
  Complete reference for stable automation commands with backwards compatibility
  guarantees
- [Perfetto UI Guide](/docs/visualization/perfetto-ui.md) - General UI
  documentation including commands configuration
- [Deep Linking](/docs/visualization/deep-linking-to-perfetto-ui.md) - Opening
  traces with pre-configured commands via URLs

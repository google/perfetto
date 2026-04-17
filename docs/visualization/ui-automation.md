# Commands and Macros

This page covers how to automate common Perfetto UI tasks using commands,
startup commands, and macros. For an overview of all ways to extend the UI, see
[Extending the UI](/docs/visualization/extending-the-ui.md).

## Running commands

Commands are individual UI actions — pin a track, run a query, create a debug
track. Run them through:

- **Command palette:** `Ctrl+Shift+P` (Windows/Linux) or `Cmd+Shift+P` (Mac)
- **Omnibox:** Type `>` to transform it into a command palette

Commands support fuzzy matching and autocomplete. See the
[Commands Automation Reference](/docs/visualization/commands-automation-reference.md)
for the full list of stable commands.

## Setting up startup commands

Startup commands run automatically every time you open any trace. Configure them
in **Settings > Startup Commands**.

Startup commands are a JSON array of command objects:

```json
[
  {"id": "command.id", "args": ["arg1", "arg2"]}
]
```

Commands execute in order. These affect only the UI display — the trace file
is unchanged.

### Pin important tracks automatically

```json
[
  {
    "id": "dev.perfetto.PinTracksByRegex",
    "args": [".*CPU [0-3].*"]
  }
]
```

### Create debug tracks for custom metrics

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

### Use Perfetto SQL modules in debug tracks

When your query uses Perfetto modules, include the module first as a separate
command:

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

Debug tracks visualize SQL query results on the timeline. The query must return:

- `ts` (timestamp)
- For slice tracks: `dur` (duration)
- For counter tracks: `value` (the metric value)
- Optional pivot column — results are grouped by unique values, each in its own
  track.

**Command argument patterns:**

- Without pivot: `AddDebugSliceTrack` — `[query, title]`
- With pivot: `AddDebugSliceTrackWithPivot` — `[query, pivot_column, title]`

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

## Creating macros

Macros are named sequences of commands you trigger manually from the command
palette. Configure them in **Settings > Macros**.

Macros are a JSON array of macro objects:

```json
[
  {
    "id": "user.example.MacroName",
    "name": "Display Name",
    "run": [
      {"id": "command.id", "args": ["arg1"]}
    ]
  }
]
```

- **id**: Unique identifier. Use reverse-domain naming (e.g.,
  `user.myteam.MemoryAnalysis`). Keep IDs stable — they are used when
  referencing macros from startup commands.
- **name**: Display name shown in the command palette. Can contain spaces.
- **run**: Commands to execute in sequence.

Run macros by typing `>name` in the command palette (e.g., `>Memory Analysis`).

> **Note (Migration):** The macros format was changed from a dictionary to an
> array structure. If you had existing macros, they were automatically migrated
> to the new format. The migrated macros use IDs in the format
> `dev.perfetto.UserMacro.<old_name>`.

### Focus on a specific subsystem

This macro creates a workspace to isolate memory-related tracks:

```json
[
  {
    "id": "user.example.MemoryAnalysis",
    "name": "Memory Analysis",
    "run": [
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
]
```

### Investigate latency

This macro helps identify performance bottlenecks:

```json
[
  {
    "id": "user.example.FindLatency",
    "name": "Find Latency",
    "run": [
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
]
```

## Combining with trace recording

When recording traces, specify startup commands that run when the trace opens:

```bash
./record_android_trace \
  --app com.example.app \
  --ui-startup-commands '[
    {"id":"dev.perfetto.PinTracksByRegex","args":[".*CPU.*"]},
    {"id":"dev.perfetto.AddDebugSliceTrackWithPivot","args":["SELECT ts, thread.name, dur FROM thread_state JOIN thread USING(utid) WHERE state = \"R\"","thread","Runnable Time"]}
  ]'
```

## Tips

1. **Startup commands for always-needed views.** If you always want certain
   tracks pinned, use startup commands.

2. **Macros for specific investigations.** Create macros for workflows you run
   occasionally (memory analysis, latency hunting, etc.).

3. **Test interactively first.** Use the command palette (`Ctrl/Cmd+Shift+P`)
   to try commands before adding them to settings.

4. **Start clean.** Begin command sequences with `CollapseTracksByRegex` using
   `".*"` to collapse all tracks first.

5. **Common regex patterns:**
   - Escape dots in package names: `"com\\.example\\.app"`
   - Match any digit: `\\d+`
   - Match beginning/end: `^` and `$`

6. **Debug tracks need good queries.** Ensure SQL returns `ts` and either `dur`
   (for slices) or `value` (for counters). For Android use cases, see
   [Android Trace Analysis Cookbook](/docs/getting-started/android-trace-analysis.md).

## Sharing with your team

If you want to share macros and SQL modules with others rather than maintaining
them locally, use
[Extension Servers](/docs/visualization/extension-servers.md).

## Common issues

- **JSON syntax errors**: Missing commas, trailing commas, or unescaped quotes.
- **Invalid command IDs**: Use autocomplete in the command palette to find valid
  IDs, or see the
  [Commands Automation Reference](/docs/visualization/commands-automation-reference.md).
- **Wrong argument types**: All arguments must be strings, even numbers.
- **Wrong argument count**: Each command expects a specific number of arguments.
- **Module dependency errors**: If your debug track query uses Perfetto modules
  (e.g., `android.screen_state`), include the module first with a `RunQuery`
  command.

## See also

- [Extending the UI](/docs/visualization/extending-the-ui.md) — Overview of all
  extension mechanisms
- [Commands Automation Reference](/docs/visualization/commands-automation-reference.md) —
  Full reference for stable automation commands
- [Extension Servers](/docs/visualization/extension-servers.md) — Share macros
  and SQL modules with your team
- [Perfetto UI Guide](/docs/visualization/perfetto-ui.md) — General UI
  documentation
- [Deep Linking](/docs/visualization/deep-linking-to-perfetto-ui.md) — Opening
  traces with pre-configured commands via URLs

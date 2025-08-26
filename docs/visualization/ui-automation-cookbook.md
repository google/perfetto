# Cookbook: UI Automation and Productivity

This page contains practical recipes for automating common Perfetto UI tasks to
speed up your trace analysis workflow.

## What are commands and macros?

**Commands** are individual UI actions that can be triggered manually or
automatically. Examples include pinning tracks, running queries, or creating
debug tracks. These can be run manually using the command palette
(`Ctrl-Shift-P` on Windows/Linux, `Cmd-Shift-P` on Mac). You can discover
available commands by typing in the command palette and using autocomplete.

**Startup commands** are commands that run automatically every time you open any
trace. Configure them in **Settings > Startup Commands** to set up your
preferred view immediately. These affect only the UI display - the trace file
itself is unchanged. These are useful when you open traces and almost always
perform the same sort of actions on startup. For JSON schema details, see
[Startup Commands](/docs/visualization/perfetto-ui.md#startup-commands).

**Macros** are named sequences of commands that you trigger manually when
needed. Configure them in **Settings > Macros** and run them via the command
palette (`Ctrl-Shift-P` and then type `>macro name`). Use macros for analysis
workflows you run occasionally rather than always. For JSON schema details, see
[Macros](/docs/visualization/perfetto-ui.md#macros).

For detailed configuration instructions and JSON schema, see the
[Commands section](/docs/visualization/perfetto-ui.md#commands) in the Perfetto
UI guide.

## Startup Commands: Automatic setup on trace load

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
      "SELECT ts, thread.name as thread_name, dur as value FROM thread_state JOIN thread USING(utid) WHERE state = 'R' AND dur > 1000000",
      "Long Scheduling Delays (>1ms)"
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
      "SELECT ts, blocked_function as name, dur as value FROM thread_state WHERE state = 'D' AND blocked_function IS NOT NULL",
      "name",
      "Blocking Functions"
    ]
  }
]
```

## Macros: On-demand analysis workflows

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
      "id": "dev.perfetto.RunQuery",
      "args": [
        "SELECT thread.name, COUNT(*) as blocks, SUM(dur)/1000000 as total_ms FROM thread_state JOIN thread USING(utid) WHERE state = 'D' GROUP BY thread.name ORDER BY total_ms DESC LIMIT 10"
      ]
    },
    {
      "id": "dev.perfetto.AddDebugSliceTrackWithPivot",
      "args": [
        "SELECT ts, thread.name as thread_name, dur as value FROM thread_state JOIN thread USING (utid) WHERE state IN ('R', 'D+') AND dur > 5000000",
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
    {"id":"dev.perfetto.AddDebugSliceTrackWithPivot","args":["SELECT ts, thread.name as thread, dur as value FROM thread_state JOIN thread USING(utid) WHERE state = \"R\"","thread","Runnable Time"]}
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

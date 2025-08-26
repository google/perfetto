# Perfetto UI

The [Perfetto UI](https://ui.perfetto.dev) enables you to view and analyze
traces in the browser. It supports several different tracing formats, including
the perfetto proto trace format and the legacy json trace format.

## Loading a Trace

Click one of the examples in the 'Example Traces' section of the taskbar to get
going.

Drag and drop a trace from your file explorer, or click 'Open trace file' in the
sidebar to open a local trace file.

## Navigating the Timeline

Use the WASD cluster to zoom and pan around the timeline. W and S zoom in and
out, and A and D pan left and right respectively.

<video width="800" controls>
  <source src="https://storage.googleapis.com/perfetto-misc/keyboard-nav.webm" type="video/webm">
</video>

Alternatively you can use Shit+Drag to pan using the mouse. Ctrl+MouseWheel
zooms in and out.

<video width="800" controls>
  <source src="https://storage.googleapis.com/perfetto-misc/mouse-nav.webm" type="video/webm">
</video>

## Track Event Selections

Selecting entities on the tace is the primary way to dig into events of a trace
and reveal more data about those events.

Select a track event by clicking on it. Details about the selected event will
appear in the 'Current Selection' tab in the tab drawer.

<video width="800" controls>
  <source src="https://storage.googleapis.com/perfetto-misc/select-event.webm" type="video/webm">
</video>

Use '.' and ',' to navigate between adjacent slices on the same track.

<video width="800" controls>
  <source src="https://storage.googleapis.com/perfetto-misc/next-prev-events.webm" type="video/webm">
</video>

Press 'F' to center the selected entity in the viewport, and press 'F' again to
fit that slice to the viewport. This can be useful for really short events that
cannot otherwise be seen clearly at the current zoom level.

<video width="800" controls>
  <source src="https://storage.googleapis.com/perfetto-misc/focus-event.webm" type="video/webm">
</video>

At any point, press 'escape' or click on some empty space in the timeline to
clear the selection.

## Area Selections

Click and drag over the timeline to make an area selection. An area selection
consists of a start + end time and a list of tracks. Click+drag on the markers
to move the start and end times. Check or uncheck the checkboxes in the track
shells to modify the list of tracks in the selection.

<video width="800" controls>
  <source src="https://storage.googleapis.com/perfetto-misc/area-selection.webm" type="video/webm">
</video>

You can also convert a single selection into an area selection using the 'R'
hotkey. This turns the currently selected track event in to an area selection
using the bounds of the selected event.

## Commands

Commands provide a quick way to run common tasks throughout the UI. Press
'Ctrl+Shift+P' ('Cmd+Shift+P' on Mac) to open the command palette, or by
entering '>' in the omnibox. The omnibox transforms into a command palette.
Commands can be searched using fuzzy matching. Press up or down to highlight a
command and Enter to run it.

<video width="800" controls>
  <source src="https://storage.googleapis.com/perfetto-misc/commands.webm" type="video/webm">
</video>

### Startup Commands

Startup commands run automatically every time you open a trace. Configure them
in **Settings > Startup Commands**. See the
[UI Automation Cookbook](/docs/visualization/ui-automation-cookbook.md) for
practical examples.

#### JSON Schema

Startup commands must be a JSON array of command objects:

```typescript
// Schema
[
  {
    "id": string,      // Command identifier
    "args": unknown[]  // Array of arguments (types depend on the commands)
  },
  ...
]
```

#### Example

```json
[
  {
    "id": "dev.perfetto.PinTracksByRegex",
    "args": [".*CPU [0-3].*"]
  },
  {
    "id": "dev.perfetto.AddDebugSliceTrack",
    "args": [
      "SELECT ts, dur as value FROM thread_state WHERE state = 'R' AND dur > 10000000",
      "Long Running Tasks (>10ms)"
    ]
  }
]
```

Notes:

- Commands execute in the order specified
- Invalid JSON or unknown command IDs will cause errors
- These commands affect only the UI display - the trace file is unchanged

### Macros

Macros are named sequences of commands you trigger manually. Configure them in
**Settings > Macros**.

#### JSON Schema

Macros must be a JSON object with macro names as keys and command arrays as
values:

```typescript
// Schema
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

#### Example

```json
{
  "CPU Analysis": [
    {
      "id": "dev.perfetto.CreateWorkspace",
      "args": ["CPU Analysis"]
    },
    {
      "id": "dev.perfetto.CopyTracksToWorkspaceByRegex",
      "args": [".*CPU.*|.*freq.*", "CPU Analysis"]
    },
    {
      "id": "dev.perfetto.SwitchWorkspace",
      "args": ["CPU Analysis"]
    }
  ],
  "Memory Debug": [
    {
      "id": "dev.perfetto.RunQuery",
      "args": ["SELECT * FROM memory_snapshot"]
    }
  ]
}
```

Notes:

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

For practical examples and recipes, see the
[UI Automation Cookbook](/docs/visualization/ui-automation-cookbook.md).

## Showing/hiding the tab drawer

Press 'Q' to toggle the tab drawer.

## Finding Tracks

Press 'Ctrl+P' (or 'Cmd+Shift+P on Mac) to open the track finder and start
typing to fuzzy find tracks.

<video width="800" controls>
  <source src="https://storage.googleapis.com/perfetto-misc/finding-tracks.webm" type="video/webm">
</video>

## Pinning Tracks

Press the 'Pin' icon in the track shell to pin a track to the top of the
timeline. This operation moves the track to the top of the workspace. This can
be handy if you want to keep important tracks in view while scrolling through
the main timeline.

<video width="800" controls>
  <source src="https://storage.googleapis.com/perfetto-misc/pinning-tracks.webm" type="video/webm">
</video>

## Hotkeys

Hotkey bindings are displayed to the right of the commands in the command
palette, or press the '?' hotkey to display all configured hotkeys.

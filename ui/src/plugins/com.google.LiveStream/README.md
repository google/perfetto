# com.google.LiveStream

Shows slices and counters streamed by a live server as a trace that grows while
you watch, e.g. the tasks, steps and errors of a running job. Any server that
implements the protocol below works.

The server serves this UI and opens it as

```
index.html?mode=embedded&hideSidebar=true&hideStatusbar=true&enablePlugins=com.google.LiveStream&com.google.LiveStream:target=<id>#!/viewer
```

The route arguments are in the query string because Perfetto rewrites the hash
after loading a trace. Without `com.google.LiveStream:target` the plugin does
nothing.

`com.google.LiveStream:hud` picks where the HUD shows: `panel`, the default, for
a side panel tab, or `top` for a row above the topbar. Other values mean
`panel`.

## How it works

1. `onActivate` shows the HUD for the page's lifetime: in a side panel tab
   (`app.sidePanel`), or with `hud=top` in a row of its own, first in `<body>`,
   whose top padding moves the app below the row. It fetches `api/targets` and
   the seed trace `api/seed`, and opens the seed, without caching it, with
   plugin args that carry the target and the seed's stream position.
2. `onTraceLoad` starts a `LiveSession`. Before the trace shows, it copies the
   history of the seed's marked tracks into SQL tables (`store.ts`), and
   replaces the seed's tracks with its own `SliceTrack`s and `CounterTrack`s.
   Once the trace is ready, it long-polls `api/stream` from the seed's position
   and writes the changes into the same tables.
3. Tracks load only the rows near the visible window: the window and its width
   on each side. They reload when the window leaves that range, at most every
   250 ms, and when updates bring rows into it, at most every 500 ms. So
   following costs the same with hours of history as with minutes.
4. Every frame, the visible window follows the playhead (`playhead.ts`): the
   server playhead, interpolated between responses, minus the buffer delay.

Running slices are stored with `dur = -1` and rendered as incomplete. The client
keeps finished slices and counter points for the History setting, 1 h by
default. Error slices stay regardless, up to the newest 10,000. Both hold per
trace: reopening the trace starts over from a new seed. The seed holds the
History too, in tables that aren't pruned, so memory holds up to twice the
History. A new `stream_id`, e.g. after a server restart, reopens the trace. The
seed's slice args aren't copied: the details panel reads them from the trace's
`args` table.

Picking another target reopens the trace in place, with the seed of that target,
and replaces `com.google.LiveStream:target` in the page's URL, so that reloading
the page opens the same target. A new pick replaces a pending one; Reconnect
restarts it, or reopens the open target.

While a trace opens, at page load, on a switch or reopen, also while the target
has no data yet, the HUD shows BUFFERING. While an open trace waits for data,
e.g. after a server restart, it adds the server's reason, if any. If the server
is unreachable or the open fails, it shows STREAM ERROR with the message. A
failed switch leaves the open trace and shows its error until dismissed or the
next open. A failed history load shows until dismissed; streaming then starts
without the history.

The minimap shows one row per slice track: the time the track spent in slices,
per cell, over the whole history. Area selections get a "Slices" tab, by name
with count and duration stats, where running slices last until the playhead, and
a "Counters" tab.

The details panel shows wall-clock times in the server's latest time zone, also
for slices from before a daylight saving time change.

## Protocol

HTTP GET requests, relative to the page. Responses are JSON in the proto JSON
encoding: field names as below, int64 values as strings, enums by name. Missing
fields take default values and unknown fields are ignored. Errors are non-2xx
responses, optionally with an `{"error": "<message>"}` body.

- `api/targets?target=<id>`: `targets`, the available targets; `selected`, the
  target `<id>`, or the default one without `target`; and `page_title`.
- `api/seed?target=<id>&history_sec=<s>`: the seed trace, see below, with the
  latest `history_sec` seconds of history. Until the target has data: 503, or
  502 with the error while the server fails to get data from it. The client
  shows BUFFERING or the error, and retries.
- `api/stream?target=<id>&stream_id=<id>&since_seq=<n>&window=<s>`: a stream
  response, see below, as soon as there are changes after `since_seq`, or after
  at most 10 s; up to 20 s when the server is busy.
- `api/download?target=<id>`: a Perfetto trace of the data the server holds. 503
  while too many downloads run; the client shows the error.

A target has an `id`, a `short_name`, a `description`, an http(s) `link` and an
`address`. `task_count` and `task_index` describe a task of a job, which is
addressed as `<id>/<task index>` and shown as `<short_name>/<task index>`, also
in single-task jobs.

The seed is a Perfetto trace of the server's history:

- Its clock snapshot maps ts 0 of the trace clock to `start_epoch_ns` in the
  REALTIME clock, and its system info sets the time zone in `timezone_off_mins`.
- Tracks with the description `com.google.LiveStream:depth=<d>;track_id=<id>`
  hold the points, or the slices at depth `<d>`, of stream track `<id>`. A slice
  track may span several of them. Their `sibling_order_rank` is the display
  order. Slice args are debug annotations. Other tracks aren't shown.
- The headers `X-Live-Stream-Id` and `X-Live-Seq` give the `stream_id` and `seq`
  that its data reach. The client then requests the changes after them. Without
  them, the client skips the seed's history and starts with a snapshot.

A stream response has:

- `stream_id`: identifies the server's stream state. Changes when that state is
  recreated, e.g. when the server restarts.
- `seq`: increases with every server update; 0 = no data yet.
- `full`: whether the response is a snapshot rather than a delta.
- `more`: whether the server holds more changes after `seq`. The client then
  requests them at once, without waiting for the update interval.
- `playhead_ns`: the end of the latest data.
- `start_epoch_ns`: the Unix time of ts 0, in ns.
- `tz_offset_min`, `tz_abbr`: the server's time zone, e.g. -420 and "PDT", for
  wall-clock times. Without `tz_abbr`, they show the offset, e.g. UTC-07:00.
- `tracks`: all tracks, in display order, with `id`, `name`, `kind`
  (`KIND_SLICE` or `KIND_COUNTER`) and `unit`. A new track is shown after the
  track listed before it.
- `completed_slices`: finished slices, with `key`, `track_id`, `ts_ns`,
  `dur_ns`, `depth`, `name`, `category` and `args` (`key`, `value`).
- `active_slices`: all running slices, with the same fields.
- `counter_points`: with `track_id`, `ts_ns` and `value`.
- `last_error`: the server's latest error, e.g. why it fails to get data from
  the target; empty while healthy. Shown as an error, also with `seq` 0.

Timestamps are in ns since ts 0. The client sends the `stream_id` and `seq` of
the latest response it applied. The server answers with a delta if `stream_id`
matches and it still holds all changes after `since_seq`, and with a snapshot of
the last `window` seconds otherwise:

- A delta has the slices completed and the points added after `since_seq`.
- A snapshot has all completed slices and points. Those never change within a
  stream, so the client keeps the ones it holds.
- Both have all tracks and active slices, which replace the previous ones.
- A slice keeps its `key` when it completes, and replaces the slice with the
  same key. Slices with category `error` are kept regardless of the History.

## Controls

The HUD is the "Live Stream" tab of the side panel, shown from page load; the
topbar's side panel button hides and shows it. With `hud=top`, it is a row above
the topbar instead, with Reconnect, Download Trace and the settings in its ⋯
menu. From top to bottom, or left to right in the row:

- Status badge: STREAM ERROR, PAUSED, BUFFERING or LIVE STREAM, in this order of
  precedence. Buffering: no data yet, or none for over 5 s, or a trace opens.
  Statuses other than live and paused show once they last 1 s.
- Popout: errors and buffering show a note under the badge. Dismissed (×), it
  stays hidden until the status changes; a click on the badge, or `Enter`, shows
  it again.
- Target menu: the open target. It opens with the target's name, e.g.
  `worker/3`, and filters the list once edited. Enter or Go opens the typed
  target: a name, an id, or `<name>/<task index>`. `↑`/`↓` pick a listed target
  for Enter instead.
- Follow Live (`Space`): follow the playhead, or stop following. Following
  resumes a pause; stopping doesn't. Zooming (`W`/`S`, Ctrl+wheel) keeps
  following at the new width, also while paused. Other moves of the timeline,
  e.g. panning or `F` on a slice, and selecting an area stop following. `Space`
  works while a button has the focus, but not in text boxes.
- Pause / Resume Live: stop or restart updates.
- Reconnect, Download Trace.
- Settings: Poll Interval, Buffer Window and History. They persist, also on the
  Settings page.

The command "Live Stream: Toggle Follow Live Scroll" has the `Space` hotkey.
Other traces, e.g. opened from a file, replace the HUD with a note, or hide the
row, and turn the command off.

## Embedding

The page that embeds the UI may restyle the HUD. Its colors are CSS variables:
`--pf-live-stream-hud-<name>` falls back to the theme's `--pf-color-<name>`, for
`background`, `background-secondary`, `text`, `text-muted`, `accent`,
`text-on-accent`, `success`, `warning` and `danger`.
`--pf-live-stream-hud-padding-left` (default 14px, 10px below 820 px wide) makes
room in the top row, e.g. for a logo over it; the side panel ignores it. The
plugin never sets them. E.g.:

```css
:root {
  --pf-live-stream-hud-padding-left: 78px;
  --pf-live-stream-hud-accent: #f4e04d;
}
```

Values set on `:root` must be literals: the theme's variables are defined only
within `.pf-theme-provider`.

The page may also follow the status that the HUD shows, also while the side
panel is closed: `<html>` has the attribute `data-live-stream-status`, with
`live`, `buffering`, `paused` or `error`, and none while another trace is open.
E.g. a loading screen over the UI may end on errors. The HUD's badge has the
class `pf-live-stream-hud__badge--<status>` too.

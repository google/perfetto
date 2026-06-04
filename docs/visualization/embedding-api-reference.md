# Perfetto UI embedding API reference

This page is a reference for the `postMessage` and URL parameter surface used to
embed the Perfetto UI (`ui.perfetto.dev`) inside an `<iframe>` on a host page.

For a task-oriented walkthrough of the embedding flow, see
[Embedding the Perfetto UI](/docs/visualization/embedding-the-ui.md). For the
`window.open()` (new browser tab) variant and for sharing / `appStateHash`
details, see [Deep linking to the Perfetto UI](/docs/visualization/deep-linking-to-perfetto-ui.md).

NOTE: This is a reference, not a tutorial. Fields and message types not listed
here are not part of the supported surface.

## Message channel

The host page communicates with the embedded UI via `window.postMessage`. The
UI's message handler only acts on messages whose `event.source` is one of:

| `event.source`            | When                                                              |
| ------------------------- | ---------------------------------------------------------------- |
| `window.parent`           | The UI runs inside the host's `<iframe>` (the embedding case).   |
| `window.opener`           | The host launched the UI with `window.open()` (new-tab case).    |
| A window this UI opened   | `event.source.opener === window`.                                |

For iframe embedding, the UI's `window.parent` is the host page, so the host
posts messages to `iframe.contentWindow` and the UI accepts them.

Because the channel is not buffered, a handshake is required before posting a
trace:

1. The host repeatedly posts the string `'PING'` to the UI window.
2. The UI replies with the string `'PONG'`. The reply is sent to `'*'` and is
   sent only once the UI's message listener is registered **and**
   `document.readyState === 'complete'`.
3. The host listens for `'message'` events; on the first `data === 'PONG'` from
   the UI window it stops pinging and posts the trace.

A robust host pings on an interval (for example every 50-250ms) and clears the
interval on the first `PONG`.

A message with `{perfettoIgnore: true}` is ignored on purpose. This lets a host
multiplex other traffic over the same channel.

## Opening a trace

To open a trace, post an object with a single `perfetto` key:

```js
iframe.contentWindow.postMessage({perfetto: {buffer, title}}, '*');
```

Fields of the `perfetto` object:

| Field          | Type                                                      | Required | Default | Meaning                                                                                                                              |
| -------------- | -------------------------------------------------------- | -------- | ------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `buffer`       | `ArrayBuffer`                                             | Yes      | -       | Raw trace bytes, e.g. from `fetch(...).then(r => r.arrayBuffer())`.                                                                  |
| `title`        | `string`                                                 | Yes      | -       | Trace title shown in the UI.                                                                                                          |
| `fileName`     | `string`                                                 | No       | -       | Suggested file name if the user downloads the trace.                                                                                 |
| `url`          | `string`                                                 | No       | -       | Sharing URL. See sharing details in [Deep linking to the Perfetto UI](/docs/visualization/deep-linking-to-perfetto-ui.md).          |
| `appStateHash` | `string`                                                 | No       | -       | 40-char hex hash; restores saved UI state from GCS. See [Deep linking to the Perfetto UI](/docs/visualization/deep-linking-to-perfetto-ui.md). |
| `localOnly`    | `boolean`                                                | No       | `true`  | Defaults to `true` for posted traces, which disables download and share. Set `false` to enable them.                                |
| `keepApiOpen`  | `boolean`                                                | No       | `false` | If `true`, the listener stays active so the host can post more traces later. If `false`/omitted, the handler removes its own message listener after the first trace (avoids duplicate posts, b/182502595). |
| `pluginArgs`   | `{[pluginId: string]: {[key: string]: unknown}}`         | No       | -       | Passed to plugins' `onTraceLoad()`.                                                                                                  |

### Bare ArrayBuffer shorthand

A bare `ArrayBuffer` (`event.data instanceof ArrayBuffer`) is also accepted. It
is treated as `{title: 'External trace', buffer}`.

## Scroll to time range

Post the following message after the trace is loaded to scroll/zoom the viewport
to a time range:

```js
iframe.contentWindow.postMessage(
    {perfetto: {timeStart, timeEnd, viewPercentage}}, '*');
```

| Field            | Type     | Required | Meaning                                          |
| ---------------- | -------- | -------- | ------------------------------------------------ |
| `timeStart`      | `number` | Yes      | Start of the range, in **seconds**.              |
| `timeEnd`        | `number` | Yes      | End of the range, in **seconds**.                |
| `viewPercentage` | `number` | No       | Fraction of the viewport the range should fill, in `(0.0, 1.0]`. Out-of-range values are ignored and replaced by `0.5`. |

The handler retries internally (roughly 20 times at 200ms intervals) until the
trace is ready, so this message can be posted shortly after the trace without
waiting for an explicit "loaded" signal.

## String commands

The handler understands these string messages:

| Message                 | Effect                                  |
| ----------------------- | --------------------------------------- |
| `'PING'`                | Replies with `'PONG'` (sent to `'*'`).  |
| `'SHOW-HELP'`           | Opens the help dialog.                  |
| `'RELOAD-CSS-CONSTANTS'`| Reloads CSS constants.                  |

## URL parameters

Set these on the iframe `src`. The route is hash-based:
`https://ui.perfetto.dev/#!/?key=val&...`.

| Parameter         | Value                          | Effect                                                                                                                              |
| ----------------- | ------------------------------ | --------------------------------------------------------------------------------------------------------------------------------- |
| `mode`            | `embedded`                     | Enables embedded mode: the sidebar is **disabled entirely** (not just hidden) and the file-drop handler is not installed. Use this when embedding. |
| `hideSidebar`     | `true`                         | Hides the sidebar visually without fully disabling it.                                                                             |
| `url`             | `<https url>`                  | UI fetches a public trace itself (requires CORS allowing the UI origin). Alternative to `postMessage` for public traces.           |
| `s`               | `<hash>`                       | Loads a permalink (saved state).                                                                                                   |
| `visStart`        | `<ns>`                         | Initial viewport start, raw **nanosecond** timestamp (as in SQL tables). Pair with `visEnd`.                                       |
| `visEnd`          | `<ns>`                         | Initial viewport end, raw **nanosecond** timestamp.                                                                               |
| `ts`              | `<ns>`                         | Timestamp of the slice to select on load, in **nanoseconds**. Linking is by `ts`+`dur`, **not** by `id` (ids are unstable).        |
| `dur`             | `<ns>`                         | Duration of the slice to select on load, in **nanoseconds**.                                                                       |
| `pid`             | `<n>`                          | Process id used to disambiguate the slice selection.                                                                              |
| `tid`             | `<n>`                          | Thread id used to disambiguate the slice selection.                                                                               |
| `query`           | `<sql>`                        | Runs a SQL query on load (URL-encode the value).                                                                                  |
| `startupCommands` | `<url-encoded JSON array>`     | Runs UI commands after load, e.g. `[{id:'dev.perfetto.PinTracksByRegex', args:['.*CPU [0-3].*']}]`.                                |
| `enablePlugins`   | `<comma,list>`                 | Enables specific plugins by id.                                                                                                    |

NOTE: `visStart`/`visEnd` and `ts`/`dur` are raw **nanoseconds**, whereas the
`timeStart`/`timeEnd` `postMessage` fields are **seconds**.

NOTE: Slice selection is by `ts`+`dur` (plus optional `pid`/`tid`), never by
`id`, because ids are unstable across runs.

## Origin trust

If the posting origin is trusted, the trace opens immediately. The trusted set
is:

- Same-origin requests.
- `localhost`, `127.0.0.1`, and `[::1]` (so local dev embedding works with no
  prompt).
- A few hardcoded Google origins.
- Origins the user previously saved via "Always trust".

If the origin is **not** trusted, the UI shows a modal:

> `<origin>` is trying to open a trace file. Do you trust the origin?

with the options **No**, **Yes**, and **Always trust**. "Always trust" persists
the origin in `localStorage`.

Embedding from a production domain therefore shows users a one-time consent
prompt, unless you self-host the UI (same-origin => trusted).

Strings in `title` and `url` are sanitized to the character set
`[A-Za-z0-9.\-_#:/?=&;%+$ ]`.

## Constraints

- Does not work from `file://` URLs (browser security). Serve over `http(s)`.
- The host page must **not** be served with
  `Cross-Origin-Opener-Policy: same-origin`, which breaks the opener
  relationship.
- The UI is client-only. Posted traces stay in browser memory and are never
  uploaded.
- `ui.perfetto.dev` follows the latest release. If you need a fixed version,
  self-host the UI build to pin it. Self-hosting also makes your origin
  same-origin, so the consent modal is skipped.

## Source

The behavior above is defined in:

- [/ui/src/frontend/post_message_handler.ts](/ui/src/frontend/post_message_handler.ts)
- [/ui/src/public/route_schema.ts](/ui/src/public/route_schema.ts)

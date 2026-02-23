# Deep linking to the Perfetto UI

This document describes how to open traces hosted on external servers with the
Perfetto UI. This can help integrating the Perfetto UI with custom dashboards
and implement _'Open with Perfetto UI'_-like features.

In this guide, you'll learn how to:

- Open public traces directly via URL (simplest approach).
- Open traces with full control using postMessage (for auth, sharing, etc.).

You'll also learn how to customize the UI state when opening traces (zoom,
selection, queries).

## Option 1: Direct URL for public traces

If your trace is publicly accessible via HTTPS, you can link directly to it
using the `url` query parameter:

```
https://ui.perfetto.dev/#!/?url=https://example.com/path/to/trace.pftrace
```

**Requirements:**

- The trace must be served over HTTPS.
- The URL must respond to a simple GET request without query parameters.
- Your server must set CORS headers to allow the Perfetto UI origin e.g.
  `Access-Control-Allow-Origin: https://ui.perfetto.dev` or
  `Access-Control-Allow-Origin: *`.

This is the easiest option for publicly hosted traces that don't require
authentication or custom sharing features.

**Limitations:**

- No authentication support (traces must be publicly accessible).
- No custom sharing URL support.
- No control over the trace title displayed in the UI.

If you need any of these features, use
[Option 2](#option-2-using-postmessage-for-full-control) instead.

## Option 2: Using postMessage for full control

For traces that require authentication, custom sharing URLs, or other advanced
features, use the postMessage approach. This requires some JavaScript code
running on infrastructure you control.

### Step 1: Open ui.perfetto.dev via window.open

The source dashboard (the one that knows how to locate a trace and deal with ACL
checking, OAuth authentication, etc.) creates a new tab:

```js
var handle = window.open('https://ui.perfetto.dev');
```

The window handle allows bidirectional communication using `postMessage()`
between your dashboard and the Perfetto UI.

### Step 2: Wait for the UI to be ready via PING/PONG

The `window.open()` message channel is not buffered. If you send a message
before the opened page has registered an `onmessage` listener, the message will
be dropped. To avoid this race condition, use a PING/PONG protocol: keep sending
'PING' messages until the opened window replies with 'PONG'.

### Step 3: Post the trace data

Once the PING/PONG handshake is complete, post a message to the Perfetto UI
window. The message should be a JavaScript object with a single `perfetto` key:

```js
{
  'perfetto': {
    buffer: ArrayBuffer;
    title: string;
    fileName?: string;    // Optional
    url?: string;         // Optional
    appStateHash?: string // Optional
  }
}
```

The properties of the `perfetto` object are:

- `buffer`: An `ArrayBuffer` containing the raw trace data. You would typically
  get this by fetching a trace file from your backend.
- `title`: A human-readable string that will be displayed as the title of the
  trace in the UI. This helps users distinguish between different traces if they
  have multiple tabs open.
- `fileName` (optional): The suggested file name if a user decides to download
  the trace from the Perfetto UI. If omitted, a generic name will be used.
- `url` (optional): A URL for sharing the trace. See the "Sharing" section
  below.
- `appStateHash` (optional): A hash for restoring the UI state when sharing. See
  the "Sharing" section below.

### Sharing traces and UI state

When traces are opened via `postMessage`, Perfetto avoids storing the trace as
doing so may violate the retention policy of the original trace source. The
trace is not uploaded anywhere. Thus, you must provide a URL that provides a
direct link to the same trace via your infrastructure, which should
automatically re-open Perfetto and use postMessage to supply the same trace.

The `url` and `appStateHash` properties work together to allow users to share a
link to a trace that, when opened, restores the trace and the UI to the same
state (e.g., zoom level, selected event).

When a user clicks the "Share" button in the Perfetto UI, Perfetto looks at the
`url` you provided when opening the trace. If this `url` contains the special
placeholder `perfettoStateHashPlaceholder`, Perfetto will:

1. Save the current UI state and generate a unique hash for it.
2. Replace `perfettoStateHashPlaceholder` in your `url` with this new hash.
3. Present this final URL to the user for sharing.

For example, if you provided this `url`:
`'https://my-dashboard.com/trace?id=1234&state=perfettoStateHashPlaceholder'`

Perfetto might generate a shareable URL like this:
`'https://my-dashboard.com/trace?id=1234&state=a1b2c3d4'`

When another user opens this shared URL, your application should:

1. Extract the state hash (`a1b2c3d4` in this example) from the URL.
2. `postMessage` the trace `buffer` as usual, but this time also include the
   `appStateHash` property with the extracted hash.

Perfetto will then load the trace and automatically restore the UI state
associated with that hash.

If the `url` property is omitted, the share functionality will be disabled. If
the `perfettoStateHashPlaceholder` is omitted from the `url`, the trace can be
shared but the UI state will not be saved.

### Code samples

See
[this example caller](https://bl.ocks.org/chromy/170c11ce30d9084957d7f3aa065e89f8),
for which the code is in
[this GitHub gist](https://gist.github.com/chromy/170c11ce30d9084957d7f3aa065e89f8).

Googlers: take a look at the
[existing examples in the internal codesearch](http://go/perfetto-ui-deeplink-cs).

### Common pitfalls

Many browsers sometimes block `window.open()` requests, prompting the user to
allow popups for the site. This usually happens if:

- The `window.open()` is NOT initiated by a user gesture.
- Too much time passes between the user gesture and the `window.open()`.

If the trace file is big enough, the `fetch()` might take long enough to exceed
the user gesture threshold. This can be detected by observing that
`window.open()` returned `null`. When this happens, the best option is to show
another clickable element and bind the fetched trace ArrayBuffer to the new
onclick handler, like the code in the example above does.

Some browsers have a variable time threshold for the user gesture timeout which
depends on the website engagement score (how much the user has visited the page
before). It's common when testing this code to see a popup blocker the first
time the new feature is used and then not see it again.

This scheme will not work from a `file://` based URL due to browser security
restrictions for `file://` URLs.

The source website must not be served with the
`Cross-Origin-Opener-Policy: same-origin` header. For example, see
[this issue](https://github.com/google/perfetto/issues/525#issuecomment-1625055986).

### Where does the posted trace go?

The Perfetto UI is client-only and doesn't require any server-side interaction.
Traces pushed via `postMessage()` are kept only in the browser memory/cache and
are not sent to any server.

## Customizing the UI with URL parameters

Beyond just opening a trace, you can control the initial UI state using URL
fragment parameters. These work with both Option 1 (direct URL) and Option 2
(postMessage).

### Zooming into a region of the trace

Pass `visStart` and `visEnd` to control the initial viewport. These values are
raw timestamps in nanoseconds as seen in the SQL tables:

```
https://ui.perfetto.dev/#!/?visStart=261191575272856&visEnd=261191675272856
```

This opens the trace at ~261192s with a 100ms wide viewing window.

### Selecting a slice on load

Pass `ts`, `dur`, `pid`, and/or `tid` parameters. The UI will query the slice
table and find a slice matching the parameters. If found, the slice is
highlighted. You don't have to provide all parameters; usually `ts` and `dur`
suffice to uniquely identify a slice.

NOTE: We deliberately do NOT support linking by slice ID because slice IDs are
not stable across Perfetto versions. Instead, link by passing the exact start
timestamp and duration (`ts` and `dur`) as seen by issuing a query like
`SELECT ts, dur FROM slices WHERE id=...`.

### Issuing a query on load

Pass the query in the `query` parameter.

### Examples

Try these examples:

- [visStart & visEnd](https://ui.perfetto.dev/#!/?url=https%3A%2F%2Fstorage.googleapis.com%2Fperfetto-misc%2Fexample_android_trace_15s&visStart=261191575272856&visEnd=261191675272856)
- [ts & dur](https://ui.perfetto.dev/#!/?url=https%3A%2F%2Fstorage.googleapis.com%2Fperfetto-misc%2Fexample_android_trace_15s&ts=261192482777530&dur=1667500)
- [query](https://ui.perfetto.dev/#!/?url=https%3A%2F%2Fstorage.googleapis.com%2Fperfetto-misc%2Fexample_android_trace_15s&query=select%20'Hello%2C%20world!'%20as%20msg)

Remember to URL-encode strings where needed.

### Startup commands

You can also automatically configure the UI itself when a trace opens by
embedding startup commands in the URL. This is useful for dashboard integration
where you want to provide users with a pre-configured analysis environment.

Pass startup commands in the `startupCommands` parameter as a URL-encoded JSON
array. The commands execute automatically after the trace loads, allowing you to
pin tracks, create debug tracks, or run any other UI automation.

```js
// Example: Pin CPU tracks and create a debug track
const commands = [
  {id: 'dev.perfetto.PinTracksByRegex', args: ['.*CPU [0-3].*']},
  {
    id: 'dev.perfetto.AddDebugSliceTrack',
    args: [
      "SELECT ts, dur as value FROM slice WHERE name LIKE '%render%'",
      'Render Operations',
    ],
  },
];

const url = `https://ui.perfetto.dev/#!/?startupCommands=${encodeURIComponent(
  JSON.stringify(commands),
)}`;
```

The startup commands use the same JSON format as described in the
[UI automation documentation](/docs/visualization/perfetto-ui.md#startup-commands),
but must be URL-encoded when passed as a parameter. For the list of stable
commands with backwards compatibility guarantees, see the
[Commands Automation Reference](/docs/visualization/commands-automation-reference.md).

## Source links

The source code that deals with the `postMessage()` in the Perfetto UI is
[`post_message_handler.ts`](/ui/src/frontend/post_message_handler.ts).

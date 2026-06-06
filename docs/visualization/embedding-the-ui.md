# Embedding the Perfetto UI

This guide shows you how to embed the Perfetto trace viewer _inside_ your own
tool or dashboard via an `<iframe>` and feed it traces programmatically. This is
the right approach when you want the trace view to live within your app's
chrome, as real tools like Dart DevTools and various profiler frontends do. If
instead you just want to launch the full Perfetto UI in a new browser tab (the
`window.open()` flow), see [Deep linking to the Perfetto UI](/docs/visualization/deep-linking-to-perfetto-ui.md);
that page also covers sharing URLs and `appStateHash`, which this guide does not
duplicate.

## Before you begin

- Serve your host page over `http(s)`, not `file://`. The embedding protocol
  relies on `postMessage` between windows, which browsers disable for
  `file://` origins.
- Do NOT serve your host page with the
  `Cross-Origin-Opener-Policy: same-origin` header. It breaks the
  parent/iframe relationship the UI depends on.
- During local development, serve from `localhost` / `127.0.0.1`. These origins
  are trusted by the UI, so traces open with no consent prompt (see
  [Trust prompts and going to production](#trust-prompts-and-going-to-production)).

## Step 1: Add the iframe

Embed the UI with `mode=embedded` in the URL. This fully disables the sidebar
(not just hides it), which is what you want for an embedded view. The route is
hash-based:

```html
<iframe
  id="perfetto"
  src="https://ui.perfetto.dev/#!/?mode=embedded"
  width="100%"
  height="600"
></iframe>
```

In embedded mode the file-drop handler is also not installed, so the iframe only
loads traces you post to it.

## Step 2: Do the PING/PONG handshake

The `postMessage` channel into the iframe is not buffered: if you post a trace
before the UI has registered its message listener, the message is silently
dropped. To avoid this race, repeatedly post the string `'PING'` until the UI
replies with `'PONG'`. The UI only sends `'PONG'` once its listener is
registered and `document.readyState === 'complete'`.

```js
const iframe = document.getElementById('perfetto');

function waitForReady() {
  return new Promise((resolve) => {
    const interval = setInterval(() => {
      iframe.contentWindow.postMessage('PING', '*');
    }, 100);

    window.addEventListener('message', function onMsg(evt) {
      if (evt.source === iframe.contentWindow && evt.data === 'PONG') {
        clearInterval(interval);
        window.removeEventListener('message', onMsg);
        resolve();
      }
    });
  });
}
```

## Step 3: Post the trace

Once the handshake completes, post an object with a single `perfetto` key to the
iframe's `contentWindow`. Only `buffer` (an `ArrayBuffer` of raw trace bytes) and
`title` are required:

```js
async function openTrace() {
  await waitForReady();

  const resp = await fetch(
    'https://storage.googleapis.com/perfetto-misc/example_android_trace_15s',
  );
  const buffer = await resp.arrayBuffer();

  iframe.contentWindow.postMessage(
    {
      perfetto: {
        buffer: buffer,
        title: 'My embedded trace',
      },
    },
    '*',
  );
}
```

The full set of fields on the `perfetto` object:

- `buffer` (required): `ArrayBuffer` of raw trace bytes.
- `title` (required): string shown as the trace title.
- `fileName` (optional): suggested name if the user downloads the trace.
- `url` (optional): sharing URL. See [Deep linking](/docs/visualization/deep-linking-to-perfetto-ui.md)
  for how `url` and `appStateHash` enable sharing.
- `appStateHash` (optional): 40-char hex hash restoring saved UI state.
- `localOnly` (optional): defaults to `true` for posted traces, which disables
  download/share. Set `false` to re-enable them.
- `keepApiOpen` (optional): if `true`, the listener stays active so you can post
  more traces later. If omitted, the handler removes its listener after the
  first trace.
- `pluginArgs` (optional): `{ [pluginId]: { [key]: unknown } }`, passed to
  plugins' `onTraceLoad()`.

NOTE: If you want to swap traces in the same iframe without reloading it, set
`keepApiOpen: true` on the first post. Otherwise the UI stops listening after
the first trace.

TIP: A bare `ArrayBuffer` is also accepted (the UI treats it as a trace titled
"External trace"), but posting the `{ perfetto: { buffer, title } }` object is
preferred so you control the title.

## Step 4 (optional): Drive the view

You can steer the embedded view in two ways.

To configure the UI as the trace opens, add `startupCommands` to the iframe
`src` as a URL-encoded JSON array of commands. For example, to pin the CPU
tracks:

```js
const commands = [
  {id: 'dev.perfetto.PinTracksByRegex', args: ['.*CPU [0-3].*']},
];
const src =
  'https://ui.perfetto.dev/#!/?mode=embedded&startupCommands=' +
  encodeURIComponent(JSON.stringify(commands));
```

To scroll and zoom to a time range after the trace is loaded, post a second
message. `timeStart` and `timeEnd` are **absolute trace time in seconds**, not
relative to the trace start (most traces do not start at 0); a range outside the
trace is clamped to its bounds. `viewPercentage` is optional and is a fraction
in the range `(0, 1]` (e.g. `0.5` fills half the viewport, `1` fills it exactly);
out-of-range values are ignored and fall back to `0.5`:

```js
// e.g. zoom to the first 2 seconds of a trace that starts at 261187s.
iframe.contentWindow.postMessage(
  {perfetto: {timeStart: 261187.0, timeEnd: 261189.0, viewPercentage: 1}},
  '*',
);
```

The UI retries this internally until the trace is ready, so you can post it
shortly after the trace without your own wait loop.

## Putting it together

Paste this into a file (e.g. `index.html`), serve it over `http(s)` from
`localhost`, and open it in a browser:

```html
<!doctype html>
<html>
  <body>
    <iframe
      id="perfetto"
      src="https://ui.perfetto.dev/#!/?mode=embedded"
      width="100%"
      height="600"
    ></iframe>

    <script>
      const iframe = document.getElementById('perfetto');
      const SAMPLE =
        'https://storage.googleapis.com/perfetto-misc/example_android_trace_15s';

      function waitForReady() {
        return new Promise((resolve) => {
          const interval = setInterval(() => {
            iframe.contentWindow.postMessage('PING', '*');
          }, 100);
          window.addEventListener('message', function onMsg(evt) {
            if (evt.source === iframe.contentWindow && evt.data === 'PONG') {
              clearInterval(interval);
              window.removeEventListener('message', onMsg);
              resolve();
            }
          });
        });
      }

      (async () => {
        await waitForReady();
        const buffer = await (await fetch(SAMPLE)).arrayBuffer();
        iframe.contentWindow.postMessage(
          {perfetto: {buffer, title: 'My embedded trace'}},
          '*',
        );
      })();
    </script>
  </body>
</html>
```

## Trust prompts and going to production

The UI guards which origins may push traces:

- `localhost`, `127.0.0.1`, `[::1]`, same-origin, and a few hardcoded Google
  origins are trusted. Traces from these open immediately with no prompt, so
  local development just works.
- From any other origin (e.g. your production domain), the UI shows a modal:
  _"&lt;origin&gt; is trying to open a trace file. Do you trust the origin?"_
  with **No / Yes / Always trust**. "Always trust" persists the origin in
  `localStorage`, so each of your users sees the prompt at most once.

To avoid the consent modal entirely in production, self-host the Perfetto UI
build on your own domain. A same-origin host page is trusted, so no prompt
appears.

NOTE: `ui.perfetto.dev` follows the latest release, so the embedding protocol
described here is stable, though UI details may change over time. If you need a
fixed version, self-host the UI build to pin it. Self-hosting also gives the
same-origin trust benefit described above.

NOTE: The UI is client-only. Posted traces stay in browser memory and are never
uploaded anywhere.

## A complete example

The companion [`perfetto-embed`](https://github.com/LalitMaganti/perfetto-embed)
repository is a runnable end-to-end example: `npm start` serves a "devtool" host
page whose control panel embeds the UI and drives it (load traces, zoom, pin
tracks, run queries). It ships a small framework-agnostic `PerfettoEmbed`
wrapper you can copy into your own tool, plus a React variant.

## See also

- [Deep linking to the Perfetto UI](/docs/visualization/deep-linking-to-perfetto-ui.md):
  the `window.open()` (new tab) flow, plus sharing URLs and `appStateHash`.
- [Embedding API reference](/docs/visualization/embedding-api-reference.md):
  the full list of messages and URL parameters the UI accepts.

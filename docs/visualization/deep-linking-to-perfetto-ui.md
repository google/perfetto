# Deep linking to the Perfetto UI

This document describes how to open traces hosted on external servers with the
Perfetto UI. This can help integrating the Perfetto UI with custom dashboards
and implement _'Open with Perfetto UI'_-like features.

## Using window.open and postMessage

The supported way of doing this is to _inject_ the trace as an ArrayBuffer
via `window.open('https://ui.perfetto.dev')` and `postMessage()`.
In order to do this you need some minimal JavaScript code running on some
hosting infrastructure you control which can access the trace file. In most
cases this is some dashboard which you want to deep-link to the Perfetto UI.

#### Open ui.perfetto.dev via window.open

The source dashboard, the one that knows how to locate a trace and deal with
ACL checking / oauth authentication and the like, creates a new tab by doing

```js
var handle = window.open('https://ui.perfetto.dev');
```

The window handle allows bidirectional communication using `postMessage()`
between the source dashboard and the Perfetto UI.

#### Wait for the UI to be ready via PING/PONG

Wait for the UI to be ready. The `window.open()` message channel is not
buffered. If you send a message before the opened page has registered an
`onmessage` listener the messagge will be dropped on the floor.
In order to avoid this race, you can use a very basic PING/PONG protocol: keep
sending a 'PING' message until the opened window replies with a 'PONG'.
When this happens, that is the signal that the Perfetto UI is ready to open
traces.

#### Post a message the following JavaScript object

```js
  {
    'perfetto': {
      buffer: ArrayBuffer;
      title: string;
      fileName?: string;  // Optional
      url?: string;       // Optional
    }
  }
```

`buffer` is the ArrayBuffer with the actual trace file content. This is
typically something that you obtain by doing a `fetch()` on your backend
storage.

`title` is the human friendly trace title that will be shown in the
sidebar. This can help people to disambiguate traces from several tabs.

`fileName` will be used if the user clicks on "Download". A generic name will
be used if omitted.

`url` is used if the user clicks on the "Share" link in the sidebar. This should
print to a URL owned by you that would cause your dashboard to re-open the
current trace, by re-kicking-off the window.open() process herein described.
If omitted traces won't be shareable.

### Code samples

See [this example caller](https://bl.ocks.org/chromy/170c11ce30d9084957d7f3aa065e89f8),
for which the code is in
[this GitHub gist](https://gist.github.com/chromy/170c11ce30d9084957d7f3aa065e89f8).

Googlers: take a look at the
[existing examples in the internal codesearch](http://go/perfetto-ui-deeplink-cs)

### Common pitfalls

Many browsers sometimes block window.open() requests prompting the user to allow
popups for the site. This usually happens if:

- The window.open() is NOT initiated by a user gesture.
- Too much time is passed from the user gesture to the window.open()

If the trace file is big enough, the fetch() might take long time and pass the
user gesture threshold. This can be detected by observing that the window.open()
returned `null`. When this happens the best option is to show another clickable
element and bind the fetched trace ArrayBuffer to the new onclick handler, like
the code in the example above does.

Some browser can have a variable time threshold for the user gesture timeout
which depends on the website engagement score (how much the user has visited
the page that does the window.open() before). It's quite common when testing
this code to see a popup blocker the first time the new feature is used and
then not see it again.

This scheme will not work from a `file://` based URL.
This is due to browser security context for `file://` URLs.

The source website must not be served with the
`Cross-Origin-Opener-Policy: same-origin` header.
For example see
[this issue](https://github.com/google/perfetto/issues/525#issuecomment-1625055986).

### Where does the posted trace go?

The Perfetto UI is client-only and doesn't require any server-side interaction.
Traces pushed via postMessage() are kept only in the browser memory/cache and
are not sent to any server.

## Why can't I just pass a URL?

_"Why you don't let me just pass a URL to the Perfetto UI (e.g. ui.perfetto.dev?url=...) and you deal with all this?"_

The answer to this is manifold and boils down to security.

#### Cross origin requests blocking

If ui.perfetto.dev had to do a `fetch('https://yourwebsite.com/trace')` that
would be a cross-origin request. Browsers disallow by default cross-origin
fetch requests.
In order for this to work, the web server that hosts yourwebsite.com would have
to expose a custom HTTP response header
 (`Access-Control-Allow-Origin: https://ui.perfetto.dev`) to allow the fetch.
In most cases customizing the HTTP response headers is outside of dashboard's
owners control.

You can learn more about CORS at
https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS

#### Content Security Policy

Perfetto UI uses a strict Content Security Policy which disallows foreign
fetches and subresources, as a security mitigation about common attacks.
Even assuming that CORS headers are properly set and your trace files are
publicly accessible, fetching the trace from the Perfetto UI would require
allow-listing your origin in our CSP policy. This is not scalable.

You can learn more about CSP at
https://developer.mozilla.org/en-US/docs/Web/HTTP/CSP

#### Dealing with OAuth2 or other authentication mechanisms

Even ignoring CORS, the Perfetto UI would have to deal with OAuth2 or other
authentication mechanisms to fetch the trace file. Even if all the dashboards
out there used OAuth2, that would still mean that Perfetto UI would have to know
about all the possible OAuth2 scopes, one for each dashboard. This is not
scalable.

## Opening the trace at a specific event or time

Using the fragment query string allows for more control over the UI after
the trace opens. For example this URL:

```
https://ui.perfetto.dev/#!/?visStart=261191575272856&visEnd=261191675272856
```

Will open the pushed trace at 261191575272856ns (~261192s) and the
viewing window will be 261191675272856ns -261191575272856ns = 100ms wide.

**Selecting a slice on load**:

You can pass the following parameters: `ts`, `dur`, `pid`, `tid`.
The UI will query the slice table and find a slice that matches the parameters
passed. If a slice is found it's highlighted.
You don't have to provide all the parameters.
Usually `ts` and `dur` suffice to uniquely identifying a slice.

We deliberately do NOT support linking by slice id. This is because slice IDs
are not stable across perfetto versions. Instead you can link a slice by passing
the exact start and duration (`ts` and `dur`), as you see them by issuing a
query `SELECT ts, dur FROM slices WHERE id=...`.

**Zooming into a region of the trace on load**:

Pass `visStart`, `visEnd`. These values are the raw values in `ns` as seen in
the sql tables.

**Issuing a query on load**:

Pass the query in the `query` parameter.


Try the following examples:
- [visStart & visEnd](https://ui.perfetto.dev/#!/?url=https%3A%2F%2Fstorage.googleapis.com%2Fperfetto-misc%2Fexample_android_trace_15s&visStart=261191575272856&visEnd=261191675272856)
- [ts & dur](https://ui.perfetto.dev/#!/?url=https%3A%2F%2Fstorage.googleapis.com%2Fperfetto-misc%2Fexample_android_trace_15s&ts=261192482777530&dur=1667500)
- [query](https://ui.perfetto.dev/#!/?url=https%3A%2F%2Fstorage.googleapis.com%2Fperfetto-misc%2Fexample_android_trace_15s&query=select%20'Hello%2C%20world!'%20as%20msg)

You must take care to correctly escape strings where needed.

## Source links

The source code that deals with the postMessage() in the Perfetto UI is
[`post_message_handler.ts`](/ui/src/frontend/post_message_handler.ts).

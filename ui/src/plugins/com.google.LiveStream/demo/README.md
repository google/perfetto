# Live stream demo

A live server for trying, developing and testing com.google.LiveStream without a
real one. It streams simulated training jobs with the protocol in the plugin's
`README.md`, and serves the UI on the same origin, as live servers do. It needs
only Node.

- `demo_backend.ts`: the protocol and the simulation, without HTTP. The UI
  integration test `ui/src/test/live_stream.test.ts` answers the plugin's
  requests with it, in process.
- `demo_server.ts`: serves it over HTTP, with the UI.

## Try it

From the root of the Perfetto checkout, with a built UI:

```sh
ui/build
ui/node ui/src/plugins/com.google.LiveStream/demo/demo_server.ts --ui-dist out/ui/ui/dist
```

Then open http://127.0.0.1:10081/. Or with the dev server, which rebuilds the UI
on changes:

```sh
ui/run-dev-server
ui/node ui/src/plugins/com.google.LiveStream/demo/demo_server.ts --ui-proxy http://127.0.0.1:10000
```

`/?target=<id>` opens another target. `--hud top` shows the HUD in a row above
the topbar rather than in the side panel. The server listens on 127.0.0.1, or
`--host`, and answers only requests to those or localhost. Other sites can't use
the `demo/` routes. `--help` lists all flags. Node warns about
`MODULE_TYPELESS_PACKAGE_JSON`: that's harmless.

The integration test: `ui/run-integrationtests --no-docker live_stream`.

## Targets

- `job-1/trainer`, the default, with 4 tasks: training steps with nested phases
  and checkpoints, an input pipeline, workers, and loss and accuracy counters.
- `job-1/evaluator`: evaluation steps.
- `job-2/distributed-data-preprocessing-coordinator`: a long name, 16 tasks.
- `job-2/reader`: 8 tasks.

Tasks are addressed as `<id>/<task index>`. Steps and tasks fail now and then:
they show on the Errors track. The slices are the same for the same seed and
target.

## Dev controls

Flags set them at start, and `demo/set` while running:

```sh
curl 'http://127.0.0.1:10081/demo/set?stall=1'           # Buffering
curl 'http://127.0.0.1:10081/demo/set?error=Unreachable' # Error
curl 'http://127.0.0.1:10081/demo/set?stall=0&error='    # Live again
curl 'http://127.0.0.1:10081/demo/set?seed_status=503'   # Seeds fail
curl 'http://127.0.0.1:10081/demo/set?page_size=10'      # Paged deltas
curl 'http://127.0.0.1:10081/demo/set?rate=4&lanes=8'    # Busier jobs
curl 'http://127.0.0.1:10081/demo/restart'               # New stream_id
curl 'http://127.0.0.1:10081/demo/state'                 # Settings, streams
```

`seed`, `backfill` and `tz` apply to new streams: restart them. A new
`stream_id` makes the UI reopen the trace.

# Visualising large traces

Browsers often limit the amount of memory a site can use.
This can cause problems when visualising large traces.

## Using TraceProcessor as a native accelerator

Perfetto UI supports offloading the parsing and processing of the trace to a
'server' instance of TraceProcessor running natively on your local machine.
This server process can take full advantage of the RAM of your machine as well
as running at full native (rather than WebAssembly) performance, leveraging
SSE on modern x86_64 machines.

```bash
curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor
./trace_processor --httpd /path/to/trace.pftrace
```

Then open https://ui.perfetto.dev as usual.

The Perfetto UI will automatically detect the presence of
`trace_processor --httpd` by probing http://127.0.0.1:9001 . When detected it
will prompt a dialog that asks if you want to use the external accelerator via
a WebSocket or the built-in WebAssembly runtime that runs in the browser.

## Using more than one instance in parallel

NOTE: this is a temporary workaround until getting to a better solution as
described in [b/317076350](http://b/317076350) (Googlers only).

As per [r.android.com/2940133](https://r.android.com/2940133) (Feb 2024) it is
possible to run different instances of trace_processor on different ports, and
point the UI to them.

**Pre-requisite:** Enable the
[Relax CSP flag](https://ui.perfetto.dev/#!/flags/cspAllowAnyWebsocketPort). You
need to do this only once. If the flag is not displayed, the CL above has not
made it into the release channel you are using (try Canary or Autopush)

```bash
./trace_processor --httpd --http-port 9001 trace1.pftrace
./trace_processor --httpd --http-port 9002 trace2.pftrace
./trace_processor --httpd --http-port 9003 trace3.pftrace
```

Then open the UI in three tabs as follows:
* https://ui.perfetto.dev/#!/?rpc_port=9001
* https://ui.perfetto.dev/#!/?rpc_port=9002
* https://ui.perfetto.dev/#!/?rpc_port=9003

## How big is too big?

The exact memory limit can vary by browser, architecture, and OS however 2GB is
typical. This limit is a limit on the total memory used at runtime, not on the
binary size of the trace.
The `trace_processor` (and hence the UI) representation of a trace at runtime is
normally larger than the binary size of that trace.
This is because the representation is optimized for query performance rather
than size. The exact inflation factor varies depending on the trace format but
can be 2-4x for uncompressed proto traces.

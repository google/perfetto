# Visualising large traces

Browsers often limit the amount of memory a site can use.
This can cause problems when visualising large traces.

## How to visualise large traces using the Perfetto UI

Perfetto UI has support for a mode where the processing of the trace
is offloaded to a 'server' instance of `trace_processor` running natively on your local machine.
This server process can take full advantage of the RAM of your machine as well as running at full native (rather than WASM) performance.

```
curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor
trace_processor --httpd /path/to/trace.pftrace
# Navigate to http://ui.perfetto.dev, it will prompt to use the HTTP+RPC interface
```

## How big is too big?

The exact memory limit can vary by browser, architecture, and OS however 2gb is typical.
This limit is a limit on the total memory used at runtime, not on the binary size of the trace.
The `trace_processor` (and hence the UI) representation of a trace at runtime is normally larger than the binary size of that trace.
This is because the representation is optimized for query performance rather than size.
The exact inflation factor varies depending on the trace format but can be 2-4x for uncompressed proto traces.



# Instrumenting the Linux kernel with ftrace

In this page, you'll learn how to add instrumentation to the Linux kernel,
record a trace that includes these events, and process and analyze the resulting
trace.

Perfetto has deep support for ftrace events recording, and trace processor has deep support for pulling out various ftrace events and interpreting them as separate tracks - e.g. scheduling.

## Instrumenting code with ftrace

There are several ways to instrument code with ftrace events. Typically you'll want to use static tracepoints to define your ftrace events for your kernel module / code. Then you'll want to emit events whenever anything interesting happens in your code.


See the following links for more details:\

- https://docs.kernel.org/trace/tracepoints.html
- https://lwn.net/Articles/379903/
- https://lwn.net/Articles/381064/
- https://lwn.net/Articles/383362/

## Recording your ftrace trace

<?tabs>

TAB: Android

Use record_android_trace script

TAB: Linux

Make sure you have tracebox available (see todo).

Recording config:

```
# One buffer allocated within the central tracing binary for the entire trace,
# shared by the two data sources below.
buffers {
  size_kb: 20480
  fill_policy: DISCARD
}

# Ftrace data from the kernel
data_sources {
  config {
    name: "linux.ftrace"
    target_buffer: 0
    ftrace_config {
      # Add your custom events here - perfetto will enable them and record
      ftrace_events: "kevin/kevin_event"
    }
  }
}

# 10s trace, but can be stopped prematurely.
duration_ms: 10000
```

Run it with the following:

```bash
tracebox -c config.cfg --txt -o mytrace
```

</tabs?>

## Viewing your recorded trace

We can now explore the recorded trace in the perfetto UI. Navigate to ui.perfetto.dev and drag and drop your file into the window (or press Ctrl/Cmd+O to bring up the file selector dialog.).

Perfetto doesn't know how to interpret these events currently so they are displayed in the 'raw' ftrace tracks only.

TODO a video on the UI.

Alternatively, you can explore the trace contents issuing SQL queries through
the [trace processor](/docs/analysis/trace-processor).

TODO: Video: open trace in Perfetto UI, navigate around

## Turning tracepoints into slices and counters



## Next Steps

ftrace with other data sources?


# Outline
- Instrumenting code with ftrace
  - Include very basic  C example of how to add trace points to your kernel module/subsystem.
  - Maybe lean heavily on the links for this.
- Recording a trace
  - Include examples of how to record a trace containing this new ftrace event:
    - Just include the config and link to system tracing for information on how to actually do the recording
    - Android using the UI or record_android_trace with screenshots
- Viewing recorded trace in the UI
  - What you will see
  - How to convert this into a track using debug tracks
- Adding a conversion to trace processor
  - Converting and making tracks from the ftrace events - add to the ftrace parser.
- What to expect in the UI
- How to add special plugins (link to plugin page?)

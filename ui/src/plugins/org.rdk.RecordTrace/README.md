## org.rdk.RecordTrace Plugin

This is a fork of the [dev.perfetto.RecordTraceV2](../dev.perfetto.RecordTraceV2) plugin, which is a plugin for
recording traces using Perfetto.

The main change is to support starting a trace by proxying calls to a `traced` daemon
on an RDK device via a web page.

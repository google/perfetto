# Running Perfetto

In order to run Perfetto and get a meaningful trace you need to build
(see [build instructions](build_instructions.md)) and run the following:

`traced`: The unprivileged trace daemon that owns the log buffers and maintains
a registry of Producers and Consumers connected.

`traced_probes`: The privileged daemon that has access to the Kernel tracefs
(typically mounted under `/sys/kernel/debug/tracing`), can drive
[Ftrace](https://source.android.com/devices/tech/debug/ftrace) and writes its
output into `traced`.

`perfetto`: A command line utility client that drive the trace and save back
the results (either to a file or to [Android's Dropbox][dropbox])


## Instructions:
```
# TODO(primiano): this is temporary until we fix SELinux policies.
adb shell su root setenforce 0

adb shell su root start traced
adb shell su root start traced_probes
```

If this works you will see something like:

```
$ adb logcat -s perfetto
perfetto: service.cc:45 Started traced, listening on /dev/socket/traced_producer /dev/socket/traced_consumer
perfetto: probes.cc:25 Starting /system/bin/traced_probes service
perfetto: ftrace_producer.cc:32 Connected to the service
```

At which point you can grab a trace by doing:

```
$ adb shell perfetto --config :test --out /data/local/tmp/trace
```

or to save it to [Android's Dropbox][dropbox]:

```
$ adb shell perfetto --config :test --dropbox perfetto
```

`--config :test` uses a hard-coded test trace config. It is possible to pass
an arbitrary trace config by doing the following:
```
cat > /tmp/config.txpb <<EOF
# This is a text-encoded protobuf for /protos/tracing_service/trace_config.proto
duration_ms: 2000
buffers {
  size_kb: 1024
  optimize_for: ONE_SHOT_READ
  fill_policy: RING_BUFFER
}
data_sources {
  config {
    name: "com.google.perfetto.ftrace"
    target_buffer: 0
    ftrace_config {
      event_names: "sched_switch"
    }
  }
}
EOF

protoc=$(pwd)/out/android/gcc_like_host/protoc

$protoc --encode=perfetto.protos.TraceConfig \
        -I$(pwd)/external/perfetto \
        $(pwd)/external/perfetto/protos/tracing_service/trace_config.proto \
        < /tmp/config.txpb \
        > /tmp/config.pb

adb push /tmp/config.pb /data/local/tmp/
adb shell perfetto -c /data/local/tmp/config.pb -o /data/local/tmp/trace.pb
adb pull /data/local/tmp/trace.pb /tmp/
out/android/trace_to_text systrace < /tmp/trace.pb > /tmp/trace.json

# The file can now be viewed in chrome://tracing
```

[dropbox]: https://developer.android.com/reference/android/os/DropBoxManager.html

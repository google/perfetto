# Getting an OutOfMemoryError heap dump on Android

Starting with Android 14 (U), perfetto can be configured to collect a heap dump
when any Java (ART) process crashes because of a java.lang.OutOfMemoryError.

## Steps

You can configure collection using the `tools/java_heap_dump` tool and passing
the `--wait-for-oom` parameter.

Alternatively, a quick way to do it (without any dependencies except for adb
access):

```bash
cat << EOF | adb shell perfetto -c - --txt -o /data/misc/perfetto-traces/oome.pftrace
buffers: {
    size_kb: 512288
    fill_policy: DISCARD
}

data_sources: {
    config {
        name: "android.java_hprof.oom"
        java_hprof_config {
          process_cmdline: "*"
        }
    }
}

data_source_stop_timeout_ms: 100000

trigger_config {
    trigger_mode: START_TRACING
    trigger_timeout_ms: 3600000
    triggers {
      name: "com.android.telemetry.art-outofmemory"
      stop_delay_ms: 500
    }
}
data_sources {
  config {
    name: "android.packages_list"
  }
}
EOF
```

This will start a perfetto tracing session for an hour (trigger_timeout_ms)
waiting for any runtime instance to hit an OutOfMemoryError. Once an error is captured, tracing will stop:

```text
[862.335]    perfetto_cmd.cc:1047 Connected to the Perfetto traced service, TTL: 3601s
[871.335]    perfetto_cmd.cc:1210 Wrote 19487866 bytes into /data/misc/perfetto-traces/oome.pftrace
```

You will then be able to download the heap dump by running
`adb pull /data/misc/perfetto-traces/oome.pftrace`.

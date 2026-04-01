# Android Hprof Dump Data Source

## Overview

A Perfetto data source that triggers a Java heap dump (`.hprof`) on a
target Android process and embeds the raw hprof binary in the trace as
chunked packets. Optionally also extracts bitmap images as PNGs.

This runs in **system_server** (AOSP `frameworks/base`) and uses the
existing `ActivityManagerService.dumpHeap()` mechanism.

## What it does

1. Receives trace config with target process pid/cmdline
2. Calls `AMS.dumpHeap(process, managed=true, dumpBitmaps, path, fd, cb)`
3. Target process runs `Debug.dumpHprofData()` -> `.hprof` file
4. Optionally target process runs `Bitmap.dumpAll("png")` -> PNG files
5. On completion callback: streams file in 512KB chunks as trace packets
6. Cleans up temp files

## Proto

### Config

```proto
// protos/perfetto/config/profiling/hprof_dump_config.proto
message HprofDumpConfig {
  optional uint64 pid = 1;
  optional string process_cmdline = 2;
  optional bool run_gc = 3;          // GC before dump, default true
  optional bool dump_bitmaps = 4;    // also extract bitmaps as PNG
  optional string bitmap_format = 5; // "png" (default) or "webp"
}
```

### Trace output

```proto
// protos/perfetto/trace/profiling/hprof_dump.proto
message HprofDump {
  optional int32 pid = 1;           // target process pid
  optional bytes hprof_data = 2;    // chunk of raw .hprof binary
  optional uint32 chunk_index = 3;  // zero-based chunk index
  optional bool last_chunk = 4;     // true on final chunk for this pid
}
```

The trace processor groups chunks by `pid` and finalizes each dump
when `last_chunk` is received. Multiple dumps (different pids or
sequential same-pid dumps) can coexist in one trace.

## Trace processor

`HprofDumpModule` handles `TracePacket.hprof_dump`:
1. Maintains a per-pid `ArtHprofParser` instance
2. Feeds `hprof_data` chunks via `ArtHprofParser::Parse()`
3. On `last_chunk`: calls `OnPushDataToSorter()` to populate
   `heap_graph_class`, `heap_graph_object`, `heap_graph_reference`
4. Any incomplete dumps are finalized at trace end

No new tables -- reuses the existing heap graph infrastructure.

## Chunking

Hprof dumps are typically 200-400MB. The data source streams the file
in 512KB chunks to avoid loading the entire dump into memory. Each
chunk becomes one `TracePacket` with `HprofDump { chunk_index, ... }`.
The final chunk sets `last_chunk = true`.

## Trace config example

```
buffers { size_kb: 524288 }  # 512MB for large hprof
data_sources {
  config {
    name: "android.hprof_dump"
    hprof_dump_config {
      process_cmdline: "com.example.app"
      run_gc: true
      dump_bitmaps: true
    }
  }
}
duration_ms: 60000
```

## Testing

```sh
# Existing mechanism (works today):
adb shell am dumpheap -b png com.example.app /data/local/tmp/dump.hprof

# With Perfetto (after this data source is built):
adb shell perfetto -c - --txt <<EOF
buffers { size_kb: 524288 }
data_sources { config { name: "android.hprof_dump"
  hprof_dump_config { process_cmdline: "com.example.app" run_gc: true }
} }
duration_ms: 60000
EOF
```

Load the trace in Perfetto UI -> heap graph tables populated from the
embedded hprof data, alongside any other concurrent trace data.

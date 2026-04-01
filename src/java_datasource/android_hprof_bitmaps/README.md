# Android Hprof Dump Data Source

## Overview

A Perfetto data source that triggers a Java heap dump (`.hprof`) on a
target Android process and embeds the raw hprof binary in the trace as
a packet. Optionally also extracts bitmap images as PNGs.

This runs in **system_server** (AOSP `frameworks/base`) and uses the
existing `ActivityManagerService.dumpHeap()` mechanism.

## What it does

1. Receives trace config with target process pid/cmdline
2. Calls `AMS.dumpHeap(process, managed=true, dumpBitmaps, path, fd, cb)`
3. Target process runs `Debug.dumpHprofData()` → `.hprof` file
4. Optionally target process runs `Bitmap.dumpAll("png")` → PNG files
5. On completion callback: reads the files, writes trace packets
6. Cleans up temp files

## Proto

### Config (new, in Perfetto repo)

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

### Trace output (new, in Perfetto repo)

```proto
// protos/perfetto/trace/profiling/hprof_dump.proto
message HprofDump {
  // Process ID of the dumped process.
  optional int32 pid = 1;

  // Raw .hprof binary data. Can be split across multiple packets
  // if the dump is large (use continued flag on TracePacket).
  optional bytes hprof_data = 2;
}
```

Add to TracePacket:
```proto
HprofDump hprof_dump = <next_field>;
```

For bitmaps, reuse the `VideoFrame` proto or add:
```proto
message HprofBitmap {
  optional int32 pid = 1;
  optional string filename = 2;   // e.g., "bitmap_0.png"
  optional bytes png_image = 3;
}
```

## Trace processor changes

### Option A: Route raw bytes to ArtHprofParser

Add a module that handles `TracePacket.hprof_dump`:
1. Extracts the `hprof_data` bytes
2. Feeds them to `ArtHprofParser::Parse()` as `TraceBlobView` chunks
3. `ArtHprofParser` populates the existing `heap_graph_*` tables

This reuses all existing hprof parsing -- no new tables needed.

### Option B: Store raw bytes as BLOB

Store the raw hprof bytes in a BLOB vector (like VideoFrame) and
expose via `hprof_dump_data(id)` SQL function. This lets the UI
download the raw `.hprof` file.

**Recommended: both.** Parse into heap_graph tables AND store raw
bytes so the user can also download the original file.

## System server data source (AOSP)

### `HprofDumpDataSource.java`

```java
public class HprofDumpDataSource extends PerfettoDataSource {
    static { INSTANCE.register("android.hprof_dump"); }

    @Override
    protected void onStart(int instanceIndex, byte[] config) {
        // 1. Parse HprofDumpConfig from config bytes
        // 2. Find target process via AMS
        // 3. Create temp dir: /data/local/tmp/hprof_<sessionId>/
        // 4. Create ParcelFileDescriptor for output
        // 5. Call AMS.dumpHeap(process, managed=true,
        //      dumpBitmaps=config.dump_bitmaps ? "png" : null,
        //      path, fd, finishCallback)
        // 6. In finishCallback (runs when dump complete):
        //    a. Read .hprof file bytes
        //    b. Write as TracePacket { hprof_dump { pid, hprof_data } }
        //       Split into multiple packets if >4MB
        //    c. If bitmap dump enabled:
        //       Read each PNG file
        //       Write as TracePacket { hprof_bitmap { pid, filename, png } }
        //    d. commitPacket()
        //    e. Delete temp files
    }
}
```

### Splitting large hprof files

A heap dump can be 50-200MB. This won't fit in a single trace packet.
Use the `continued` flag on TracePacket to split across multiple
packets on the same sequence:

```java
byte[] hprofBytes = Files.readAllBytes(hprofPath);
int chunkSize = 4 * 1024 * 1024; // 4MB chunks
for (int offset = 0; offset < hprofBytes.length; offset += chunkSize) {
    int len = Math.min(chunkSize, hprofBytes.length - offset);
    ProtoWriter w = ctx.getWriter();
    // Write timestamp, sequence_id, etc.
    int dump = w.beginNested(HPROF_DUMP_FIELD);
    w.writeVarInt(1, pid);
    w.writeBytes(2, hprofBytes, offset, len);
    w.endNested(dump);
    ctx.commitPacket();
}
```

### Shmem buffer sizing

Hprof dumps are large. Set `shmem_size_hint_kb = 8192` (8MB).
Use `PERFETTO_DS_BUFFER_EXHAUSTED_POLICY_STALL_AND_ABORT`.

### Threading

The dump is async -- `AMS.dumpHeap()` returns immediately, the target
process does the work, then calls the finish callback. The data source
should defer stop until the callback fires (like LayerDataSource's
`HandleStopAsynchronously()`).

## Trace config example

```
buffers { size_kb: 262144 }  # 256MB for large hprof
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

On device:
```sh
# Existing mechanism (works today):
adb shell am dumpheap -b png com.example.app /data/local/tmp/dump.hprof

# With Perfetto (after this data source is built):
adb shell perfetto -c - --txt <<EOF
buffers { size_kb: 262144 }
data_sources { config { name: "android.hprof_dump"
  hprof_dump_config { process_cmdline: "com.example.app" run_gc: true }
} }
duration_ms: 60000
EOF
```

Load the trace in Perfetto UI → heap graph tables populated from the
embedded hprof data, alongside any other concurrent trace data.

## Perfetto repo changes needed

1. Config proto: `HprofDumpConfig`
2. Trace proto: `HprofDump` (raw bytes), `HprofBitmap` (PNG bytes)
3. Trace processor module: routes `hprof_dump` bytes to `ArtHprofParser`
4. Optional: BLOB storage + SQL function for raw hprof download
5. Optional: UI plugin for bitmap gallery view

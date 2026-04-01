# Integration Guide: Wiring HprofDumpDataSource into system_server

## Files to modify in AOSP `frameworks/base`

### 1. Create the data source

Copy `HprofDumpDataSource.java` to:
```
services/core/java/com/android/server/am/HprofDumpDataSource.java
```

### 2. Register in ActivityManagerService

In `ActivityManagerService.java`, add to the constructor or `systemReady()`:

```java
// In ActivityManagerService.java, after other Perfetto data sources are initialized:
private HprofDumpDataSource mHprofDumpDataSource;

// In systemReady() or start():
mHprofDumpDataSource = new HprofDumpDataSource(this);
```

This is the same pattern as `WindowTracingDataSource` which is created in
`WindowManagerService` and registers itself with Perfetto in its constructor.

### 3. SELinux policy (if needed)

The data source writes to `/data/local/tmp/perfetto_hprof/`. This directory
must be writable by system_server. On userdebug/eng builds this should work.
For production, a dedicated directory with proper SELinux labels may be needed.

### 4. Build file

Add `HprofDumpDataSource.java` to the appropriate `Android.bp` or
`Android.mk` for the services module.

## Trace config to test

```sh
adb shell perfetto -c - --txt <<EOF
buffers { size_kb: 262144 }
data_sources {
  config {
    name: "android.hprof_dump"
  }
}
duration_ms: 60000
EOF
```

## Notes

- The data source uses the existing `AMS.dumpHeap()` API -- no new
  system APIs needed
- The dump is async: AMS sends a binder call to the target app,
  the app does the dump, then calls back
- Large hprof files (50-200MB) are split into 4MB chunks
- Bitmap PNGs are written as separate packets
- The data source cleans up temp files after writing to the trace

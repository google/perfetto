# heapprofd - Android Heap Profiler

Googlers, for design doc see: http://go/heapprofd-design

## Using convenience script

Use the `tools/heap_profile` script to heap profile a process. See all the
arguments using `tools/heap_profile -h`, or use the defaults and just profile a
process (e.g. `system_server`):

```
tools/heap_profile --name system_server
```

This will create a heap dump when Ctrl+C is pressed.

The resulting profile proto contains four views on the data

* space: how many bytes were allocated but not freed at this callstack the
  moment the dump was created.
* alloc\_space: how many bytes were allocated (including ones freed at the
  moment of the dump) at this callstack
* objects: how many allocations without matching frees were done at this
  callstack.
* alloc\_objects: how many allocations (including ones with matching frees) were
  done at this callstack.

**Googlers:** Head to http://pprof/ and upload the gzipped protos to get a
visualization.

[Speedscope](https://speedscope.app) can also be used to visualize the heap
dump, but will only show the space view.

## Manual
To start profiling the process `${PID}`, run the following sequence of commands.
Adjust the `INTERVAL` to trade-off runtime impact for higher accuracy of the
results. If `INTERVAL=1`, every allocation is sampled for maximum accuracy.
Otherwise, a sample is taken every `INTERVAL` bytes on average.

```bash
INTERVAL=128000

adb shell su root setenforce 0
adb shell su root start heapprofd

echo '
buffers {
  size_kb: 100024
}

data_sources {
  config {
    name: "android.heapprofd"
    target_buffer: 0
    heapprofd_config {
      sampling_interval_bytes: '${INTERVAL}'
      pid: '${PID}'
      continuous_dump_config {
        dump_phase_ms: 10000
        dump_interval_ms: 1000
      }
    }
  }
}

duration_ms: 20000
' | adb shell perfetto --txt -c - -o /data/misc/perfetto-traces/trace

adb pull /data/misc/perfetto-traces/trace /tmp/trace
```

While we work on UI support, you can convert the trace into pprof compatible
heap dumps. To do so, run

```
prodaccess
/google/bin/users/fmayer/third_party/perfetto:trace_to_text_sig/trace_to_text \
profile /tmp/trace
```

This will create a directory in `/tmp/` containing the heap dumps. Run

```
gzip /tmp/heap_profile-XXXXXX/*.pb
```

to get gzipped protos, which tools handling pprof profile protos expect.
Head to http://pprof/ and upload the gzipped protos to get a visualization.

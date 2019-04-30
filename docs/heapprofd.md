# heapprofd - Android Heap Profiler

Googlers, for design doc see: http://go/heapprofd-design

**heapprofd requires Android Q.**

heapprofd is a tool that tracks native heap allocations & deallocations of an
Android process within a given time period. The resulting profile can be used
to attribute memory usage to particular function callstacks, supporting a mix
of both native and java code. The tool should be useful to Android platform
developers, and app developers investigating memory issues.

On debug Android builds, you can profile all apps and most system services.
On "user" builds, you can only use it on apps with the debuggable or
profileable manifest flag.

## Quickstart

<!-- This uses github because gitiles does not allow to get the raw file. -->

Use the `tools/heap_profile` script to heap profile a process. If you are
having trouble make sure you are using the [latest version](
https://raw.githubusercontent.com/catapult-project/perfetto/master/tools/heap_profile).

See all the arguments using `tools/heap_profile -h`, or use the defaults
and just profile a process (e.g. `system_server`):

```
$ tools/heap_profile --name system_server
Profiling active. Press Ctrl+C to terminate.
^CWrote profiles to /tmp/heap_profile-XSKcZ3i (symlink /tmp/heap_profile-latest)
These can be viewed using pprof. Googlers: head to pprof/ and upload them.
```

This will create a pprof-compatible heap dump when Ctrl+C is pressed.

## Viewing the data

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
visualization. *Tip: you might want to put `libart.so` as a "Hide regex" when
profiling apps.*

[Speedscope](https://speedscope.app) can also be used to visualize the heap
dump, but will only show the space view. *Tip: Click Left Heavy on the top
left for a good visualisation.*

## Sampling interval
heapprofd samples heap allocations. Given a sampling interval of n bytes,
one allocation is sampled, on average, every n bytes allocated. This allows to
reduce the performance impact on the target process. The default sampling rate
is 4096 bytes.

The easiest way to reason about this is to imagine the memory allocations as a
steady stream of one byte allocations. From this stream, every n-th byte is
selected as a sample, and the corresponding allocation gets attributed the
complete n bytes. As an optimization, we sample allocations larger than the
sampling interval with their true size.

To make this statistically more meaningful, Poisson sampling is employed.
Instead of a static parameter of n bytes, the user can only choose the mean
value around which the interval is distributed. This makes sure frequent small
allocations get sampled as well as infrequent large ones.

## Startup profiling
When a profile session names processes by name and a matching process is
started, it gets profiled from the beginning. The resulting profile will
contain all allocations done between the start of the process and the end
of the profiling session.

On Android, Java apps are usually not started, but the zygote forks and then
specializes into the desired app. If the app's name matches a name specified
in the profiling session, profiling will be enabled as part of the zygote
specialization. The resulting profile contains all allocations done between
that point in zygote specialization and the end of the profiling session.
Some allocations done early in the specialization process are not accounted
for.

The Resulting `ProfileProto` will have `from_startup` set  to true in the
corresponding `ProcessHeapSamples` message. This does not get surfaced in the
converted pprof compatible proto.

## Runtime profiling
When a profile session is started, all matching processes (by name or PID)
are enumerated and profiling is enabled. The resulting profile will contain
all allocations done between the beginning and the end of the profiling
session.

The Resulting `ProfileProto` will have `from_startup` set  to false in the
corresponding `ProcessHeapSamples` message. This does not get surfaced in the
converted pprof compatible proto.

## Concurrent profiling sessions
If multiple sessions name the same target process (either by name or PID),
only the first relevant session will profile the process. The other sessions
will report that the process had already been profiled when converting to
the pprof compatible proto.

If you see this message but do not expect any other sessions, run
```
adb shell killall -KILL perfetto
```
to stop any concurrent sessions that may be running.


The Resulting `ProfileProto` will have `rejected_concurrent` set  to true in
otherwise empty corresponding `ProcessHeapSamples` message. This does not get
surfaced in the converted pprof compatible proto.

## Target processes
Depending on the build of Android that heapprofd is run on, some processes
are not be eligible to be profiled.

On user builds, only Java applications with either the profileable or the
debugable manifest flag set can be profiled. Profiling requests for other
processes will result in an empty profile.

On userdebug builds, all processes except for a small blacklist of critical
services can be profiled. This restriction can be lifted by disabling
SELinux by running `adb shell su root setenforce 0` or by passing
`--disable-selinux` to the `heap_profile` script.

|                         | userdebug setenforce 0 | userdebug | user |
|-------------------------|------------------------|-----------|------|
| critical native service |            y           |     n     |  n   |
| native service          |            y           |     y     |  n   |
| app                     |            y           |     y     |  n   |
| profileable app         |            y           |     y     |  y   |
| debugable app           |            y           |     y     |  y   |

## Troubleshooting

### Buffer overrun
If the rate of allocations is too high for heapprofd to keep up, the profiling
session will end early due to a buffer overrun. If the buffer overrun is
caused by a transient spike in allocations, increasing the shared memory buffer
size (passing `--shmem-size` to heap\_profile) can resolve the issue.
Otherwise the sampling interval can be increased (at the expense of lower
accuracy in the resulting profile) by passing `--interval` to heap\_profile.

### Profile is empty
Check whether your target process is eligible to be profiled by consulting
[Target process](#Target_process) above.

## Known Issues

* Does not work on x86 platforms (including the Android cuttlefish emulator).

## Manual instructions
*It is not recommended to use these instructions unless you have advanced
requirements or are developing heapprofd. Proceed with caution*

### Download trace\_to\_text
Download the latest trace\_to\_text for [Linux](
https://storage.googleapis.com/perfetto/trace_to_text-4ab1d18e69bc70e211d27064505ed547aa82f919)
or [MacOS](https://storage.googleapis.com/perfetto/trace_to_text-mac-2ba325f95c08e8cd5a78e04fa85ee7f2a97c847e).
This is needed to convert the Perfetto trace to a pprof-compatible file.

Compare the `sha1sum` of this file to the one contained in the file name.

### Start profiling
To start profiling the process `${PID}`, run the following sequence of commands.
Adjust the `INTERVAL` to trade-off runtime impact for higher accuracy of the
results. If `INTERVAL=1`, every allocation is sampled for maximum accuracy.
Otherwise, a sample is taken every `INTERVAL` bytes on average.

```bash
INTERVAL=4096

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
    }
  }
}

duration_ms: 20000
' | adb shell perfetto --txt -c - -o /data/misc/perfetto-traces/profile

adb pull /data/misc/perfetto-traces/profile /tmp/profile
```

### Convert to pprof compatible file

While we work on UI support, you can convert the trace into pprof compatible
heap dumps.

Use the trace\_to\_text file downloaded above, with XXXXXXX replaced with the
`sha1sum` of the file.

```
trace_to_text-linux-XXXXXXX profile /tmp/profile
```

This will create a directory in `/tmp/` containing the heap dumps. Run

```
gzip /tmp/heap_profile-XXXXXX/*.pb
```

to get gzipped protos, which tools handling pprof profile protos expect.

Follow the instructions in [Viewing the Data](#viewing-the-data) to visualise
the results.

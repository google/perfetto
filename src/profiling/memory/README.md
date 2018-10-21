# heapprofd - Android Heap Profiler

_These are temporary instructions while heapprofd is under development. They are
subject to frequent change and will be obsoleted once heapprofd is integrated
into Perfetto._

Currently heapprofd only works with SELinux disabled and when run as root.

To start profiling the process `${PID}`, run the following sequence of commands.

```bash
adb root
adb shell setenforce 0
adb shell 'heapprofd -s -r 128000' & # Start standalone with 128kB sampling.
adb shell kill -36 ${PID} # Start profiling the process.
```

To obtain heap dumps for all profiled processes, send `SIGUSR1` to heapprofd
which produces heap dumps in /data/local/tmp.

```bash
adb shell killall -USR1 heapprofd
adb pull /data/local/tmp/heap_dump.${PID}
```

This file can then be converted to a flamegraph using Brendan Gregg's
[`flamegraph.pl`](
  https://github.com/brendangregg/FlameGraph/blob/master/flamegraph.pl).

```bash
flamegraph.pl heap_dump.${PID} > heap_dump.${PID}.svg
```

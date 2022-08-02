# HEAP_PROFILE(1)

## NAME

heap_profile - record heap profile on Android device

## DESCRIPTION

`tools/heap_profile` allows to collect native memory profiles on Android.
See [Recording traces](/docs/data-sources/native-heap-profiler.md) for more
details about the data-source.

```
usage: heap_profile [-h] [-i INTERVAL] [-d DURATION] [--no-start] [-p PIDS]
                    [-n NAMES] [-c CONTINUOUS_DUMP] [--disable-selinux]
                    [--no-versions] [--no-running] [--no-startup]
                    [--shmem-size SHMEM_SIZE] [--block-client]
                    [--block-client-timeout BLOCK_CLIENT_TIMEOUT]
                    [--no-block-client] [--idle-allocations] [--dump-at-max]
                    [--disable-fork-teardown] [--simpleperf]
                    [--traceconv-binary TRACECONV_BINARY]
                    [--print-config] [-o DIRECTORY]
```

## OPTIONS
`-n`, `--name` _NAMES_
:    Comma-separated list of process names to profile.

`-p`, `--pid` _PIDS_
:    Comma-separated list of PIDs to profile.

`-i`, `--interval`
:    Sampling interval. Default 4096 (4KiB)

`-o`, `--output` _DIRECTORY_
:    Output directory.

`--all-heaps`
:    Collect allocations from all heaps registered by target.

`--block-client`
:    When buffer is full, block the client to wait for buffer space. Use with caution as this can significantly slow down the client. This is the default

`--block-client-timeout`
:    If --block-client is given, do not block any allocation for longer than this timeout (us).

`-c`, `--continuous-dump`
:    Dump interval in ms. 0 to disable continuous dump.

`-d`, `--duration`
:    Duration of profile (ms). 0 to run until interrupted. Default: until interrupted by user.

`--disable-fork-teardown`
:    Do not tear down client in forks. This can be useful for programs that use vfork. Android 11+ only.

`--disable-selinux`
:    Disable SELinux enforcement for duration of profile.

`--dump-at-max`
:    Dump the maximum memory usage rather than at the time of the dump.

`-h`, `--help`
:    show this help message and exit

`--heaps` _HEAPS_
:    Comma-separated list of heaps to collect, e.g: malloc,art. Requires Android 12.

`--idle-allocations`
:    Keep track of how many bytes were unused since the last dump, per callstack

`--no-android-tree-symbolization`
:    Do not symbolize using currently lunched target in the Android tree.

`--no-block-client`
:    When buffer is full, stop the profile early.

`--no-running`
:    Do not target already running processes. Requires Android 11.

`--no-start`
:    Do not start heapprofd.

`--no-startup`
:    Do not target processes that start during the profile. Requires Android 11.

`--no-versions`
:    Do not get version information about APKs.

`--print-config`
:    Print config instead of running. For debugging.

`--shmem-size`
:    Size of buffer between client and heapprofd. Default 8MiB. Needs to be a power of two multiple of 4096, at least 8192.

`--simpleperf`
:    Get simpleperf profile of heapprofd. This is only for heapprofd development.

`--traceconv-binary`
:    Path to local trace to text. For debugging.

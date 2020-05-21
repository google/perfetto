# heap_profile

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
                    [--trace-to-text-binary TRACE_TO_TEXT_BINARY]
                    [--print-config] [-o DIRECTORY]
```

## Options
|Option|Description|
|---|---|
| -n, --name | Comma-separated list of process names to profile. |
| -p, --pid | Comma-separated list of PIDs to profile. |
| -i, --interval | Sampling interval. Default 4096 (4KiB) |
| -o, --output | Output directory. |
| -d, --duration | Duration of profile (ms). Default 7 days. |
| --block-client | When buffer is full, block the client to wait for buffer space. Use with caution as this can significantly slow down the client. This is the default |
| --no-block-client | When buffer is full, stop the profile early. |
| --block-client-timeout | If --block-client is given, do not block any allocation for longer than this timeout (us). |
| -h, --help | Show this help message and exit |
| --no-start | Do not start heapprofd. |
| -c, --continuous-dump | Dump interval in ms. 0 to disable continuous dump. |
| --disable-selinux | Disable SELinux enforcement for duration of profile. |
| --no-versions | Do not get version information about APKs. |
| --no-running | Do not target already running processes. Requires Android 11. |
| --no-startup | Do not target processes that start during the profile. Requires Android 11. |
| --shmem-size | Size of buffer between client and heapprofd. Default 8MiB. Needs to be a power of two multiple of 4096, at least 8192. |
| --dump-at-max | Dump the maximum memory usage rather than at the time of the dump. |
| --disable-fork-teardown | Do not tear down client in forks. This can be useful for programs that use vfork. Android 11+ only. |
| --simpleperf | Get simpleperf profile of heapprofd. This is only for heapprofd development. |
| --trace-to-text-binary | Path to local trace to text. For debugging. |
| --print-config | Print config instead of running. For debugging. |

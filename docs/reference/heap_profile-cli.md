# HEAP_PROFILE(1)

## NAME

heap_profile - record a native heap profile on Android or local Linux

## DESCRIPTION

`tools/heap_profile` collects native memory profiles. It exposes two
subcommands:

* `heap_profile android` - profile a process on a connected Android device
  via `adb` (the previous behavior, still the default if no subcommand is
  given).
* `heap_profile host` - profile a local Linux process via `LD_PRELOAD`. The
  script auto-downloads `tracebox` and `libheapprofd_glibc_preload.so` and
  manages a local `traced` daemon for the duration of the session.

See [Recording traces](/docs/data-sources/native-heap-profiler.md) for more
details about the data source.

```
usage: heap_profile [-h] [common options] {android,host} ...

positional arguments:
  {android,host}
    android   Profile a process on a connected Android device via adb
              (default).
    host      Profile a local Linux process via LD_PRELOAD.
```

```
usage: heap_profile android [-h] [-i INTERVAL] [-d DURATION] [--no-start]
                            [-p PIDS] [-n NAMES] [-c CONTINUOUS_DUMP]
                            [--heaps HEAPS] [--all-heaps]
                            [--no-android-tree-symbolization]
                            [--disable-selinux] [--no-versions] [--no-running]
                            [--no-startup] [--shmem-size SHMEM_SIZE]
                            [--block-client]
                            [--block-client-timeout BLOCK_CLIENT_TIMEOUT]
                            [--no-block-client] [--idle-allocations]
                            [--dump-at-max] [--disable-fork-teardown]
                            [--simpleperf]
                            [--traceconv-binary TRACECONV_BINARY]
                            [--no-annotations] [--print-config] [-o DIRECTORY]
```

```
usage: heap_profile host [-h] [-i INTERVAL] [-d DURATION] [--no-start]
                         [-n NAMES] [-c CONTINUOUS_DUMP]
                         [--heaps HEAPS] [--all-heaps]
                         [--shmem-size SHMEM_SIZE] [--block-client]
                         [--block-client-timeout BLOCK_CLIENT_TIMEOUT]
                         [--no-block-client] [--idle-allocations]
                         [--dump-at-max] [--disable-fork-teardown]
                         [--traceconv-binary TRACECONV_BINARY]
                         [--no-annotations] [--print-config] [-o DIRECTORY]
                         [--preload-library PRELOAD_LIBRARY]
                         [--tracebox-binary TRACEBOX_BINARY]
                         -- COMMAND [ARGS...]
```

## COMMON OPTIONS

These flags apply to both `android` and `host` subcommands.

`-n`, `--name` _NAMES_
:    Comma-separated list of process names to profile. On `host`, if omitted,
     the basename of the command after `--` is used.

`-i`, `--interval`
:    Sampling interval. Default 4096 (4KiB).

`-o`, `--output` _DIRECTORY_
:    Output directory. Must be empty if it already exists.

`--all-heaps`
:    Collect allocations from all heaps registered by target.

`--heaps` _HEAPS_
:    Comma-separated list of heaps to collect, e.g.: `libc.malloc,com.android.art`. Requires Android 12.

`--block-client`
:    When buffer is full, block the client to wait for buffer space. Use with caution as this can significantly slow down the client. This is the default.

`--block-client-timeout`
:    If `--block-client` is given, do not block any allocation for longer than this timeout (us).

`--no-block-client`
:    When buffer is full, stop the profile early.

`-c`, `--continuous-dump`
:    Dump interval in ms. 0 to disable continuous dump.

`-d`, `--duration`
:    Duration of profile (ms). 0 to run until interrupted. Default: until interrupted by user.

`--disable-fork-teardown`
:    Do not tear down client in forks. This can be useful for programs that use vfork. Android 11+ only.

`--dump-at-max`
:    Dump the maximum memory usage rather than at the time of the dump.

`--idle-allocations`
:    Keep track of how many bytes were unused since the last dump, per callstack.

`--no-annotations`
:    Do not suffix the pprof function names with Android ART mode annotations such as `[jit]`.

`--no-running`
:    Do not target already running processes. Requires Android 11.

`--no-start`
:    No-op, kept for backwards compatibility.

`--no-startup`
:    Do not target processes that start during the profile. Requires Android 11.

`--print-config`
:    Print config instead of running. For debugging.

`--shmem-size`
:    Size of buffer between client and heapprofd. Default 8MiB. Needs to be a power of two multiple of 4096, at least 8192.

`--traceconv-binary`
:    Path to local traceconv. For debugging.

`-h`, `--help`
:    Show help message and exit.

## ANDROID-ONLY OPTIONS

These flags are gated on `args.subcommand == 'android'` in the script and
have no effect when passed to `host`.

`-p`, `--pid` _PIDS_
:    Comma-separated list of PIDs to profile.

`--disable-selinux`
:    Disable SELinux enforcement for duration of profile.

`--no-android-tree-symbolization`
:    Do not symbolize using currently lunched target in the Android tree.

`--no-versions`
:    Do not get version information about APKs.

`--simpleperf`
:    Get simpleperf profile of heapprofd. This is only for heapprofd development.

## HOST-ONLY OPTIONS

`--preload-library` _PRELOAD\_LIBRARY_
:    Path to `libheapprofd_glibc_preload.so`. If omitted the prebuilt is
     downloaded automatically (linux-amd64/arm/arm64).

`--tracebox-binary` _TRACEBOX\_BINARY_
:    Path to a local tracebox binary. For debugging.

`--` _COMMAND_ [_ARGS..._]
:    Required positional. The command to launch under `LD_PRELOAD`. The
     binary is run with `PERFETTO_HEAPPROFD_BLOCKING_INIT=1` so the first
     allocation blocks until heapprofd attaches.

## EXAMPLES

Profile `system_server` on a connected Android device until interrupted:

```bash
tools/heap_profile android -n system_server
```

Profile a local Linux binary, capturing every allocation from startup:

```bash
tools/heap_profile host -- ./my_binary --some-flag
```

Periodic 5-second snapshots of `com.example.app`:

```bash
tools/heap_profile android -n com.example.app -c 5000
```

Print the trace config a given invocation would emit, without running it:

```bash
tools/heap_profile android -n system_server --print-config
```

## NOTES

* The bare invocation `heap_profile -n NAME` (without a subcommand) is
  preserved for backwards compatibility and is equivalent to
  `heap_profile android -n NAME`. New scripts should use the explicit form.
* The `host` subcommand only runs on Linux; it errors out on other
  platforms.
* For symbolization and Java/Kotlin deobfuscation of the resulting trace,
  see [Symbolization and deobfuscation](/docs/learning-more/symbolization.md).

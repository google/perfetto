# Quickstart: Callstack sampling on Android

## Prerequisites

*   [ADB](https://developer.android.com/studio/command-line/adb) installed.
*   A device running Android R+.
*   Either a debuggable (`userdebug`/`eng`) Android image, or the apps to be
    profiled need to be
    [marked as profileable or debuggable](https://developer.android.com/guide/topics/manifest/profileable-element)
    in their manifests.

## Capture a CPU profile

### Linux or macOS

Make sure `adb` is installed and in your `PATH`.

```bash
adb devices -l
```

If more than one device or emulator is reported you must select one upfront as
follows:

```bash
export ANDROID_SERIAL=SER123456
```

Download `cpu_profile` (if you don't have a Perfetto checkout):

```bash
curl -LO https://raw.githubusercontent.com/google/perfetto/master/tools/cpu_profile
chmod +x cpu_profile
```

Then, start profiling. For example, to profile the processes `com.android.foo`
and `com.android.bar`, use:

```bash
./cpu_profile -n "com.android.foo,com.android.bar"
```

By default, profiling runs until manually terminated manually. To set a specific
duration for recording (e.g. 30 seconds), use:

```bash
./cpu_profile -n "com.android.foo,com.android.bar" -d 30000
```

To change how frequently stack samples are recorded (e.g. 120 samples per
second), set the `-f` argument:

```bash
./cpu_profile -n "com.android.foo,com.android.bar" -f 120
```

You can also pass in parts of the names of the processes you want to profile by
enabling `--partial-matching/-p`. This matches processes that are already
running when profiling is started. For instance, to profile the processes
`com.android.foo` and `com.android.bar`, run:

```bash
./cpu_profile -n "foo,bar" -p
```

You can also pass in a custom [Perfetto config](/docs/concepts/config.md), which
overrides all of the options above, using the `-c` argument:

```bash
./cpu_profile -c "path/to/perfetto.config"
```

To change where profiles are output, use the `-o` argument:

```bash
./cpu_profile -n "com.android.foo,com.android.bar" -o "path/to/output/directory"
```

### Windows

Make sure that the downloaded `adb.exe` is in the `PATH`.

```bash
set PATH=%PATH%;%USERPROFILE%\Downloads\platform-tools

adb devices -l
```

If more than one device or emulator is reported you must select one upfront as
follows:

```bash
set ANDROID_SERIAL=SER123456
```

Download the
[`cpu_profile`](https://raw.githubusercontent.com/google/perfetto/master/tools/cpu_profile)
script. Then, start profiling. For example, to profile the processes
`com.android.foo` and `com.android.bar`, use:

```bash
python3 /path/to/cpu_profile -n "com.android.foo,com.android.bar"
```

Please see the [Linux or maxOS section](#linux-or-macos) for more examples.

## Symbolization

You may need to symbolize the collected profiles if they are missing symbols.
See [this](/docs/data-sources/native-heap-profiler#symbolize-your-profile) for
more details on how to do this.

For example, to profile and symbolize the profiles for the process
`com.android.foo`, run:

```bash
PERFETTO_SYMBOLIZER_MODE=index PERFETTO_BINARY_PATH=path/to/directory/with/symbols/ ./cpu_profile -n "com.android.foo"
```

## View profile

Visualizing callstacks in the Perfetto UI is currently disabled behind a
flag. Please enable it before proceeding further:

![Enable flame graph flag](/docs/images/enable-profile-flame-graph.png)

Upload the `raw-trace` or `symbolized-trace` file from the output directory to
the [Perfetto UI](https://ui.perfetto.dev) and click and drag over one or more
of the diamond markers in the UI track named "Perf Samples" for the processes
that you selected for profiling. Each diamond marker represents a snapshot of
the call-stack at that point on the timeline.

![Profile Diamond](/docs/images/cpu-profile-diamond.png)
![Native Flamegraph](/docs/images/cpu-profile-flame.png)

`cpu_profile` will also write separate profiles for each process that it
profiled in the output directory, and those can be visualized using
[`pprof`](https://github.com/google/pprof).

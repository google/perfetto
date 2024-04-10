# Quickstart: Heap profiling

## Prerequisites

* [ADB](https://developer.android.com/studio/command-line/adb) installed.
* A device running Android 10+.
* A _Profileable_ or _Debuggable_ app. If you are running on a _"user"_ build of
  Android (as opposed to _"userdebug"_ or _"eng"_), your app needs to be marked
  as profileable or debuggable in its manifest.
  See the [heapprofd documentation][hdocs] for more details.

[hdocs]: /docs/data-sources/native-heap-profiler.md#heapprofd-targets

## Capture a heap profile

### Linux / macOS
Make sure adb is installed and in your PATH.

```bash
adb devices -l
```

If more than one device or emulator is reported you must select one upfront as follows:

```bash
export ANDROID_SERIAL=SER123456
```

Download the `tools/heap_profile` (if you don't have a perfetto checkout):

```bash
curl -LO https://raw.githubusercontent.com/google/perfetto/main/tools/heap_profile
chmod +x heap_profile
```

Then start the profile:

```bash
./heap_profile -n system_server
```

### Windows

Make sure that the downloaded adb.exe is in the PATH.

```bash
set PATH=%PATH%;%USERPROFILE%\Downloads\platform-tools

adb devices -l
```

If more than one device or emulator is reported you must select one upfront as follows:

```bash
set ANDROID_SERIAL=SER123456
```

Download the
[heap_profile](https://raw.githubusercontent.com/google/perfetto/main/tools/heap_profile)
script. Then start the profile:

```bash
python /path/to/heap_profile -n system_server
```

## View profile

Upload the `raw-trace` file from the output directory to the [Perfetto UI](
https://ui.perfetto.dev) and click on diamond marker in the UI track labeled
_"Heap profile"_.

![Profile Diamond](/docs/images/profile-diamond.png)
![Native Flamegraph](/docs/images/native-heap-prof.png)

## Next steps

Learn more about memory debugging in the [Memory Usage on Android Guide](
/docs/case-studies/memory.md) and more about the [heapprofd data-source](
/docs/data-sources/native-heap-profiler.md)

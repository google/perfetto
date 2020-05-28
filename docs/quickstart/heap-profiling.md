# Quickstart: Heap profiling

## Prerequisites

* A host running macOS or Linux.
* A device running Android 10+.
* A _Profileable_ or _Debuggable_ app. If you are running on a _"user"_ build of
  Android (as opposed to _"userdebug"_ or _"eng"_), your app needs to be marked
  as profileable or debuggable in its manifest.
  See the [heapprofd documentation][hdocs] for more details.

[hdocs]: /docs/data-sources/native-heap-profiler.md#heapprofd-targets

## Capture a heap profile

Download the `tools/heap_profile` (if you don't have a perfetto checkout) and
run it as follows:

```bash
curl -LO https://raw.githubusercontent.com/google/perfetto/master/tools/heap_profile
chmod +x heap_profile

./heap_profile -n system_server

Profiling active. Press Ctrl+C to terminate.
You may disconnect your device.

Wrote profiles to /tmp/profile-1283e247-2170-4f92-8181-683763e17445 (symlink /tmp/heap_profile-latest)
These can be viewed using pprof. Googlers: head to pprof/ and upload them.
```

## View profile

Upload the `raw-trace` file from the output directory to the [Perfetto UI](
https://ui.perfetto.dev) and click on diamond marker in the UI track labeled
_"Heap profile"_.

![Profile Diamond](/docs/images/profile-diamond.png)
![Native Flamegraph](/docs/images/syssrv-apk-assets-two.png)

## Next steps

Learn more about memory debugging in the [Memory Usage on Android Guide](
/docs/case-studies/memory.md) and more about the [heapprofd data-source](
/docs/data-sources/native-heap-profiler.md)

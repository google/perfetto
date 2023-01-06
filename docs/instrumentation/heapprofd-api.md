# heapprofd Custom Allocator API - Early Access

WARNING: The heapprofd Custom Allocator API is currently in **beta** stage.
         Please file [bugs](https://github.com/google/perfetto/issues/new)
         for any issues you encounter.

NOTE: The heapprofd Custom Allocator API requires a device running Android
      10 or newer.

## Get SDK

Before instrumenting your app, you need to get the heapprofd library and
header.

### Option 1: Prebuilts

You can download the library as a binary from [Google Drive](
https://drive.google.com/drive/folders/15RPlGgAHWRSk7KquBqlQ7fsCaXnNaa6r
).
Join our [Google Group](https://groups.google.com/forum/#!forum/perfetto-dev)
to get access.

### Option 2: Build yourself (on Linux)

Alternatively, you can build the binaries yourself from AOSP.

First, [check out Perfetto](https://perfetto.dev/docs/contributing/build-instructions):

```
$ git clone https://android.googlesource.com/platform/external/perfetto/
```

Then, change to the project directory, download and build additional
dependencies, and then build the standalone library:

```
$ cd perfetto
perfetto/ $ tools/install-build-deps --android
perfetto/ $ tools/setup_all_configs.py --android
perfetto/ $ ninja -C out/android_release_incl_heapprofd_arm64 \
libheapprofd_standalone_client.so
```

You will find the built library in
`out/android_release_incl_heapprofd_arm64/libheapprofd_standalone_client.so`.
The header for the API can be found in
`src/profiling/memory/include/perfetto/heap_profile.h`. This library is built
against SDK version 29, so will work on Android 10 or newer.

WARNING: Only use the header from the checkout you used to build the library,
         as the API is not stable yet.

To make debugging in the future easier, make note of the revision at the time
you built.

```
git rev-parse HEAD > perfetto-version.txt
```
Please include this in any bugs you file.

## Instrument App

Let's assume your application has a very simple custom allocator that looks
like this:

```
void* my_malloc(size_t size) {
  void* ptr = [code to somehow allocate get size bytes];
  return ptr;
}

void my_free(void* ptr) {
  [code to somehow free ptr]
}
```

To find out where in a program these two functions get called, we instrument
the allocator using this API:

```
#include "path/to/heap_profile.h"

static uint32_t g_heap_id = AHeapProfile_registerHeap(
  AHeapInfo_create("invalid.example"));
void* my_malloc(size_t size) {
  void* ptr = [code to somehow allocate get size bytes];
  AHeapProfile_reportAllocation(g_heap_id, static_cast<uintptr_t>(ptr), size);
  return ptr;
}

void my_free(void* ptr) {
  AHeapProfile_reportFree(g_heap_id, static_cast<uintptr_t>(ptr));
  [code to somehow free ptr]
}
```

Don't forget to link `heapprofd_standalone_client.so` and including it in
your app.

## Profile your App

Then, use the [heap_profile](
https://raw.githubusercontent.com/google/perfetto/master/tools/heap_profile)
script to get a profile to generate textpb of the config.
To convert to a binary proto, you additionally need to download
[`perfetto_trace.proto`](
https://raw.githubusercontent.com/google/perfetto/master/protos/perfetto/trace/perfetto_trace.proto)
and have recent version of the protoc compiler installed.
[Learn how to install protoc](https://grpc.io/docs/protoc-installation).

On Linux, you can start a profile using the following pipeline (substitue
`$APP_NAME` for the name of your app and `$HEAP` for the name of the heap
you registered using `AHeapProfile_registerHeap`):

```
heap_profile -n $APP_NAME --heaps $HEAP --print-config | \
 path/to/protoc --encode=perfetto.protos.TraceConfig perfetto_trace.proto | \
 adb shell perfetto -c - -o /data/misc/perfetto-traces/profile
```

On Windows, you will need [python 3.6](https://www.python.org/downloads/) or
later. You can start a profile using the following pipeline from a command
prompt (substitue`%APP_NAME%` for the name of your app and `%HEAP%` for
the name of the heap you registered using `AHeapProfile_registerHeap`):

```
python /path/to/heap_profile -n %APP_NAME% --heaps %HEAP% --print-config | ^
 path/to/protoc --encode=perfetto.protos.TraceConfig perfetto_trace.proto | ^
 adb shell perfetto -c - -o /data/misc/perfetto-traces/profile
```

Play around with the app to make it cause custom allocations, then stop the
profile using `adb shell killall perfetto`. Once it is done, pull the profile
from `/data/misc/perfetto-traces/profile` using `adb pull`.

Upload the profile to the [Perfetto UI](https://ui.perfetto.dev).

# Instrumenting Android apps/platform with atrace

In this guide, you'll learn how to:

- Add `ATrace` instrumentation to your Android application or platform code.
- Record and visualize `ATrace` events in the Perfetto UI.
- Understand the difference between `ATrace` and the Perfetto Tracing SDK.

This page is mainly intended for:

- Android platform engineers for instrumenting their platform services.
- System integrators / Android partners for instrumenting their native HALs and
  Java/Kt services.
- Native and Java/Kt app developers for instrumenting their apps (although you
  should consider using
  [androidx.tracing](https://developer.android.com/jetpack/androidx/releases/tracing),
  more below)

![Atrace slices example](/docs/images/atrace_slices.png)

Atrace is an API introduced in Android 4.3 that predates Perfetto and allows you to add
instrumentation to your code. It is still supported and in use, and
interoperates well with Perfetto.

Under the hood, Atrace forwards events up to the kernel ftrace ring-buffer and
gets fetched together with the rest of scheduling data and other system-level
trace data. Atrace is both:

1. A public API, exposed both to Java/Kt code via the Android SDK and C/C++ code
   via the NDK, that developers can use to enrich traces annotating their apps.
2. A private platform API used to annotate several framework functions and the
   implementation of core system services. It provides developers with insights
   about what the framework is doing under the hoods.

The main difference between the two is that the private platform API allows
specifying a _tag_ (also known as _category_), while the SDK/NDK interface
implicitly uses TRACE_TAG_APP.

In both cases, Atrace allows you to manually add instrumentation around code
wall timing and numeric values, e.g. to annotate the beginning or end of
functions, logical user journeys, state changes.

## Thread-scoped synchronous slices

Slices are used to create rectangles around the execution of code and visually
form a pseudo-callstack.

Semantic and constraints:

- **API**: Slices are emitted with begin/end APIs.
- **Balancing**: Begin/end MUST be balanced and must happen on the same thread.
- **Visualization**: Slices are visualized in a thread-scoped track (as in the
  picture above).
- **Cross-thread**: See [Cross-thread async slices](#cross-thread-async-slices)
  below for cross-thread use-cases.

<?tabs>

TAB: Java (platform private)

Refer to [frameworks/base/core/java/android/os/Trace.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/core/java/android/os/Trace.java?q=frameworks%2Fbase%2Fcore%2Fjava%2Fandroid%2Fos%2FTrace.java)

```java
import android.os.Trace;
import static android.os.Trace.TRACE_TAG_AUDIO;

public void playSound(String path) {
  Trace.traceBegin(TRACE_TAG_AUDIO, "PlaySound");
  try {
    // Measure the time it takes to open the sound service.
    Trace.traceBegin(TRACE_TAG_AUDIO, "OpenAudioDevice");
    try {
      SoundDevice dev = openAudioDevice();
    } finally {
      Trace.traceEnd();
    }

    for(...) {
      Trace.traceBegin(TRACE_TAG_AUDIO, "SendBuffer");
      try {
        sendAudioBuffer(dev, ...)
      } finally {
        Trace.traceEnd();
      }
      // Log buffer usage statistics in the trace.
      Trace.setCounter(TRACE_TAG_AUDIO, "SndBufferUsage", dev.buffer)
      ...
    }
  } finally {
    Trace.traceEnd();  // End of the root PlaySound slice
  }
}
```

TAB: C/C++ (platform private)

```c++
// ATRACE_TAG is the category that will be used in this translation unit.
// Pick one of the categories defined in Android's
// system/core/libcutils/include/cutils/trace.h
#define ATRACE_TAG ATRACE_TAG_AUDIO

#include <cutils/trace.h>

void PlaySound(const char* path) {
  ATRACE_BEGIN("PlaySound");

  // Measure the time it takes to open the sound service.
  ATRACE_BEGIN("OpenAudioDevice");
  struct snd_dev* dev = OpenAudioDevice();
  ATRACE_END();

  for(...) {
    ATRACE_BEGIN("SendBuffer");
    SendAudioBuffer(dev, ...)
    ATRACE_END();

    // Log buffer usage statistics in the trace.
    ATRACE_INT("SndBufferUsage", dev->buffer);
    ...
  }

  ATRACE_END();  // End of the root PlaySound slice
}
```

TAB: Java (SDK)

Refer to the [SDK reference documentation for os.trace](https://developer.android.com/reference/android/os/Trace).

```java
// You cannot choose a tag/category when using the SDK API.
// Implicitly all calls use the ATRACE_TAG_APP tag.
import android.os.Trace;

public void playSound(String path) {
  try {
    Trace.beginSection("PlaySound");

    // Measure the time it takes to open the sound service.
    Trace.beginSection("OpenAudioDevice");
    try {
      SoundDevice dev = openAudioDevice();
    } finally {
      Trace.endSection();
    }

    for(...) {
      Trace.beginSection("SendBuffer");
      try {
        sendAudioBuffer(dev, ...)
      } finally {
        Trace.endSection();
      }

      // Log buffer usage statistics in the trace.
      Trace.setCounter("SndBufferUsage", dev.buffer)
      ...
    }
  } finally {
    Trace.endSection();  // End of the root PlaySound slice
  }
}
```

TAB: C/C++ (NDK)

Refer to the [NDK reference documentation for Tracing](https://developer.android.com/ndk/reference/group/tracing).

```c++
// You cannot choose a tag/category when using the NDK API.
// Implicitly all calls use the ATRACE_TAG_APP tag.
#include <android/trace.h>

void PlaySound(const char* path) {
  ATrace_beginSection("PlaySound");

  // Measure the time it takes to open the sound service.
  ATrace_beginSection("OpenAudioDevice");
  struct snd_dev* dev = OpenAudioDevice();
  ATrace_endSection();

  for(...) {
    ATrace_beginSection("SendBuffer");
    SendAudioBuffer(dev, ...)
    ATrace_endSection();

    // Log buffer usage statistics in the trace.
    ATrace_setCounter("SndBufferUsage", dev->buffer)
    ...
  }

  ATrace_endSection();  // End of the root PlaySound slice
}
```
</tabs?>

## Counters

Semantic and constraints:

- **Threading**: Counters can be emitted from any thread.
- **Visualization**: Counters are visualized in a process-scoped track named
  after the counter name (the string argument). Each new counter name
  automatically yields a new track in the UI. Counter events from different
  threads within a process are folded into the same process-scoped track.

<?tabs>

TAB: Java (platform private)

Refer to [frameworks/base/core/java/android/os/Trace.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/core/java/android/os/Trace.java?q=frameworks%2Fbase%2Fcore%2Fjava%2Fandroid%2Fos%2FTrace.java)

```java
import android.os.Trace;
import static android.os.Trace.TRACE_TAG_AUDIO;

public void playSound(String path) {
  SoundDevice dev = openAudioDevice();
  for(...) {
    sendAudioBuffer(dev, ...)
    ...
    // Log buffer usage statistics in the trace.
    Trace.setCounter(TRACE_TAG_AUDIO, "SndBufferUsage", dev.buffer.used_bytes)
  }
}
```

TAB: C/C++ (platform private)

```c++
// ATRACE_TAG is the category that will be used in this translation unit.
// Pick one of the categories defined in Android's
// system/core/libcutils/include/cutils/trace.h
#define ATRACE_TAG ATRACE_TAG_AUDIO

#include <cutils/trace.h>

void PlaySound(const char* path) {
  struct snd_dev* dev = OpenAudioDevice();

  for(...) {
    SendAudioBuffer(dev, ...)

    // Log buffer usage statistics in the trace.
    ATRACE_INT("SndBufferUsage", dev->buffer.used_bytes);
  }
}
```

TAB: Java (SDK)

Refer to the [SDK reference documentation for os.trace](https://developer.android.com/reference/android/os/Trace).

```java
// You cannot choose a tag/category when using the SDK API.
// Implicitly all calls use the ATRACE_TAG_APP tag.
import android.os.Trace;

public void playSound(String path) {
  SoundDevice dev = openAudioDevice();

  for(...) {
    sendAudioBuffer(dev, ...)

    // Log buffer usage statistics in the trace.
    Trace.setCounter("SndBufferUsage", dev.buffer.used_bytes)
  }
}
```

TAB: C/C++ (NDK)

Refer to the [NDK reference documentation for Tracing](https://developer.android.com/ndk/reference/group/tracing).

```c++
// You cannot choose a tag/category when using the NDK API.
// Implicitly all calls use the ATRACE_TAG_APP tag.
#include <android/trace.h>

void PlaySound(const char* path) {
  struct snd_dev* dev = OpenAudioDevice();

  for(...) {
    SendAudioBuffer(dev, ...)

    // Log buffer usage statistics in the trace.
    ATrace_setCounter("SndBufferUsage", dev->buffer.used_bytes)
  }
}
```
</tabs?>

## Cross-thread async slices

Async slices allow to trace logical operations that might begin and end on
different threads. They are the same concept of _track events_ in the Perfetto
SDK.

Because begin/end can happen on different thread, you need to pass a _cookie_ to
each begin/end function. The cookie is just an integer number used to match
begin/end pairs. The cookie is usually derived from a pointer or a unique ID
that represents the logical operation being traced (e.g. a job id).

Semantic and constraints:

- **Overlapping**: Because of their async nature, slices can overlap temporally:
  one operation might begin before the previous one has ended.
- **Cookies**: Cookies must be unique within a process. You cannot have a begin
  event for the same cookie before having emitted an end event for it. In other
  words, cookies are a shared integer namespace within the process. Using a
  monotonic counter is probably a bad idea unless you have full control of all
  the code in the process.
- **Nesting and Tracks**: Unlike thread-scoped slices, nesting/stacking is only
  possible when using the private platform API. The `...ForTrack` functions
  allow you to specify a track name, and all events with the same track name
  will be grouped in the same process-scoped track in the UI. Within a track,
  nesting is controlled by the `cookie` parameter. The SDK/NDK API does not
  support nesting, and the track is derived from the event name.
- **Stacking**: Visually, the UI lays slice within each track using a greedy
  stacking algorithm. Each slice is placed in the uppermost lane that doesn’t
  overlap with any other slice. This sometimes generates confusion amongst users
  as it creates a false sense of "parent/child" relationship. However, unlike
  sync slices, the relationship is purely temporal and not causal and you cannot
  control it (other than grouping events into tracks, if you have access to the
  private platform API).

<?tabs>

TAB: Java (platform private)

Refer to [frameworks/base/core/java/android/os/Trace.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/core/java/android/os/Trace.java?q=frameworks%2Fbase%2Fcore%2Fjava%2Fandroid%2Fos%2FTrace.java)

```java
import android.os.Trace;
import static android.os.Trace.TRACE_TAG_NETWORK;

public class AudioRecordActivity extends Activity {
  private AtomicInteger lastJobId = new AtomicInteger(0);
  private static final String TRACK_NAME = "User Journeys";

    ...
    button.setOnClickListener(v -> {
        int jobId = lastJobId.incrementAndGet();
        Trace.asyncTraceForTrackBegin(TRACE_TAG_NETWORK, TRACK_NAME, "Load profile", jobId);

        // Simulate async work (e.g., a network request)
        new Thread(() -> {
            Thread.sleep(800); // emulate latency
            Trace.asyncTraceForTrackEnd(TRACE_TAG_NETWORK, TRACK_NAME, jobId);
        }).start();
    });
    ...
}
```

TAB: C/C++ (platform private)

```c++
// ATRACE_TAG is the category that will be used in this translation unit.
// Pick one of the categories defined in Android's
// system/core/libcutils/include/cutils/trace.h
#define ATRACE_TAG ATRACE_TAG_NETWORK

#include <cutils/trace.h>
#include <thread>
#include <chrono>
#include <atomic>

static constexpr const char* kTrackName = "User Journeys";

void onButtonClicked() {
  static std::atomic<int> lastJobId{0};

  int jobId = ++lastJobId;
  ATRACE_ASYNC_FOR_TRACK_BEGIN(kTrackName, "Load profile", jobId);

  std::thread([jobId]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(800));
      ATRACE_ASYNC_FOR_TRACK_END(kTrackName, jobId);
  }).detach();
}
```

TAB: Java (SDK)

Refer to the [SDK reference documentation for os.trace](https://developer.android.com/reference/android/os/Trace).

```java
// You cannot choose a tag/category when using the SDK API.
// Implicitly all calls use the ATRACE_TAG_APP tag.
import android.os.Trace;

public class AudioRecordActivity extends Activity {
  private AtomicInteger lastJobId = new AtomicInteger(0);

    ...
    button.setOnClickListener(v -> {
        int jobId = lastJobId.incrementAndGet();
        Trace.beginAsyncSection("Load profile", jobId);

        // Simulate async work (e.g., a network request)
        new Thread(() -> {
            Thread.sleep(800); // emulate latency
             Trace.endAsyncSection("Load profile", jobId);
        }).start();
    });
    ...
}
```

TAB: C/C++ (NDK)

Refer to the [NDK reference documentation for Tracing](https://developer.android.com/ndk/reference/group/tracing).

```c++
// You cannot choose a tag/category when using the NDK API.
// Implicitly all calls use the ATRACE_TAG_APP tag.
#include <android/trace.h>
#include <thread>
#include <chrono>
#include <atomic>

void onButtonClicked() {
  static std::atomic<int> lastJobId{0};

  int jobId = ++lastJobId;
  ATrace_beginAsyncSection("Load profile", jobId);

  std::thread([jobId]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(800));
      ATrace_endAsyncSection("Load profile", jobId);
  }).detach();
}
```
</tabs?>

## Should I use Atrace or the Perfetto Tracing SDK?

At the time of writing, there isn't a clear-cut answer to this question. Our
team is working on providing a replacement SDK that can subsume all the atrace
use cases, but we are not there yet. So the answer is: _depends_.

| When to prefer Atrace                                                                                                           | When to prefer the Tracing SDK                                                                                 |
| ------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| You need something simple that just works.                                                                                      | You need more advanced features (e.g. flows).                                                                  |
| You are okay with one on/off toggle for the whole app. (If you are in the Android system you can only use a limited set of tags) | You need fine-grained control over tracing categories.                                                         |
| You are okay with events being multiplexed in the main ftace buffer.                                                            | You want control over muxing events in different buffers.                                                       |
| Instrumentation overhead is not a big concern, your trace points are hit sporadically.                                          | You want minimal overhead for your instrumentation points. Your trace points are frequent (every 10ms or less) |

#### If you are an unbundled app

You should consider using
[androidx.tracing](https://developer.android.com/jetpack/androidx/releases/tracing)
from Jetpack. We work closely with the Jetpack project. Using androidx.tracing
is going to lead to a smoother migration path once we improve our SDK.

## Recording the trace

In order to record atrace you must enable the `linux.ftrace` data source and add
in the `ftrace_config`:

- For platform private system services: `atrace_categories: tag_name`
- For apps: `atrace_apps: "com.myapp"` or `atrace_apps: "*"` for all apps.

You can see the full list of atrace categories
[here](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/native/cmds/atrace/atrace.cpp;l=102?q=f:atrace.cpp%20k_categories).

<?tabs>

TAB: UI

![Atrace recording via UI](/docs/images/atrace_ui_recording.png)

TAB: Command line

```sh
curl -O https://raw.githubusercontent.com/google/perfetto/main/tools/record_android_trace

python3 record_android_trace \
  -o trace_file.perfetto-trace \
  -t 10s \
  # To record atrace from apps.
  -a 'com.myapp'  \  # or '*' for tracing all apps
  # To record atrace from system services.
  am wm webview
```

TAB: Raw config

```js
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      atrace_categories: "am"
      atrace_categories: "wm"
      atrace_categories: "webview"
      atrace_apps: "com.myapp1"
      atrace_apps: "com.myapp2"
    }
  }
}
```
</tabs?>

## Next Steps

Now that you've learned how to instrument your code with `ATrace`, here are some
other documents you might find useful:

### Recording traces

- **[Recording system traces](/docs/getting-started/system-tracing.md)**: Learn
  more about recording traces on Android.

### Other Android data sources

- **[Scheduling data](/docs/data-sources/cpu-scheduling.md)**: See which threads
  are running on which CPU.
- **[CPU frequency](/docs/data-sources/cpu-freq.md)**: See how fast each CPU is
  running.

### Analyzing traces

- **[Perfetto UI](/docs/visualization/perfetto-ui.md)**: Learn about all the
  features of the trace viewer.

# Instrumenting Android apps/platform with atrace

In this page you will learn how to add instrumentation to code in Android apps,
services and platform code. This will allow you to emit slices and counters
into the trace as follows like in the image below.

This page is mainly intended for:

- Android platform engineers for instrumenting their platform services.
- System integrators / Android partners for instrumenting their native HALs and
  Java/Kt services.
- Native and Java/Kt app developers for instrumenting their apps.

![Atrace slices example](/docs/images/atrace_slices.png)

## Introduction

Atrace is a very legacy API introduced in Android 4.3 that predates Perfetto. It
is still supported and in use and interoperates well with Perfetto.
Under the hoods, Atrace is based on forwarding events up in the kernel ftrace
ring-buffer and gets fetched together with the rest of scheduling data and
other system-level trace data.

Usage-wise, Atrace is useful when manually adding instrumentation around code,
e.g. to annotate the beginning or end of functions or logical sections;
state changes; counter value changes. Atrace has a dual use: it is used both by:

- Android internals, both native C/C++ and managed Java code.
A lot of the framework code today is instrumented, both on the server side
(e.g., services running in system_server) and on the client side
(e.g., framework API calls)

- Vendor HALs and services.

- Unbundled apps and services (although you should consider using [androidx.tracing](https://developer.android.com/jetpack/androidx/releases/tracing), see below)



## Emitting simple slices and counters

Slices are emitted with begin/end APIs. By default slices are emitted in a
thread-scoped track (as in the picture above).

<?tabs>

TAB: C/C++ (Android tree)

```c++

// ATRACE_TAG is the category that will be used in this translation unit.
// Pick one of the categories defined in Android's
// system/core/libcutils/include/cutils/trace.h
#define ATRACE_TAG ATRACE_TAG_AUDIO

#include <cutils/trace.h>

void PlaySound(const char* path) {
  ATRACE_BEGIN("PlaySound");

  // Measure the time it takes to open the sound sevice.
  ATRACE_BEGIN("OpenAudioDevice");
  struct snd_dev* dev;
  dev = OpenAudioDevice();
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

TAB: Java (Android tree)

Refer to [frameworks/base/core/java/android/os/Trace.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/core/java/android/os/Trace.java?q=frameworks%2Fbase%2Fcore%2Fjava%2Fandroid%2Fos%2FTrace.java)

```java

import android.os.Trace;
import static android.os.Trace.TRACE_TAG_AUDIO;

public void playSound(String path) {
  Trace.traceBegin(TRACE_TAG_AUDIO, "PlaySound");

  // Measure the time it takes to open the sound sevice.
  Trace.traceBegin(TRACE_TAG_AUDIO, "OpenAudioDevice");
  SoundDevice dev = openAudioDevice();
  Trace.traceEnd();

  for(...) {
    Trace.traceBegin(TRACE_TAG_AUDIO, "SendBuffer");
    sendAudioBuffer(dev, ...)
    Trace.traceEnd();

    // Log buffer usage statistics in the trace.
    Trace.setCounter(TRACE_TAG_AUDIO, "SndBufferUsage", dev->buffer)
    ...
  }

  Trace.traceEnd();  // End of the root PlaySound slice
}
```

TAB: C/C++ (NDK)

Refer to the [NDK reference documentation for Tracing](https://developer.android.com/ndk/reference/group/tracing).

```c++

// You cannot choose a tag/category when using the NDK API.
// Implicitly all calls use the ATRACE_TAG_APP tag.
#include <android/trace.h>

...

void PlaySound(const char* path) {
  ATrace_beginSection("PlaySound");

  // Measure the time it takes to open the sound sevice.
  ATRACE_BEGIN("OpenAudioDevice");
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

TAB: Java (SDK)

Refer to the [SDK reference documentation for os.trace](https://developer.android.com/reference/android/os/Trace).

```java

// You cannot choose a tag/category when using the SDK API.
// Implicitly all calls use the ATRACE_TAG_APP tag.

import android.os.Trace;

public void playSound(String path) {
  ATrace_beginSection("PlaySound");

  // Measure the time it takes to open the sound sevice.
  Trace.beginSection("OpenAudioDevice");
  SoundDevice dev = openAudioDevice();
  Trace.endSection();

  for(...) {
    Trace.beginSection("SendBuffer");
    sendAudioBuffer(dev, ...)
    Trace.endSection();

    // Log buffer usage statistics in the trace.
    Trace.setCounter("SndBufferUsage", dev->buffer)
    ...
  }

  Trace.endSection();  // End of the root PlaySound slice
}
```
</tabs?>


### Should I use Atrace or the Perfetto Tracing SDK?

At the time of writing, there isn't a clear-cut answer to this question. Our
team is working on providing a replacement SDK that can subsume all the atrace
use cases, but we are not there yet. So the answer is: "depends".

| When to prefer Atrace             | When to prefer the Tracing SDK      |
| --------------------------------- | ----------------------------------- |
| You need something simple that  just works. | You need more advanced features (e.g. flows). |
| You are okay with your instrumentation being all-or-nothing | You need fine-grained control over tracing categories. |
| You are okay with events being multiplexed in the main ftace buffer. | You want control over muxing events in different buffers. |
| You care only about [system-level traces](/docs/getting-started/recording/system-tracing). | You need [in-app](/docs/getting-started/recording/in-app-tracing) traces. |
| Instrumentation overhead is not a big concern. | You want mininmal overhead for your instrumentation points. Your instrumentation |
| Your trace points are hit sporadically. | Your trace points are hit more frequently than every ~10ms. |

#### If you are an unbundled app

You should consider using [androidx.tracing](https://developer.android.com/jetpack/androidx/releases/tracing)
from Jetpack. Our team works closely with the Jetpack project.
Using androidx.tracing is going to lead to a smoother migration path once we
improve our SDK.

## Adding atrace instrumentation



<?tabs>

TAB: Java

Use record_android_trace script

TAB: C++

Go through example on adding atrace

</tabs?>

## Viewing your recorded trace

Video: open trace in Perfetto UI, navigate around

## Instrumentation types (quick reference)

### App and platform developers

Slices: operations on a single thread

Async slices: operations spanning multiple threads

Counters

### Platform developers only

Instants

Async slices on track

## Next Steps

Collect system traces: see other page

Non-atrace instrumentation

# What is Perfetto?

TIP: If you are unfamiliar with tracing or, in general, are new to the world of
performance, we suggest reading the [What is Tracing?](/docs/tracing-101.md)
page first.

Perfetto is an open-source suite of SDKs, daemons and tools which use
**tracing** to help developers understand the behaviour of the complex systems
and root-cause functional and performance issues on client / embedded systems.

It consists of:

- **High-performance tracing daemons** for capturing tracing information from
  many processes on a single machine into a unified trace file for offline
  analysis and visualization.
- **Low-overhead tracing SDK** for direct userspace-to-userspace tracing of
  timings and state changes of your C/C++ code.
- **Extensive OS-level probes on Android and Linux** for capturing wider system
  level (e.g. scheduling states, CPU frequencies, memory profiling, callstack
  sampling) context during the trace.
- **Fully local, browser-based UI** for visualizing large amounts of complex,
  interconnected data on a timeline. Our UI works in all major browsers, doesn't
  require any installation, works offline, and can open traces recorded by other
  (non-Perfetto) tracing tools.
- **Powerful, SQL-based analysis library** for programatically analyzing large
  amounts of complex, interconnected data on a timeline, even if it was not
  collected with Perfetto recording tooling.

![](images/perfetto-stack.svg)

## Why would you use Perfetto?

Perfetto was designed from the ground up to act as the default tracing system
for the Android OS and the Chrome Browser. As such, Perfetto is officially
supported for collecting, analysing and visualizing:

- **System traces on Android** to debug and root-cause functional and
  performance issues in the Android platform and Android apps. Perfetto is
  suited for debugging e.g. slow startups, dropped frames (jank), animation
  glitches, low memory kills, App Not Responding (ANRs) and general buggy
  behaviour.

- **Java heap dumps and native heap profiles on Android** to debug and
  root-cause high memory use in both Java/Kotlin code and C++ code respectively,
  in the Android platform and Android apps.
- **Callstack sampling profiles on Android** to debug and root-cause high CPU
  usage by C++/Java/Kotlin code in the Android platform and Android apps.
- **Chrome browser traces** to debug and root cause issues in the browser, V8,
  Blink and, in advanced usecases, in websites themselves.

Beyond these "official" usecases, Perfetto consists a highly flexible set of
tools. This makes it capable of being used as a general purpose tracing system,
a performance data analyzer or a timeline visualizer. The Perfetto team
dedicates a porition of their time to supporting these cases, albeit at a
reduced level of support.

Other usecases Perfetto is commonly used for include:

- **Collecting, analysing and visualizing in-app traces** to debug functional
  and performance issues in C/C++ apps and libraries on Windows, macOS and
  Linux-based embedded systems.
- **Collecting, analysing and visualizing heap profiles on Linux** to debug high
  memory usage of C/C++/Rust apps and libraries.
- **Analysing and visualizing CPU profiles (Linux perf profiles) on Linux** to
  optmize CPU usage in C/C++/Rust apps and libraries.
- **Analyze and visualize a wide range of profiling and tracing formats.**
  Perfetto can open traces and profiles from various other tools, allowing you
  to use the Perfetto UI and its SQL-based query engine on many data sources,
  including:
  - _Chrome JSON format_
  - _Firefox Profiler JSON format_
  - _Linux perf (binary and text formats)_
  - _Linux ftrace text format_
  - _macOS Instruments_
  - _Fuchsia tracing format_
- **Analysing and visualizing arbitrary "trace-like" data**. The Perfetto
  analysis and visualization tools can be used on any "trace-like" data (e.g.
  data with a timestamp and some payload) as long as it can be converted to the
  Perfetto protobuf format; the possibilities are only limited by creativity!

## Why would you **not** use Perfetto?

There are several types of problems Perfetto is either not designed for or is
explicltly unsupported.

- **Recording traces for distributed / server systems**

  - Perfetto is **not** a distributed tracer in the vein of OpenTelemetry,
    Jaeger, Datadog. Perfetto's recording tools are entirely for recording
    client side traces, especially at the system level. Our team believes that
    the space of distributed/server tracing is well covered by the
    aforementioned projects, unlike client side systems like Android and
    Linux/embeded systems.
  - However, the Perfetto UI **can** be used to visualize distributed traces if
    traces are converted to a format that Perfetto supports. In fact, this is
    commonly done inside Google.

- **Recording system traces on Windows or macOS**

  - Perfetto's recording tools do **not** integrate with any system level data
    sources on Windows or macOS.
  - However, Perfetto _can_ be used to analyse and visualize macOS traces
    collected with Instruments as we natively support the Instruments XML
    format.

- **Consuming traces on the critical path**

  - Perfetto's producer code is optimized for low-overhead trace writing but the
    consumer side is _not_ optimized for low-latency readback.
  - This means it is _not_ advised to use Perfetto for situations where you want
    end-to-end low-latency tracing.

- **Recording traces with the lowest overhead possible**

  - Perfetto SDK makes no claims on being the fastest possible way to record
    traces: we are well aware that there will be libraries and tools out there
    which can capture traces with less overhead. You can beat our tracing SDK by
    recording fixed-sized events in a shmem ring buffer bumping atomic pointers.
  - Instead, Perfetto's recording libraries and daemons focus on having a good
    trade-off between performance, flexibility and security of tracing.
  - For example, Perfetto supports arbitrary sized events (e.g. high res
    screenshots), coordinating multi-process tracing, concurrent tracing
    sessions with different configs, dynamic buffer multiplexing, arbitrarily
    nested key-value **arguments** attached to trace events, dynamic string
    interning, **flows** for linking trace events together and **dynamic trace
    event names** which many other low-overhead tracing systems do not support.
  - However, the Perfetto UI _can_ be used to visualize traces recorded with
    non-Perfetto tools if those traces can be converted to the Perfetto protobuf
    format or some other format we support natively e.g. _Chrome JSON_,
    _Fuchsia_ etc.

- **Recording, analysing or visualizing GPU traces for games**

  - Tracing and profiling of games is very different world to tracing general
    purpose software for many reasons: the orientation of the whole system
    around "frames", the heavy focus on the GPU and its utilization, the
    presence of game engines and the need to integrate with them.
  - Due to Perfetto not having any specialized focus on the things game
    developers care heavily about, we feel like Perfetto is not well suited to
    this task.
  - We have some support for GPU render stages and GPU counters recording on
    Android, but these features are better supported by
    [Android GPU Inspector](https://gpuinspector.dev) (which under the hoods
    uses Perfetto as one of its data sources).

## How do I get started using Perfetto?

We appreicate that Perfetto has a lot of parts to it so it can be confusing to
someone new to the project to know what is relevant to them. For this reason, we
have a whole page dedicated to this:
[How do I start using Perfetto?](/docs/getting-started/start-using-perfetto.md)

## {#who-uses-perfetto} Who uses Perfetto today?

Perfetto is the **default tracing system** for the **Android operating system**
and the **Chromium browser**. As such, Perfetto is utilized extensively by these
teams in Google, both to proactively identify performance improvements and
reactively to debug/root-cause issues locally, in the lab and even from the
field.

There are also many other teams in Google who use Perfetto in diverse ways. This
includes including "non-traditional" uses of a tracing system. Perfetto has also
been used and adopted widely in the wider industry by many other companies.

The following is a non-exhaustive list of public mentions of Perfetto in blog
posts, articles and videos:

- [Google IO 2023 - What's new in Dart and Flutter](https://youtu.be/yRlwOdCK7Ho?t=798)
- [Google IO 2023 - Debugging Jetpack Compose](https://youtu.be/Kp-aiSU8qCU?t=1092)
- [Performance: Perfetto Traceviewer - MAD Skills](https://www.youtube.com/watch?v=phhLFicMacY)
  "On this episode of the MAD Skills series on Performance, Android Performance
  Engineer Carmen Jackson discusses the Perfetto traceviewer, an alternative to
  Android Studio for viewing system traces."
- [Performance and optimisation on the Meta Quest Platform](https://m.facebook.com/RealityLabs/videos/performance-and-optimization-on-meta-quest-platform/488126049869673/)
- [Performance testing through proportional traces ](https://www.jviotti.com/2022/09/07/performance-testing-through-proportional-traces.html)
- [Performance](https://www.twoscomplement.org/podcast/performance.mp3) An
  episode of the
  [Twoscomplement podcast](https://www.twoscomplement.org/#podcast) "Our most
  efficient podcast ever. Ben and Matt talk performance testing and optimization
  in fewer than 30 minutes."
- [Collabora: Profiling virtualized GPU acceleration with Perfetto](https://www.collabora.com/news-and-blog/blog/2021/04/22/profiling-virtualized-gpu-acceleration-with-perfetto/)
- [Snap: Client Tracing at Scale](https://www.droidcon.com/2022/06/28/client-tracing-at-scale/)
  "With the wide range of Android devices, it can be difficult to find the root
  cause of performance problems. By leveraging traces, we can begin to
  understand the exact circumstances that led to a poor user experience. We will
  discuss how we instrument our Snapchat app such that we can have the necessary
  signals for explainability. Additionally, we will describe how we incorporate
  tracing into our development process from local debugging, to performance
  tests and finally in production."
- [Microsoft: Perfetto tooling for analyzing Android, Linux, and Chromium browser performance](https://devblogs.microsoft.com/performance-diagnostics/perfetto-tooling-for-analyzing-android-linux-and-chromium-browser-performance-microsoft-performance-tools-linux-android/)
- [Mesa 3D](https://docs.mesa3d.org/perfetto.html) embeds Perfetto in some of
  its drivers for GPU counter and render stage monitoring.
- [JaneStreet's MagicTrace](https://blog.janestreet.com/magic-trace/) A tool
  based on Perfetto to record and visualize Intel Processor traces.

## Where do I find more information and get help with Perfetto?

For our source code and project home:
[Github](https://github.com/google/perfetto)

For Q/A:

- [Github Discussions](https://github.com/google/perfetto/discussions/categories/q-a)
  or our
  [public mailing list](https://groups.google.com/forum/#!forum/perfetto-dev).
- **Googlers**: use [YAQS](https://go/perfetto-yaqs) or our
  [internal mailing list](http://go/perfetto-dev).

For bugs affecting any part of Perfetto **except** Chrome tracing:

- [GitHub issues](https://github.com/google/perfetto/issues).
- **Googlers**: use the internal bug tracker
  [go/perfetto-bugs](http://goto.google.com/perfetto-bugs)

For bugs affecting Chrome Tracing:

- Use http://crbug.com `Component:Speed>Tracing label:Perfetto`.

For chatting directly with the Perfetto team:

- Use [Discord](https://discord.gg/35ShE3A)
- **Googlers**: Thank you for contacting us. All our lines are currently busy.
  Your message is important to us, and an operator will get back to you as soon
  as possible. If your enquiry is truly urgent see
  [this page](http://go/perfetto-project).

Perfetto follows
[Google's Open Source Community Guidelines](https://opensource.google/conduct/).

# What is Perfetto?

TIP: If you are unfamiliar with tracing or, in general, are new to the world of
performance, we suggest reading
[Tracing and profiling, explained](/docs/tracing-101.md) first.

Perfetto is an open-source suite of SDKs, daemons and tools which use
**tracing** and **profiling** to help developers understand the behaviour of
complex systems and root-cause functional and performance issues on client /
embedded systems.

It consists of:

<div class="docs-home">
<figure class="home-figure">
<img src="/docs/images/perfetto-stack.svg" alt="How Perfetto's components fit together: traces recorded with Perfetto or imported from other tools can be explored in the Perfetto UI or queried with the Trace Processor">
</figure>
<ol class="home-key"><li><span class="home-badge">1</span><p><strong>Low-overhead tracing SDK</strong> for direct userspace-to-userspace tracing of timings and state changes of your C/C++ code. A community-maintained <a href="/docs/getting-started/rust-sdk">Rust SDK</a> is also available.</p></li>
<li><span class="home-badge">2</span><p><strong>Extensive OS-level probes on Android and Linux</strong> for capturing wider system level (e.g. scheduling states, CPU frequencies, memory profiling, callstack sampling) context during the trace.</p></li>
<li><span class="home-badge">3</span><p><strong>High-performance tracing daemons</strong> for capturing tracing information from many processes on a single machine into a unified trace file for offline analysis and visualization.</p></li>
<li><span class="home-badge">4</span><p><strong>Fully local, browser-based UI</strong> for visualizing large amounts of complex, interconnected data on a timeline and for exploring CPU and memory profiles with flamegraphs and call trees. Our UI works in all major browsers, doesn't require any installation, works offline, and can open traces and profiles recorded by other tools, such as pprof, Linux perf and simpleperf.</p></li>
<li><span class="home-badge">5</span><p><strong>Powerful, SQL-based analysis library</strong> for programmatically analyzing large amounts of complex, interconnected data on a timeline, even if it was not collected with Perfetto recording tooling.</p></li>
</ol>
</div>

## Why would you use Perfetto?

Perfetto was designed from the ground up to act as the default tracing system
for the Android OS and the Chrome Browser. As such, Perfetto is officially
supported for collecting, analysing and visualizing:

<div class="docs-home">
<div class="home-features">
<div class="home-feature">
<div class="home-feature-text">
<span class="home-icon material-icons-round" aria-hidden="true">timeline</span>
<h3>System traces on Android</h3>
<p>Debug and root-cause functional and performance issues in the Android platform and Android apps. Perfetto is suited for debugging e.g. slow startups, dropped frames (jank), animation glitches, low memory kills, App Not Responding (ANRs) and general buggy behaviour.</p>
<a class="home-more" href="/docs/getting-started/system-tracing">Record your first system trace</a>
</div>
<a class="home-shot" href="/docs/images/system-tracing-trace-view.png"><img src="/docs/images/home/android-system-trace.jpg" alt="An Android system trace in the Perfetto UI" loading="lazy"></a>
</div>
<div class="home-feature flip">
<div class="home-feature-text">
<span class="home-icon material-icons-round" aria-hidden="true">memory</span>
<h3>ART heap dumps and native heap profiles on Android</h3>
<p>Debug and root-cause high memory use in both Java/Kotlin code and C++ code respectively, in the Android platform and Android apps.</p>
<a class="home-more" href="/docs/getting-started/memory-profiling">Profile native memory on Android</a>
</div>
<a class="home-shot" href="/docs/images/native-heap-prof.png"><img src="/docs/images/home/native-heap-profile.jpg" alt="A native heap profile flamegraph in the Perfetto UI" loading="lazy"></a>
</div>
<div class="home-feature">
<div class="home-feature-text">
<span class="home-icon material-icons-round" aria-hidden="true">speed</span>
<h3>Callstack sampling profiles on Android</h3>
<p>Debug and root-cause high CPU usage by C++/Java/Kotlin code in the Android platform and Android apps.</p>
<a class="home-more" href="/docs/getting-started/cpu-profiling">Record CPU profiles and perf counters</a>
</div>
<a class="home-shot" href="/docs/images/perf-callstack-ui.png"><img src="/docs/images/home/cpu-profile.jpg" alt="CPU callstack samples and a flamegraph in the Perfetto UI" loading="lazy"></a>
</div>
<div class="home-feature flip">
<div class="home-feature-text">
<span class="home-icon material-icons-round" aria-hidden="true">language</span>
<h3>Chrome browser traces</h3>
<p>Debug and root cause issues in the browser, V8, Blink and, in advanced usecases, in websites themselves.</p>
<a class="home-more" href="/docs/getting-started/chrome-tracing">Record a Chrome trace</a>
</div>
<a class="home-shot" href="/docs/images/track-events.png"><img src="/docs/images/home/chrome-trace.jpg" alt="Chrome trace events in the Perfetto UI" loading="lazy"></a>
</div>
</div>
</div>

Beyond these "official" usecases, Perfetto consists a highly flexible set of
tools. This makes it capable of being used as a general purpose tracing system,
a performance data analyzer or a timeline visualizer. The Perfetto team
dedicates a portion of their time to supporting these cases, albeit at a
reduced level of support.

Other usecases Perfetto is commonly used for include:

<div class="docs-home">
<div class="home-uses">
<div class="home-use"><span class="home-icon material-icons-round role-import" aria-hidden="true">apps</span><div><strong>Collecting, analysing and visualizing in-app traces</strong><div class="home-use-text">Debug functional and performance issues in C/C++ and Rust apps and libraries on Windows, macOS and Linux-based embedded systems.</div></div></div>
<div class="home-use"><span class="home-icon material-icons-round role-import" aria-hidden="true">terminal</span><div><strong>Collecting, analysing and visualizing system traces on Linux</strong><div class="home-use-text">Debug kernel issues via ftrace, scheduling latency or IPCs between processes.</div></div></div>
<div class="home-use"><span class="home-icon material-icons-round role-import" aria-hidden="true">data_usage</span><div><strong>Collecting, analysing and visualizing heap profiles on Linux</strong><div class="home-use-text">Debug high memory usage of C/C++/Rust apps and libraries.</div></div></div>
<div class="home-use"><span class="home-icon material-icons-round role-import" aria-hidden="true">local_fire_department</span><div><strong>Viewing and analysing CPU profiles from other profilers</strong><div class="home-use-text">Such as pprof, Linux perf and simpleperf, to optimize CPU usage of apps and libraries in any language. See <a href="/docs/getting-started/viewing-cpu-profiles">Open a pprof, perf, simpleperf or samply profile</a>.</div></div></div>
<div class="home-use"><span class="home-icon material-icons-round role-import" aria-hidden="true">upload_file</span><div><strong>Analyze and visualize a wide range of profiling and tracing formats</strong><div class="home-use-text">Perfetto can open traces and profiles from various other tools, allowing you to use the Perfetto UI and its SQL-based query engine on many data sources, including:<ul class="home-chips"><li>pprof</li><li>Chrome JSON format</li><li>Firefox Profiler JSON format</li><li>Linux perf (binary and text formats)</li><li>Android simpleperf</li><li>Collapsed stacks (Brendan Gregg's FlameGraph format)</li><li>Linux ftrace text format</li><li>macOS Instruments</li><li>Fuchsia tracing format</li></ul></div></div></div>
<div class="home-use"><span class="home-icon material-icons-round role-import" aria-hidden="true">auto_graph</span><div><strong>Analysing and visualizing arbitrary "trace-like" data</strong><div class="home-use-text">The Perfetto analysis and visualization tools can be used on any "trace-like" data (e.g. data with a timestamp and some payload) as long as it can be converted to the Perfetto protobuf format; the possibilities are only limited by creativity!</div></div></div>
</div>
</div>

## Why would you **not** use Perfetto?

There are several types of problems Perfetto is either not designed for or is
explicitly unsupported.

<div class="docs-home">
<div class="home-rows">
<div class="home-row"><div class="home-row-title"><span class="home-icon material-icons-round role-no" aria-hidden="true">block</span><strong>Recording traces for distributed / server systems</strong></div><div class="home-row-body"><p>Perfetto is <strong>not</strong> a distributed tracer in the vein of OpenTelemetry, Jaeger, Datadog. Perfetto's recording tools are entirely for recording client side traces, especially at the system level. Our team believes that the space of distributed/server tracing is well covered by the aforementioned projects, unlike client side systems like Android and Linux/embedded systems.</p><p class="home-however"><span class="material-icons-round" aria-hidden="true">check_circle</span><span>However, the Perfetto UI <strong>can</strong> be used to visualize distributed traces if traces are converted to a format that Perfetto supports. In fact, this is commonly done inside Google.</span></p></div></div>
<div class="home-row"><div class="home-row-title"><span class="home-icon material-icons-round role-no" aria-hidden="true">block</span><strong>Recording system traces on Windows or macOS</strong></div><div class="home-row-body"><p>Perfetto's recording tools do <strong>not</strong> integrate with any system level data sources on Windows or macOS.</p><p class="home-however"><span class="material-icons-round" aria-hidden="true">check_circle</span><span>However, Perfetto <em>can</em> be used to analyse and visualize macOS traces collected with Instruments as we natively support the Instruments XML format.</span></p></div></div>
<div class="home-row"><div class="home-row-title"><span class="home-icon material-icons-round role-no" aria-hidden="true">block</span><strong>Consuming traces on the critical path</strong></div><div class="home-row-body"><p>Perfetto's producer code is optimized for low-overhead trace writing but the consumer side is <em>not</em> optimized for low-latency readback.</p><p>This means it is <em>not</em> advised to use Perfetto for situations where you want end-to-end low-latency tracing.</p></div></div>
<div class="home-row"><div class="home-row-title"><span class="home-icon material-icons-round role-no" aria-hidden="true">block</span><strong>Recording traces with the lowest overhead possible</strong></div><div class="home-row-body"><p>Perfetto SDK makes no claims on being the fastest possible way to record traces: we are well aware that there will be libraries and tools out there which can capture traces with less overhead. You can beat our tracing SDK by recording fixed-sized events in a shmem ring buffer bumping atomic pointers.</p><p>Instead, Perfetto's recording libraries and daemons focus on having a good trade-off between performance, flexibility and security of tracing.</p><p>For example, Perfetto supports arbitrary sized events (e.g. high res screenshots), coordinating multi-process tracing, concurrent tracing sessions with different configs, dynamic buffer multiplexing, arbitrarily nested key-value <strong>arguments</strong> attached to trace events, dynamic string interning, <strong>flows</strong> for linking trace events together and <strong>dynamic trace event names</strong> which many other low-overhead tracing systems do not support.</p><p class="home-however"><span class="material-icons-round" aria-hidden="true">check_circle</span><span>However, the Perfetto UI <em>can</em> be used to visualize traces recorded with non-Perfetto tools if those traces can be converted to the Perfetto protobuf format or some other format we support natively e.g. <em>Chrome JSON</em>, <em>Fuchsia</em> etc.</span></p></div></div>
<div class="home-row"><div class="home-row-title"><span class="home-icon material-icons-round role-no" aria-hidden="true">block</span><strong>Recording, analysing or visualizing GPU traces for games</strong></div><div class="home-row-body"><p>Tracing and profiling of games is very different world to tracing general purpose software for many reasons: the orientation of the whole system around "frames", the heavy focus on the GPU and its utilization, the presence of game engines and the need to integrate with them.</p><p>Due to Perfetto not having any specialized focus on the things game developers care heavily about, we feel like Perfetto is not well suited to this task.</p><p class="home-however"><span class="material-icons-round" aria-hidden="true">check_circle</span><span>We have some support for GPU render stages and GPU counters recording on Android, but these features are better supported by <a href="https://gpuinspector.dev">Android GPU Inspector</a> (which under the hoods uses Perfetto as one of its data sources).</span></p></div></div>
</div>
</div>

## How do I get started using Perfetto?

<div class="docs-home">
<div class="home-cards">
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">radio_button_checked</span>Record a trace</h3><p>Capture what the system was doing on Android, Linux or in Chrome.</p><ul><li><a href="/docs/getting-started/system-tracing">Record your first system trace</a></li><li><a href="/docs/how-to/choose-how-to-record">Choose how to record</a></li><li><a href="/docs/getting-started/chrome-tracing">Record a Chrome trace</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">memory</span>Profile CPU and memory</h3><p>Find where CPU time and memory go with callstack sampling, heapprofd and heap dumps.</p><ul><li><a href="/docs/how-to/choose-a-profiler">Choose a profiler</a></li><li><a href="/docs/getting-started/memory-profiling">Profile native memory on Android</a></li><li><a href="/docs/how-to/art-heap-dump">Take an ART heap dump</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">code</span>Instrument your code</h3><p>Add trace points to C, C++ and Rust apps, or to Android code with atrace.</p><ul><li><a href="/docs/getting-started/in-app-tracing">Instrument a C++ app</a></li><li><a href="/docs/how-to/choose-an-sdk">Choose an SDK: C, C++ or Rust</a></li><li><a href="/docs/getting-started/atrace">Instrument Android code with atrace</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-use" aria-hidden="true">travel_explore</span>Explore traces in the UI</h3><p>Move around the timeline, select events and explore heap dumps, all in your browser.</p><ul><li><a href="/docs/visualization/perfetto-ui">Tour the Perfetto UI</a></li><li><a href="/docs/visualization/large-traces">Open large traces</a></li><li><a href="/docs/visualization/heap-dump-explorer">Heap Dump Explorer</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-engine" aria-hidden="true">table_chart</span>Analyze with SQL</h3><p>Query traces with PerfettoSQL from the UI, the command line or Python.</p><ul><li><a href="/docs/analysis/perfetto-sql-getting-started">Query traces with PerfettoSQL</a></li><li><a href="/docs/getting-started/command-line-analysis">Work with traces from the command line</a></li><li><a href="/docs/analysis/trace-processor-python">Analyze traces from Python</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-import" aria-hidden="true">upload_file</span>Bring your own data</h3><p>Open profiles from pprof, perf and simpleperf, traces from other tools, or convert your own data.</p><ul><li><a href="/docs/getting-started/viewing-cpu-profiles">Open a pprof, perf, simpleperf or samply profile</a></li><li><a href="/docs/getting-started/other-formats">Supported trace and profile formats</a></li><li><a href="/docs/getting-started/converting">Convert your own data to a trace</a></li></ul></section>
</div>
<div class="home-strip"><span class="home-strip-label">Solve an Android problem</span><span class="home-strip-links"><a href="/docs/how-to/investigate-jank">Investigate jank</a><a href="/docs/how-to/investigate-startup">Investigate slow app startup</a><a href="/docs/case-studies/memory">Investigate memory use on Android</a><a href="/docs/how-to/investigate-blocked-thread">Investigate a blocked thread</a></span></div>
<div class="home-strip"><span class="home-strip-label">Perfetto for</span><span class="home-strip-links"><a href="/docs/start/android">Android developers</a><a href="/docs/start/linux">Linux developers</a><a href="/docs/start/cpp-rust">C, C++ and Rust developers</a><a href="/docs/start/chrome">Chromium developers</a><a href="/docs/start/performance">Performance engineers</a></span></div>
</div>

## {#who-uses-perfetto} Who uses Perfetto today?

Perfetto is the **default tracing system** for the **Android operating system**
and the **Chromium browser**. As such, Perfetto is utilized extensively by these
teams in Google, both to proactively identify performance improvements and
reactively to debug/root-cause issues locally, in the lab and even from the
field.

There are also many other teams in Google who use Perfetto in diverse ways. This
includes "non-traditional" uses of a tracing system. Perfetto has also been used
and adopted widely in the wider industry by many other companies.

The following is a non-exhaustive list of public mentions of Perfetto in blog
posts, articles and videos:

<div class="docs-home">
<div class="home-rows">
<div class="home-row"><div class="home-row-title"><span class="home-icon material-icons-round" aria-hidden="true">extension</span><strong>Projects built on Perfetto</strong></div><ul class="home-row-body home-media"><li><a href="https://blog.janestreet.com/magic-trace/">MagicTrace</a><span class="home-source"> (Jane Street)</span><p>A tool based on Perfetto to record and visualize Intel Processor traces.</p></li><li><a href="https://docs.mesa3d.org/perfetto.html">Mesa 3D</a><p>Embeds Perfetto in some of its drivers for GPU counter and render stage monitoring.</p></li></ul></div>
<div class="home-row"><div class="home-row-title"><span class="home-icon material-icons-round" aria-hidden="true">smart_display</span><strong>Talks and videos</strong></div><ul class="home-row-body home-media"><li><a href="https://www.droidcon.com/2022/06/28/client-tracing-at-scale/">Client Tracing at Scale</a><span class="home-source"> (Snap at droidcon)</span><p>"With the wide range of Android devices, it can be difficult to find the root cause of performance problems. By leveraging traces, we can begin to understand the exact circumstances that led to a poor user experience. We will discuss how we instrument our Snapchat app such that we can have the necessary signals for explainability. Additionally, we will describe how we incorporate tracing into our development process from local debugging, to performance tests and finally in production."</p></li><li><a href="https://www.youtube.com/watch?v=phhLFicMacY">Performance: Perfetto Traceviewer</a><span class="home-source"> (Android MAD Skills)</span><p>"On this episode of the MAD Skills series on Performance, Android Performance Engineer Carmen Jackson discusses the Perfetto traceviewer, an alternative to Android Studio for viewing system traces."</p></li><li><a href="https://m.facebook.com/RealityLabs/videos/performance-and-optimization-on-meta-quest-platform/488126049869673/">Performance and optimisation on the Meta Quest Platform</a><span class="home-source"> (Meta)</span></li><li><a href="https://youtu.be/yRlwOdCK7Ho?t=798">What's new in Dart and Flutter</a><span class="home-source"> (Google I/O 2023)</span></li><li><a href="https://youtu.be/Kp-aiSU8qCU?t=1092">Debugging Jetpack Compose</a><span class="home-source"> (Google I/O 2023)</span></li></ul></div>
<div class="home-row"><div class="home-row-title"><span class="home-icon material-icons-round" aria-hidden="true">article</span><strong>Articles</strong></div><ul class="home-row-body home-media"><li><a href="https://devblogs.microsoft.com/performance-diagnostics/perfetto-tooling-for-analyzing-android-linux-and-chromium-browser-performance-microsoft-performance-tools-linux-android/">Perfetto tooling for analyzing Android, Linux, and Chromium browser performance</a><span class="home-source"> (Microsoft)</span></li><li><a href="https://www.collabora.com/news-and-blog/blog/2021/04/22/profiling-virtualized-gpu-acceleration-with-perfetto/">Profiling virtualized GPU acceleration with Perfetto</a><span class="home-source"> (Collabora)</span></li><li><a href="https://dfamonteiro.com/posts/using-dotnet-trace-with-perfetto/">Diagnosing performance issues in .NET applications with dotnet-trace and Perfetto</a><span class="home-source"> (dfamonteiro.com)</span></li><li><a href="https://www.jviotti.com/2022/09/07/performance-testing-through-proportional-traces.html">Performance testing through proportional traces</a><span class="home-source"> (jviotti.com)</span></li></ul></div>
<div class="home-row"><div class="home-row-title"><span class="home-icon material-icons-round" aria-hidden="true">podcasts</span><strong>Podcasts</strong></div><ul class="home-row-body home-media"><li><a href="https://www.twoscomplement.org/podcast/performance.mp3">Performance</a><span class="home-source"> (Two's Complement podcast)</span><p>"Our most efficient podcast ever. Ben and Matt talk performance testing and optimization in fewer than 30 minutes."</p></li></ul></div>
</div>
</div>

## Where do I find more information and get help with Perfetto?

<div class="docs-home">
<div class="home-cards home-help">
<section class="home-card"><h3><span class="home-icon material-icons-round" aria-hidden="true">code</span>Source code</h3><p>For our source code and project home:</p><ul><li><a href="https://github.com/google/perfetto">GitHub</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round" aria-hidden="true">forum</span>Questions</h3><p>For Q/A:</p><ul><li><a href="https://github.com/google/perfetto/discussions/categories/q-a">GitHub Discussions</a></li><li><a href="https://groups.google.com/forum/#!forum/perfetto-dev">Public mailing list</a></li><li class="home-note"><strong>Googlers</strong>: use <a href="https://go/perfetto-yaqs">YAQS</a> or our <a href="http://go/perfetto-dev">internal mailing list</a>.</li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round" aria-hidden="true">bug_report</span>Bugs</h3><p>For bugs affecting any part of Perfetto <strong>except</strong> Chrome tracing:</p><ul><li><a href="https://github.com/google/perfetto/issues">GitHub issues</a></li><li class="home-note"><strong>Googlers</strong>: use the internal bug tracker <a href="http://goto.google.com/perfetto-bugs">go/perfetto-bugs</a>.</li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round" aria-hidden="true">language</span>Chrome tracing bugs</h3><p>For bugs affecting Chrome Tracing:</p><ul><li><a href="http://crbug.com">crbug.com</a></li><li class="home-note">Use <code>Component:Speed&gt;Tracing label:Perfetto</code>.</li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round" aria-hidden="true">chat</span>Chat</h3><p>For chatting directly with the Perfetto team:</p><ul><li><a href="https://discord.gg/35ShE3A">Discord</a></li><li class="home-note"><strong>Googlers</strong>: Thank you for contacting us. All our lines are currently busy. Your message is important to us, and an operator will get back to you as soon as possible. If your enquiry is truly urgent see <a href="http://go/perfetto-project">this page</a>.</li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round" aria-hidden="true">groups</span>Community</h3><p>Perfetto follows Google's Open Source Community Guidelines.</p><ul><li><a href="https://opensource.google/conduct/">Community Guidelines</a></li><li><a href="https://gugu-perf.github.io/perfetto-docs-zh-cn">🇨🇳 Community-maintained Chinese translation of the Perfetto docs</a></li></ul></section>
</div>
</div>

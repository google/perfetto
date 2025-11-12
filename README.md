# Perfetto - System profiling, app tracing and trace analysis

Perfetto is an open-source suite of SDKs, daemons and tools which use
**tracing** to help developers understand the behaviour of complex systems and
root-cause functional and performance issues on client and embedded systems.

It is a production-grade tool that is the default tracing system for the
**Android operating system** and the **Chromium browser**.

![](docs/images/perfetto-stack.svg)

## Core Components

Perfetto is not a single tool, but a collection of components that work
together:

- **High-performance tracing daemons:** For capturing tracing information from
  many processes on a single machine into a unified trace file.
- **Low-overhead tracing SDK:** A C++17 library for direct
  userspace-to-userspace tracing of timings and state changes in your
  application.
- **Extensive OS-level probes:** For capturing system-wide context on Android
  and Linux (e.g. scheduling states, CPU frequencies, memory profiling,
  callstack sampling).
- **Browser-based UI:** A powerful, fully local UI for visualizing and exploring
  large, multi-GB traces on a timeline. It works in all major browsers, requires
  no installation, and can open traces from other tools.
- **SQL-based analysis library:** A powerful engine that allows you to
  programmatically query traces using SQL to automate analysis and extract
  custom metrics.

## Why Use Perfetto?

Perfetto was designed to be a versatile and powerful tracing system for a wide
range of use cases.

- **For Android App & Platform Developers:** Debug and root-cause functional and
  performance issues like slow startups, dropped frames (jank), animation
  glitches, low memory kills, and ANRs. Profile both Java/Kotlin and native C++
  memory usage with heap dumps and profiles.
- **For C/C++ Developers (Linux, macOS, Windows):** Use the
  [Tracing SDK](docs/instrumentation/tracing-sdk.md) to instrument your
  application with custom trace points to understand its execution flow, find
  performance bottlenecks, and debug complex behavior. On Linux, you can also
  perform detailed CPU and native heap profiling.
- **For Linux Kernel & System Developers:** Get deep insights into kernel
  behavior. Perfetto acts as an efficient userspace daemon for `ftrace`,
  allowing you to visualize scheduling, syscalls, interrupts, and custom kernel
  tracepoints on a timeline.
- **For Chromium Developers:** Perfetto is the tracing backend for
  `chrome://tracing`. Use it to debug and root-cause issues in the browser, V8,
  and Blink.
- **For Performance Engineers & SREs:** Analyze and visualize a wide range of
  profiling and tracing formats, not just Perfetto's. Use the powerful SQL
  interface to programmatically analyze traces from tools like **Linux perf**,
  **macOS Instruments**, **Chrome JSON traces**, and more.

## Getting Started

We've designed our documentation to guide you to the right information as
quickly as possible, whether you're a newcomer to performance analysis or an
experienced developer.

1.  **New to tracing?** If you're unfamiliar with concepts like tracing and
    profiling, start here:

    - [**What is Tracing?**](https://perfetto.dev/docs/tracing-101) - A gentle
      introduction to the world of performance analysis.

2.  **Ready to dive in?** Our "Getting Started" guide is the main entry point
    for all users. It will help you find the right tutorials and documentation
    for your specific needs:

    - [**How do I start using Perfetto?**](https://perfetto.dev/docs/getting-started/start-using-perfetto) -
      Find your path based on your role and goals (e.g., Android App Developer,
      C/C++ Developer, etc.).

3.  **Want the full overview?** For a comprehensive look at what Perfetto is,
    why it's useful, and who uses it, see our main documentation page:
    - [**Perfetto Documentation Home**](https://perfetto.dev/docs/)

## Debian Distribution

For users interested in the Debian distribution of Perfetto, the official source
of truth and packaging efforts are maintained at
[Debian Perfetto Salsa Repository](https://salsa.debian.org/debian/perfetto)

## Community & Support

Have questions? Need help?

- **[GitHub Discussions](https://github.com/google/perfetto/discussions/categories/q-a):**
  For Q&A and general discussions.
- **[GitHub Issues](https://github.com/google/perfetto/issues):** For bug
  reports.
- **[Discord](https://discord.gg/35ShE3A):** For live chat with the community
  and developers.

We follow
[Google's Open Source Community Guidelines](https://opensource.google/conduct/).

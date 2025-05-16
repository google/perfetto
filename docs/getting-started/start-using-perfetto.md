# How do I start using Perfetto?

Perfetto is a large project and it can be daunting for someone new to undestand
what parts of the documentation relevant to them. By focusing on what technology
you are using and what you are trying to accomplish, this page will guide you
through the Perfetto documentation and help you solve problems with Perfetto as
quickly as possible.

NOTE: If you are unfamiliar with the word "tracing" or in general, are new to
the world of performance, we suggest reading the
[What is Tracing?](/docs/tracing-101.md) page first. If you are not quite sure
what Perfetto is and why it's useful, check out the
[What is Perfetto?](/docs/tracing-101.md) page first.

Our docs make use of the terms "Tutorials" and "Case Studies":

- **Tutorials** are guides which explain how to get started using Perfetto
  tools. They are focused on teaching you the tools themselves and _not_ so much
  on how the tools can be used to solve real world problems.
- **Case Studies** are detailed, opinionated guides which take you step-by-step
  how you can debug and root-cause a "vertical" problem using Perfetto. They
  focus more on helping you solve the problem and and less on teaching you
  Perfetto tools generally. They may also make signifcant use of non-Perfetto
  based tools or commands where appropriate.

Based on what technology you are interested in, please choose one of the
following sections to go next:

- [Android App/Platform developers](#android-app-platform-developer)
- [Chromium developers](#chromium-developers)
- [Linux kernel developers](#linux-kernel-developer)
- [C/C++ developers (non-Android)](#c-c-developer-non-android-)
- [Anyone with custom "trace-like" data to analyse/visualize](#anyone-with-quot-trace-like-quot-data-to-analyse-visualize)
- [Anyone not listed above](#anyone-not-listed-above)

## Android App/Platform Developer

Perfetto is the default tracing system on Android and is a powerful way to
analyse functional and performance issues on Android and, in general, to
understand the behaviour of the whole Android OS.

If you are a developer working on an Android app or on Android platform code
(i.e. the Android OS itsef), Perfetto can be used to answer all sorts of
questions:

1. Why is the runtime of an API take longer than I expect?
2. Why is my component buggy when it's called in this niche usecase?
3. How do I root-cause a complex bug affecting many processes/subsystems?
4. Why is my dependency it slow?
5. Why is my system using so much memory?
6. How can I reduce the CPU usage of my component?

**For app developers**, you might already be using Perfetto via one of the
app-focused tools which use Perfetto under the hood:

- Android Studio Profiler
- Macrobenchmark Libraries
- Android ProfileManager API

These are wrappers around Perfetto trace tooling to make it more well integrated
for app developers. However, these wrappers usually only expose one facet of
Perfetto or simplify it to make it easier for developers getting started.
Perfetto as a whole is signifcantly more flexible and powerful than any one of
these tools.

**For platform developers**, in Google, Perfetto is deeply integrated with
Android's [lab testing](http://go/crystalball) and
[field telemetry](http://go/perfetto-project) systems (links are for Googlers
only). Many OEMs also collect Perfetto traces in different scenarios: pelase
consult your internal company documentation for details on this.

### Tutorials

#### Collect, visualize and analyse traces

_System traces_.

Inside Google, system traces are used extensively by teams working on Android to
understand the behaviour of Android platform components and to debug and
root-cause functional and performance issues both inside the operating system or
with app's interaction with system. System traces are also used by Google (1P)
App teams who want to understand their own execution and how the system
interacts with them.

_Heap profiling_.

_CPU profiling_.

#### Add tracing annotations to code

For an app with no instrumentation, Perfetto traces will only contain tracing of
platform code not any of your app code. This is because Perfetto does not know
about the execution of your app: we need annotations to your app code to add
data to the trace. Think of this a bit like logging statements just with more
structure.

The `android.os.Trace` platform APIs allow for adding this annotation to your
code. This is explained by TODO.

#### Programatically query traces

Create Python scripts to automatically extract trace-based metrics from traces
collected locally, in the lab or from the field (e.g. through ProfileManager).

#### Analysis and visualization of non-Perfetto debugging data

- simpleperf
- logcat
- bugreports

### Case Studies

#### Debugging memory use

This guides explains how you might want to debug cases where your app is either
using a lot of memory or is allocating/freeing a lot of memory back to back
(causing jank, GCs etc). It walks you through understanding all the things which
make up "memory use" on Android.

## Chromium Developers

Perfetto is the default tracing system for the Chromium browser. The browser has
its own best-practicies for instrumenting and collecting traces on Chromium so
our documentation does not go into too much detail on this.

[This blog post](https://calendar.perfplanet.com/2023/digging-chrome-traces-introduction-example/)
might prove a good starting end-to-end starter guide of tracing on Chromium.

### Tutorials

#### Collecting traces

See [TODO](#) for how you can use the Perfetto UI to collect traces with Chrome
and consult

## Linux Kernel Developer

Perfetto has deep integration with the Linux kernel:

- A daemon to convert ->

### Tutorials

#### Collect, visualize and analyse traces

#### Addding ftrace tracepoints to kernel code

#### Programatically query traces

Create Python scripts to automatically extract trace-based metrics from traces
collected locally, in the lab or from the field.

## C/C++ Developer (non-Android)

### Tutorials

#### Collect, visualize and analyse in-app traces

#### Add tracing annotations to code

## Anyone with "trace-like" data to analyse/visualize

### Tutorials

#### Analyze widely adopted, non-Perfetto tracing formats

#### Converting custom traces to Perfetto

## Anyone not listed above

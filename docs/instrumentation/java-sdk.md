# Java / Android SDK

Perfetto has a Java tracing API for Android, in the `dev.perfetto.sdk` package.
Like the [Rust SDK](/docs/getting-started/rust-sdk.md), it is a language binding
over the [C SDK](/docs/instrumentation/choosing-an-sdk.md): the Java classes call
into `libperfetto_c` through JNI, so Java trace events use the same core and
produce the same trace format as every other Perfetto SDK.

NOTE: The Java SDK is currently **Android-internal**. `PerfettoTrace` is a hidden
(`@hide`) platform API, it depends on Android runtime features, and it is gated
behind the `enable_perfetto_android_java_sdk` build flag. It is not yet a
general-purpose, cross-platform Java library, and it inherits the C SDK's
[stability caveat](/docs/reference/c-sdk-api.md#stability).

## When to use it

For most Android app instrumentation, prefer the standard
[`android.os.Trace`](https://developer.android.com/reference/android/os/Trace)
API. It is fully supported by Perfetto (see
[ATrace instrumentation](/docs/data-sources/atrace.md)) and is the simplest path
for slices on the timeline.

Reach for the Perfetto Java SDK when you need richer track events than
`android.os.Trace` offers — typed debug annotations, counters, flows, custom and
nested tracks, correlation ids, or interned fields — from Java or Kotlin on
Android.

## Shape of the API

The entry point is `dev.perfetto.sdk.PerfettoTrace`. Events are built with a
fluent builder and committed with `emit()`:

```java
import dev.perfetto.sdk.PerfettoTrace;

// Register once (in-process or system backend).
PerfettoTrace.register(/* isBackendInProcess= */ false);

PerfettoTrace.Category category = new PerfettoTrace.Category("rendering");

PerfettoTrace.begin(category, "DrawGame")
    .addArg("player_number", 1)
    .emit();
// ...
PerfettoTrace.end(category).emit();
```

The builder mirrors the C SDK's track-event capabilities: `instant`, `begin` /
`end`, `counter`, and `state`; `addArg(...)` for debug annotations; `usingTrack`
/ `usingNamedTrack` / `usingCounterTrack` for custom tracks; `setFlow` /
`addFlow`; and `setCorrelationId`.

## Source

- Java API: `src/android_sdk/java/main/dev/perfetto/sdk/`
- JNI glue: `src/android_sdk/jni/`
- Native layer over the C SDK: `src/android_sdk/perfetto_sdk_for_jni/tracing_sdk.h`

## Next steps

- **[Choosing a Perfetto SDK](/docs/instrumentation/choosing-an-sdk.md)**: how the
  bindings relate.
- **[ATrace](/docs/data-sources/atrace.md)**: the standard Android tracing API.
- **[Track Events model](/docs/instrumentation/track-events.md)**: the concepts
  behind the events you emit.

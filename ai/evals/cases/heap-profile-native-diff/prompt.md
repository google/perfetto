---
name: "Native heap allocation profile diff: C++ malloc regression"
tags: [android, memory, heap-profile, native, diff, workflow]
runs: 3
files:
  - src: "{repo}/test/data/android_memory/native_alloc_profile_before.perfetto-trace"
    dst: native_alloc_before.perfetto-trace
  - src: "{repo}/test/data/android_memory/native_alloc_profile_after.perfetto-trace"
    dst: native_alloc_after.perfetto-trace
---
Compare the before and after Perfetto native heap allocation profile traces
(`libc.malloc`) captured during a UI action in `com.android.systemui.qs.panel`:
`./native_alloc_before.perfetto-trace` and `./native_alloc_after.perfetto-trace`.
How much did native allocation churn increase, and which C++/JNI function and
shared library caused the regression?

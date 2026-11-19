---
type: regex
flags: i
pattern: "Signal Catcher"
---
The two `Explicit concurrent mark compact GC` slices in
`com.google.android.gms.persistent` (393.2 ms and 349.3 ms, both top-level)
run on the `Signal Catcher` thread: ART collects on `SIGUSR1`, so they were
requested from outside the app rather than triggered by allocation pressure:
`SELECT thread_name, name, dur / 1e6 FROM thread_slice WHERE upid = 112 AND name GLOB 'Explicit*'`
(with `INCLUDE PERFETTO MODULE slices.with_context`).

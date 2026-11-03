---
type: regex
flags: i
pattern: "901([.,]\\d+)?\\s*ms|0[.,]90\\d*\\s*s|30,?265|30[.,][23]\\d*\\s*MB|29[.,][56]\\d*\\s*Mi?B"
---
The longest GC in `com.google.android.gms.persistent` (upid 112) is the
901.3 ms `Background concurrent mark compact GC` on `HeapTaskDaemon` (followed
by a 423.5 ms `CompactionPhase`), which brings `Heap size (KB)` from 66,549 KB
down to 30,265 KB (30.3 MB / 29.6 MiB). Answers that only look at the two
`Explicit` GCs miss it. Either its duration or its post-GC heap passes:
`SELECT thread_name, name, dur / 1e6 FROM thread_slice WHERE upid = 112 AND name GLOB '* GC' ORDER BY dur DESC`
(with `INCLUDE PERFETTO MODULE slices.with_context`).

---
type: regex
flags: i
pattern: "24,?693|24[.,][67]\\d*\\s*MB|24[.,]1\\d*\\s*Mi?B"
---
The lowest post-GC Java heap in `com.google.android.gms.persistent`
(upid 112), reached after the final `Explicit` GC, is 24,693 KB (24.1 MiB, or
24.7 "MB" when KB is divided by 1000); it is also the minimum of the track:
`SELECT MIN(c.value) FROM counter c JOIN process_counter_track t ON c.track_id = t.id WHERE t.upid = 112 AND t.name = 'Heap size (KB)'`.

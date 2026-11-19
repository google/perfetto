---
type: regex
flags: i
pattern: "37,?495|20,?563|36[.,]6\\d*\\s*Mi?B|37[.,]5\\d*\\s*MB|38[.,]4\\d*\\s*MB|20[.,][01]\\d*\\s*Mi?B|20[.,][56]\\d*\\s*MB|21[.,]0\\d*\\s*MB"
---
`Heap size (KB)` in `com.android.systemui` (upid 3) grows from 16,932 KB to
37,495 KB (+20,563 KB). The end value or the delta must be reported (the start
value alone does not show growth). 37,495 KB is 36.6 MiB, 37.5 "MB" when KB is
divided by 1000, or 38.4 MB in bytes; 20,563 KB is 20.1 MiB / 20.6 "MB" /
21.06 MB:
`SELECT MIN(c.value), MAX(c.value) FROM counter c JOIN process_counter_track t ON c.track_id = t.id WHERE t.upid = 3 AND t.name = 'Heap size (KB)'`.

---
type: regex
flags: i
pattern: "\\b112,?316,?416|\\b112(?:[.,]3\\d*)?\\s*MB|\\b107(?:[.,]1\\d*)?\\s*Mi?B"
---
`GPU Memory` in `com.android.systemui` (upid 3) peaks mid-trace at
112,316,416 B (112.3 MB / 107.1 MiB). Answers that only compare the first and
last samples miss it:
`SELECT CAST(MAX(c.value) AS INT) FROM counter c JOIN process_counter_track t ON c.track_id = t.id WHERE t.upid = 3 AND t.name = 'GPU Memory'`.

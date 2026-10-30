---
type: regex
flags: i
pattern: "66,?596,?864|66[.,][56]\\d*\\s*MB|63[.,]5\\d*\\s*Mi?B|54,?931,?456|54[.,]9\\d*\\s*MB|52[.,][34]\\d*\\s*Mi?B"
---
`GPU Memory` in `com.android.systemui` (upid 3) ends at 66,596,864 B
(66.6 MB / 63.5 MiB), +54,931,456 B (+54.9 MB / +52.4 MiB) over its first
sample of 11,665,408 B. Answers that only report min and max miss that it
does not return to the start level:
`SELECT value FROM counter c JOIN process_counter_track t ON c.track_id = t.id WHERE t.upid = 3 AND t.name = 'GPU Memory' ORDER BY c.ts DESC LIMIT 1`.

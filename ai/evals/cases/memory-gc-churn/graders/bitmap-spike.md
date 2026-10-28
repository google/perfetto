---
type: regex
flags: i
pattern: "bitmap[^\\n]{0,60}\\b141\\b|\\b141\\s*(?:bitmaps?|native bitmaps?)\\b|8,?954,?064|8[.,]9\\d*\\s*MB|8[.,]5[34]\\d*\\s*Mi?B"
---
`Bitmap Count` in `com.google.android.gms.persistent` (upid 112, pid 30647)
peaks at 141 (`Bitmap Memory` = 8,954,064 B = 8.95 MB / 8.54 MiB) between GCs
and returns to 0 by the end of the trace. The count must appear next to the
word "bitmap" so an unrelated "141" does not pass:
`SELECT t.name, MAX(c.value) FROM counter c JOIN process_counter_track t ON c.track_id = t.id WHERE t.upid = 112 AND t.name GLOB 'Bitmap *' GROUP BY t.name`.

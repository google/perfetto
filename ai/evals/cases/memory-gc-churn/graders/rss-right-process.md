---
type: regex
flags: i
pattern: "89,?829|89[.,][78]\\d*\\s*MB|85[.,][67]\\d*\\s*Mi?B|129,?523|129[.,]5\\d*\\s*MB|123[.,]5\\d*\\s*Mi?B|261,?132|261[.,]1\\d*\\s*MB|249[.,]0\\d*\\s*Mi?B"
---
RSS of `com.google.android.gms.persistent` pid 30647 (upid 112):
`mem.rss.anon` goes from 129,523,712 B (129.5 MB / 123.5 MiB) to 89,829,376 B
(89.8 MB / 85.7 MiB), and its single `mem.rss` sample is 261,132,288 B
(261.1 MB / 249.0 MiB). The trace also has a second
`com.google.android.gms.persistent` (pid 15864, upid 137: anonymous RSS
115.9 -> 112.7 MB, `mem.rss` 321.0 MB); selecting the process by name instead
of pid picks it up. Values truncated to KB (129,523 KB -> 89,829 KB) pass:
`SELECT t.name, c.value FROM counter c JOIN process_counter_track t ON c.track_id = t.id JOIN process p USING (upid) WHERE p.pid = 30647 AND t.name IN ('mem.rss', 'mem.rss.anon') ORDER BY t.name, c.ts`.

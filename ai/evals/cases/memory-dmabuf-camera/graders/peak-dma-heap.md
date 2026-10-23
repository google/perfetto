---
type: regex
flags: i
pattern: "1,?109,?766,?144|1,?109[.,]?\\d*\\s*MB|1,?110\\s*MB|1[.,]1[01]\\d*\\s*GB|1,?058[.,]?\\d*\\s*Mi?B|1[.,]0[34]\\d*\\s*GiB"
---
Device `mem.dma_heap` peaks at 1,109,766,144 B (1.11 GB / 1,109.8 MB /
1,058.4 MiB / 1.03 GiB):
`SELECT MAX(c.value) FROM counter c JOIN counter_track t ON c.track_id = t.id WHERE t.name = 'mem.dma_heap'`.

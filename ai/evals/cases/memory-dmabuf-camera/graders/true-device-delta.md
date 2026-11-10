---
type: regex
flags: i
pattern: "24,?207,?360|24[.,]2\\d*\\s*MB|23[.,][01]\\d*\\s*Mi?B"
---
Net device DMA-BUF delta across all 1,860 `mem.dma_heap_change` events in the
trace is +24,207,360 B (+24.21 MB / +23.09 MiB; `mem.dma_heap` first sample
316,366,848 B already includes the initial +18,255,872 B event from
298,110,976 B, and ends at 322,318,336 B):
`SELECT CAST(SUM(c.value) AS INT) FROM counter c JOIN counter_track t ON c.track_id = t.id WHERE t.type = 'android_dma_heap_change'`.

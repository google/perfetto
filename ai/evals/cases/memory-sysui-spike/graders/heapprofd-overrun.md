---
type: regex
flags: i
pattern: "heapprofd_buffer_overran|buffer[_ -]?overr[au]n|(?:heapprofd|shared[_ -]?mem|profile)[^.\\n]{0,60}(?:overr[au]n|truncat)"
---
`SELECT name, idx, severity, value FROM stats WHERE severity IN ('error', 'data_loss') AND value > 0`
shows `heapprofd_buffer_overran = 1` (idx 15865), truncating the embedded
`libc.malloc` heap profile.

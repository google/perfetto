---
type: regex
flags: i
pattern: "\\bgfx\\b|atrace[^\\n]{0,80}\\bview\\b|[\"`]view[\"`]|android\\.gpu\\.memory|gpu_mem_total|android_memory\\.pftxt"
---
The answer tells the user how to record the missing signals: atrace categories
`gfx` (bitmaps, gralloc) and `view` (HWUI, Skia), the `android.gpu.memory`
data source or `gpu_mem/gpu_mem_total` ftrace event (GPU memory), or the
`android_memory.pftxt` config that enables all of them. Ground truth:
`SELECT str_value FROM metadata WHERE name = 'trace_config_pbtxt'` lists only
`am` and `dalvik` atrace categories, and
`SELECT DISTINCT name FROM counter_track WHERE name GLOB '*GPU*' OR name GLOB '*HWUI*' OR name GLOB 'Bitmap*' OR name GLOB 'Skia*'`
returns no rows.

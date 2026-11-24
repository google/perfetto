---
type: llm
criteria: |
  The trace was recorded without the sources for these signals: its config
  enables only the `am` and `dalvik` atrace categories and no
  `android.gpu.memory` data source or `gpu_mem/gpu_mem_total` ftrace event, and
  no process in the trace has a `GPU Memory`, `*HWUI*`, `Skia *` or
  `Bitmap *` track. The answer must say clearly that GPU memory, HWUI caches
  and bitmaps were not recorded in this trace, so their growth cannot be
  determined, and must not report numbers for them or claim they stayed flat,
  were zero, or did not grow. Reporting what the trace does contain (for
  example `Heap size (KB)` from the `dalvik` category) is fine.
---
Honest "not recorded" answer for GPU, HWUI and bitmap memory, with no invented
numbers.

---
type: llm
criteria: |
  The answer gives a per-state breakdown of the Gmail main thread during
  the ~379 ms cold start that is close to: running ~241 ms, sleeping
  ~88 ms, runnable ~19-26 ms, uninterruptible ~4 ms; and it concludes
  that scheduling latency (runnable time) is a minor factor, under about
  10% of the startup, so the start is CPU-bound rather than starved of
  CPU. An answer that reports runnable time above ~40 ms, calls
  scheduling latency the main problem, or gives no per-state numbers,
  fails.
---
Correct breakdown and conclusion.

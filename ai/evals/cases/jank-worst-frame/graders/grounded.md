---
type: llm
criteria: |
  The answer reports, from the frame timeline data in the trace, that
  roughly 43 of Gmail's ~116 frames were janky, that the trace attributes
  most of them to "Buffer Stuffing" and the rest to "App Deadline Missed",
  and that the worst single frame was about 498 ms (App Deadline Missed).
  Numbers within ~10% are fine. An answer that gives no frame counts, no
  jank types from the trace, or a different worst-frame duration fails.
---
Grounded jank summary.

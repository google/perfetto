---
type: regex
pattern: "\\b4[0-5]\\b[^.\\n]{0,40}(jank|frames)|(jank|frames)[^.\\n]{0,40}\\b4[0-5]\\b"
---
Ground truth (actual_frame_timeline_slice for com.google.android.gm):
116 frames, 43 janky: 31 "Buffer Stuffing", 9 "App Deadline Missed",
3 both. Counting variants that land between 40 and 45 are accepted.

# Android Video Frames Data Source

## Overview

A Perfetto data source that captures screen frames as JPEG images and stores
them as `VideoFrame` trace packets with native BLOB storage.

This data source lives in the Android tree (`frameworks/base`), not in this
repo. The Perfetto-side changes (trace processor module, UI plugin, tests) are
in this repo.

## Test data

- Trace generator: `test/trace_processor/diff_tests/parser/android/generate_video_trace.py`
- Diff tests: `test/trace_processor/diff_tests/parser/android/tests.py`
  (see `test_video_frame_*`)

## Proto

`VideoFrame` is field 129 on `TracePacket`, defined in
`protos/perfetto/trace/android/video_frame.proto`.

Fields:
- `frame_number` (uint64): sequential frame number
- `jpg_image` (bytes): JPEG image data
- `track_name` (string): display name for the UI track
- `track_id` (uint32): groups frames into streams

## Trace config example

```proto
data_sources {
  config {
    name: "android.video_frames"
  }
}
```

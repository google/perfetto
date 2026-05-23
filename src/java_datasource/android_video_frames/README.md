# Android Video Frames Data Source

## Overview

A Perfetto data source that captures screen frames as JPEG images, emitted as
`TrackEvent` instants with a `VideoFrame` sub-message. JPEG data is stored in
dedicated blob storage (not base64 in args) for efficient access.

This data source lives in the Android tree (`frameworks/base`), not in this
repo. The Perfetto-side changes (trace processor, UI plugin, tests) are here.

## Proto

`VideoFrame` is defined in `protos/perfetto/trace/android/video_frame.proto`
and carried as `TrackEvent.video_frame` (field 57).

Fields:
- `frame_number` (uint64): sequential frame number
- `jpg_image` (bytes): JPEG image data

Track identity and display name come from standard `TrackDescriptor` machinery
— each stream uses its own `track_uuid` and `TrackDescriptor.name`.

## Emitting video frames

```proto
# One track descriptor per stream.
packet {
  trusted_packet_sequence_id: 1
  track_descriptor { uuid: 100  name: "Front Camera" }
}

# One TrackEvent per frame.
packet {
  timestamp: 1000000000
  trusted_packet_sequence_id: 1
  track_event {
    type: TYPE_INSTANT
    track_uuid: 100
    video_frame {
      frame_number: 0
      jpg_image: <jpeg bytes>
    }
  }
}
```

## Test data

- Trace generator: `test/trace_processor/diff_tests/parser/android/generate_video_trace.py`
- Diff tests: `test/trace_processor/diff_tests/parser/android/tests.py`
  (see `test_video_frame_*`)

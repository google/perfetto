// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';

// One slice per touch gesture on a display, per input device: from DOWN to
// the next UP (or CANCEL, if the gesture never lifts), at kernel event
// times, so it lines up with the video frames above. Reads the intrinsic
// table, like the video frames track, so the track needs no stdlib include
// at trace load.
export function createTouchesTrack(
  trace: Trace,
  uri: string,
  displayId: number,
) {
  const src = `
    WITH
      events AS (
        SELECT
          id,
          COALESCE(event_ts, ts) AS ts,
          device_id,
          action & 0xff AS action
        FROM __intrinsic_android_motion_events
        -- MotionEvent ACTION_DOWN, ACTION_UP, ACTION_CANCEL.
        WHERE display_id = ${displayId}
          AND (action & 0xff) IN (0, 1, 3)
          -- Skip events InputDispatcher derives or generates itself (split
          -- halves, OUTSIDE, CANCEL): their ids have the INPUT_DISPATCHER
          -- source (bits 30-31 = 1, see IdGenerator), as android.input's
          -- android_motion_event_pointers.is_synthesized does.
          AND ((event_id >> 30) & 3) != 1
      ),
      -- Number the gestures: each DOWN starts the next one on its device.
      numbered AS (
        SELECT
          *,
          SUM(action = 0) OVER (
            PARTITION BY device_id
            ORDER BY ts, id
            ROWS UNBOUNDED PRECEDING
          ) AS gesture
        FROM events
      ),
      gestures AS (
        SELECT
          MAX(IIF(action = 0, id, NULL)) AS id,
          MAX(IIF(action = 0, ts, NULL)) AS ts,
          COALESCE(
            MIN(IIF(action = 1, ts, NULL)),
            MIN(IIF(action = 3, ts, NULL))
          ) AS end_ts
        FROM numbered
        WHERE gesture > 0
        GROUP BY device_id, gesture
      )
    SELECT
      id,
      ts,
      COALESCE(end_ts - ts, 0) AS dur,
      0 AS depth,
      'Touch' AS name
    FROM gestures
  `;
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {id: NUM, ts: LONG, dur: LONG, name: STR, depth: NUM},
      src,
    }),
  });
}

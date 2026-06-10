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

import {materialColorScheme} from '../../components/colorizer';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {ProcessStateDetailsPanel} from './process_state_details_panel';

// A single timeline track: one slice per process_state snapshot, spanning from
// its capture time to the next (dur = -1, like the Screenshots / Video Frames
// tracks). Selecting a slice opens the ProcessStateDetailsPanel (compact graph
// + jump-to-full-explorer). The slice id IS the snapshot id, so selection maps
// straight onto android_process_state_snapshot.
export function createProcessStateTrack(trace: Trace, uri: string) {
  const src = `
    SELECT
      s.id AS id,
      s.ts AS ts,
      -1 AS dur,
      0 AS depth,
      (SELECT count(*) FROM android_process_state_process p
         WHERE p.snapshot_id = s.id)
        || ' procs · reason ' || s.oom_adj_reason AS name
    FROM android_process_state_snapshot s
  `;

  // One panel instance, reused across selections so mithril patches in place.
  const panel = new ProcessStateDetailsPanel(trace);

  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {id: NUM, ts: LONG, dur: LONG, name: STR, depth: NUM},
      src,
    }),
    detailsPanel: () => panel,
    colorizer: (row) => materialColorScheme(row.name),
  });
}

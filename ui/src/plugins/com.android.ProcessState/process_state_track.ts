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
import type {ProcessStateController} from './process_state_controller';

// A single timeline track: one slice per snapshot, spanning from its capture
// time to the next snapshot (the last extends to the trace end) so a slice is a
// selectable region of "this graph state". The slice name is the oom-adj reason
// (already resolved to a name by the importer; NULL for a one-shot dumpsys
// snapshot, shown as "SNAPSHOT"); the per-importance-tier process counts are the
// nested counter tracks under this one. Selecting a slice opens the
// ProcessStateDetailsPanel. The slice id IS the snapshot id, so selection maps
// straight onto _ps_snapshot.
export function createProcessStateTrack(
  trace: Trace,
  uri: string,
  controller: ProcessStateController,
) {
  // dur (the gap-free span to the next snapshot) and name (the oom-adj reason,
  // "SNAPSHOT" for a one-shot dumpsys capture) are precomputed and materialized
  // in _ps_snapshot, so this is a plain indexed read — no per-fetch window
  // function recompute. The per-importance-tier process counts are the nested
  // counter tracks, not this slice's name.
  const src = `SELECT id, ts, dur, 0 AS depth, name FROM _ps_snapshot`;

  // One panel instance, reused across selections so mithril patches in place.
  // It reads the shared controller so its selection matches the full page.
  const panel = new ProcessStateDetailsPanel(controller);

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

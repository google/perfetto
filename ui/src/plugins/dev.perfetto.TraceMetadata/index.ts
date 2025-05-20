// Copyright (C) 2024 The Android Open Source Project
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

import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TraceMetadata';
  static readonly dependencies = [StandardGroupsPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    const res = await trace.engine.query(`
      select count() as cnt from (select 1 from clock_snapshot limit 1)
    `);
    const row = res.firstRow({cnt: NUM});
    if (row.cnt === 0) {
      return;
    }
    const uri = `/clock_snapshots`;
    const title = 'Clock Snapshots';
    const track = new DatasetSliceTrack({
      trace,
      uri,
      dataset: new SourceDataset({
        src: `
          SELECT
            id,
            ts,
            'Snapshot' as name
          FROM clock_snapshot
        `,
        schema: {
          id: NUM,
          ts: LONG,
          name: STR,
        },
      }),
    });
    trace.tracks.registerTrack({
      uri,
      title,
      track,
    });
    const trackNode = new TrackNode({uri, title});
    const group = trace.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(trace.workspace, 'SYSTEM');
    group.addChildInOrder(trackNode);
  }
}

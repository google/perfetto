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

import {Time} from '../../base/time';
import {materialColorScheme} from '../../components/colorizer';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {PerfettoPlugin} from '../../public/plugin';
import {SMAPS_TRACK_KIND} from '../../public/track_kinds';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {computeInitialColumns, SmapsDetailsPanel} from './smaps_details_panel';

// One row per smaps snapshot (a group of rows from process_memory_mappings),
// keyed by {upid, ts}.
const SNAPSHOTS_TABLE = '_smaps_snapshots';

function trackUri(upid: number): string {
  return `/process_${upid}/smaps`;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Smaps';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  // Typically not all memory metrics are recorded in a trace, so we might not
  // show all of the columns. Decide on the set once, lazily when the details
  // panel is first opened.
  private initialColumns?: ReturnType<typeof computeInitialColumns>;

  async onTraceLoad(trace: Trace): Promise<void> {
    await createPerfettoTable({
      engine: trace.engine,
      name: SNAPSHOTS_TABLE,
      as: `
        SELECT MIN(id) AS id, upid, ts
        FROM process_memory_mappings
        GROUP BY upid, ts
      `,
    });

    const upids = await this.getUpids(trace);
    const groupsPlugin = trace.plugins.getPlugin(ProcessThreadGroupsPlugin);

    // Lazily compute the initial set of columns.
    const getInitialColumns = () => {
      if (this.initialColumns === undefined) {
        this.initialColumns = computeInitialColumns(trace.engine);
      }
      return this.initialColumns;
    };

    for (const upid of upids) {
      const group = groupsPlugin.getGroupForProcess(upid);
      if (!group) continue;

      const uri = trackUri(upid);
      const renderer = SliceTrack.create({
        trace,
        uri,
        dataset: new SourceDataset({
          src: SNAPSHOTS_TABLE,
          schema: {id: NUM, ts: LONG},
          filter: {col: 'upid', eq: upid},
        }),
        colorizer: () => materialColorScheme('smaps'), // single colour
        tooltip: () => 'Memory mapping snapshot',
        detailsPanel: (row) =>
          new SmapsDetailsPanel(
            trace,
            upid,
            Time.fromRaw(row.ts),
            getInitialColumns,
          ),
      });
      trace.tracks.registerTrack({
        uri,
        renderer,
        tags: {kinds: [SMAPS_TRACK_KIND], upid},
      });

      group.addChildInOrder(
        new TrackNode({
          uri,
          name: 'Memory mapping snapshots',
          sortOrder: -25,
        }),
      );
    }
  }

  private async getUpids(trace: Trace): Promise<ReadonlyArray<number>> {
    const result = await trace.engine.query(
      `SELECT DISTINCT upid FROM ${SNAPSHOTS_TABLE} ORDER BY upid`,
    );
    const upids: number[] = [];
    for (const it = result.iter({upid: NUM}); it.valid(); it.next()) {
      upids.push(it.upid);
    }
    return upids;
  }
}

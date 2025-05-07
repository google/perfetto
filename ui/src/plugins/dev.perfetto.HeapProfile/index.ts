// Copyright (C) 2021 The Android Open Source Project
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

import {HEAP_PROFILE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {NUM} from '../../trace_processor/query_result';
import {createHeapProfileTrack} from './heap_profile_track';
import {TrackNode} from '../../public/workspace';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {Track} from '../../public/track';

const EVENT_TABLE_NAME = 'heap_profile_events';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.HeapProfile';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  private readonly trackMap = new Map<number, Track>();

  async onTraceLoad(trace: Trace): Promise<void> {
    await this.createHeapProfileTable(trace);
    await this.addProcessTracks(trace);

    trace.onTraceReady.addListener(async () => {
      await this.selectFirstHeapProfile(trace);
    });
  }

  private async createHeapProfileTable(trace: Trace) {
    await createPerfettoTable(
      trace.engine,
      EVENT_TABLE_NAME,
      `
        SELECT
          MIN(id) as id,
          graph_sample_ts AS ts,
          upid,
          0 AS dur,
          0 AS depth,
          'graph' AS type
        FROM heap_graph_object
        GROUP BY graph_sample_ts, upid

        UNION ALL

        SELECT
          MIN(id) as id,
          ts,
          upid,
          0 AS dur,
          0 AS depth,
          'heap_profile:' || GROUP_CONCAT(DISTINCT heap_name) AS type
        FROM heap_profile_allocation
        GROUP BY ts, upid
      `,
    );
  }

  private async addProcessTracks(trace: Trace) {
    const trackGroupsPlugin = trace.plugins.getPlugin(
      ProcessThreadGroupsPlugin,
    );
    const incomplete = await this.getIncomplete(trace);
    const result = await trace.engine.query(`
      SELECT DISTINCT 
        upid
      FROM ${EVENT_TABLE_NAME}
    `);
    for (const it = result.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const uri = `/process_${upid}/heap_profile`;
      const title = 'Heap Profile';

      const track: Track = {
        uri,
        title,
        tags: {
          kind: HEAP_PROFILE_TRACK_KIND,
          upid,
        },
        track: createHeapProfileTrack(
          trace,
          uri,
          EVENT_TABLE_NAME,
          upid,
          incomplete,
        ),
      };

      trace.tracks.registerTrack(track);
      this.trackMap.set(upid, track);

      const group = trackGroupsPlugin.getGroupForProcess(upid);
      const trackNode = new TrackNode({uri, title, sortOrder: -30});
      group?.addChildInOrder(trackNode);
    }
  }

  private async getIncomplete(trace: Trace): Promise<boolean> {
    const it = await trace.engine.query(`
      SELECT value
      FROM stats
      WHERE name = 'heap_graph_non_finalized_graph'
    `);
    const incomplete = it.firstRow({value: NUM}).value > 0;
    return incomplete;
  }

  private async selectFirstHeapProfile(ctx: Trace) {
    // Select the first sample from each track
    const result = await ctx.engine.query(`
        SELECT
          id,
          upid
        FROM ${EVENT_TABLE_NAME}
        ORDER BY ts
        LIMIT 1
      `);

    const iter = result.maybeFirstRow({id: NUM, upid: NUM});
    if (!iter) return;

    const track = this.trackMap.get(iter.upid);
    if (!track) return;

    ctx.selection.selectTrackEvent(track.uri, iter.id);
  }
}

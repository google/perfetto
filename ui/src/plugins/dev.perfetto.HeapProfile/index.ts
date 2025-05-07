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
import {LONG, NUM} from '../../trace_processor/query_result';
import {createHeapProfileTrack} from './heap_profile_track';
import {TrackNode} from '../../public/workspace';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {Track} from '../../public/track';

function getUriForTrack(upid: number): string {
  return `/process_${upid}/heap_profile`;
}

interface TrackReference {
  readonly tableName: string;
  readonly track: Track;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.HeapProfile';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  private readonly tracks: TrackReference[] = [];

  async onTraceLoad(trace: Trace): Promise<void> {
    const incomplete = await this.getIncomplete(trace);
    const upids = await this.getUniqueUpids(trace);

    for (const upid of upids) {
      const uri = getUriForTrack(upid);
      const title = 'Heap Profile';
      const tableName = `_heap_profile_${upid}`;

      createPerfettoTable(
        trace.engine,
        tableName,
        `
          WITH
            heaps AS (
              SELECT
                group_concat(DISTINCT heap_name) AS h
              FROM heap_profile_allocation
              WHERE upid = ${upid}
            ),
            allocation_tses AS (
              SELECT DISTINCT
                ts
              FROM heap_profile_allocation
              WHERE upid = ${upid}
            ),
            graph_tses AS (
              SELECT DISTINCT
                graph_sample_ts
              FROM heap_graph_object
              WHERE upid = ${upid}
            )
          SELECT
            *,
            0 AS dur,
            0 AS depth
          FROM (
            SELECT
              (
                SELECT a.id
                FROM heap_profile_allocation a
                WHERE a.ts = t.ts
                ORDER BY a.id
                LIMIT 1
              ) AS id,
              ts,
              'heap_profile:' || (SELECT h FROM heaps) AS type
            FROM allocation_tses t
            UNION ALL
            SELECT
              (
                SELECT o.id
                FROM heap_graph_object o
                WHERE o.graph_sample_ts = g.graph_sample_ts
                ORDER BY o.id
                LIMIT 1
              ) AS id,
              graph_sample_ts AS ts,
              'graph' AS type
            FROM graph_tses g
          )
        `,
      );

      const track: Track = {
        uri,
        title,
        tags: {
          kind: HEAP_PROFILE_TRACK_KIND,
          upid,
        },
        track: createHeapProfileTrack(trace, uri, tableName, upid, incomplete),
      };

      this.tracks.push({tableName, track});
      trace.tracks.registerTrack(track);

      const group = trace.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForProcess(upid);
      const trackNode = new TrackNode({uri, title, sortOrder: -30});
      group?.addChildInOrder(trackNode);
    }

    trace.onTraceReady.addListener(async () => {
      await this.selectFirstHeapProfile(trace);
    });
  }

  private async getIncomplete(trace: Trace): Promise<boolean> {
    const it = await trace.engine.query(`
      SELECT value FROM stats
      WHERE name = 'heap_graph_non_finalized_graph'
    `);
    const incomplete = it.firstRow({value: NUM}).value > 0;
    return incomplete;
  }

  async getUniqueUpids(trace: Trace): Promise<number[]> {
    const result = await trace.engine.query(`
      SELECT DISTINCT upid FROM heap_profile_allocation
      UNION
      SELECT DISTINCT upid FROM heap_graph_object
    `);
    const upids: number[] = [];
    for (const it = result.iter({upid: NUM}); it.valid(); it.next()) {
      upids.push(it.upid);
    }
    return upids;
  }

  async selectFirstHeapProfile(ctx: Trace) {
    const samples: {trackUri: string; eventId: number; ts: bigint}[] = [];

    // Select the first sample from each track
    for (const {tableName, track} of this.tracks) {
      const result = await ctx.engine.query(`
        SELECT id, ts
        FROM ${tableName}
        ORDER BY ts
        LIMIT 1
      `);

      for (const it = result.iter({id: NUM, ts: LONG}); it.valid(); it.next()) {
        samples.push({
          trackUri: track.uri,
          eventId: it.id,
          ts: it.ts,
        });
      }
    }

    // Sort samples by timestamp and select the first one
    samples.sort((a, b) => Number(a.ts - b.ts));
    if (samples.length === 0) return;
    const firstSample = samples[0];
    ctx.selection.selectTrackEvent(firstSample.trackUri, firstSample.eventId);
  }
}

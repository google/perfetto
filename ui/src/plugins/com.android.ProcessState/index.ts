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

import './process_state.scss';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {CounterTrack} from '../../components/tracks/counter_track';
import {createProcessStateTrack} from './process_state_track';
import {BUCKETS} from './process_graph';
import {buildProcessState} from './relations';
import {
  ProcessStateController,
  SNAPSHOT_TRACK_URI,
} from './process_state_controller';

const PLUGIN_ID = 'com.android.ProcessState';

// UI for the ActivityManager process/service graph (the data behind `dumpsys
// activity`). A timeline track of snapshots; selecting one opens the explorer
// in the details panel — the relationship graph plus the process list and every
// binding/hosted table, with snapshot scrubbing and diff mode. The panel's
// layout is responsive (side-by-side when short, stacked when expanded), so the
// whole experience is self-contained with no separate page.
export default class implements PerfettoPlugin {
  static readonly id = PLUGIN_ID;

  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.maybeAddSnapshotExplorer(ctx);
  }

  private async maybeAddSnapshotExplorer(ctx: Trace): Promise<void> {
    if ((await buildProcessState(ctx.engine)) === 0) return;

    const controller = new ProcessStateController(ctx);

    // The snapshot slice track is the group's summary row (collapsed shows the
    // reason slices); expanding reveals one stepped counter per importance tier
    // counting the processes in that tier at each snapshot.
    const uri = SNAPSHOT_TRACK_URI;
    ctx.tracks.registerTrack({
      uri,
      renderer: createProcessStateTrack(ctx, uri, controller),
    });
    const group = new TrackNode({
      uri,
      name: 'Process state',
      sortOrder: -50,
      isSummary: true,
    });
    ctx.defaultWorkspace.addChildInOrder(group);

    // Pre-aggregate the per-tier process counts ONCE into a materialized table:
    // one row per snapshot with a column per importance tier. The nested counters
    // then read a tiny precomputed column instead of re-aggregating on render.
    // Aggregate the process table by snapshot_id FIRST, then join the small
    // (one-row-per-snapshot) result to the snapshot table for its ts. Joining the
    // two big intrinsic tables first and grouping after is catastrophic (~110s on
    // a 10M-row trace); group-first is a few seconds.
    const adj = 'COALESCE(oom_score, 999)'; // NULL oom_score => cached, as in the graph.
    const sumCols = BUCKETS.map((bucket, i) => {
      const lower = i === 0 ? undefined : `${adj} > ${BUCKETS[i - 1].maxAdj}`;
      const upper =
        bucket.maxAdj === Infinity ? undefined : `${adj} <= ${bucket.maxAdj}`;
      const range = [lower, upper].filter((c) => c !== undefined).join(' AND ');
      return `SUM(${range}) AS t${i}`;
    }).join(',\n          ');
    const outCols = BUCKETS.map((_, i) => `c.t${i}`).join(', ');
    await ctx.engine.query(`
      CREATE PERFETTO TABLE _ps_tier_count AS
      SELECT s.ts AS ts, ${outCols}
      FROM (
        SELECT snapshot_id,
          ${sumCols}
        FROM __intrinsic_android_process_state_process
        GROUP BY snapshot_id
      ) c
      JOIN __intrinsic_android_process_state_snapshot s ON s.id = c.snapshot_id
    `);

    BUCKETS.forEach((bucket, i) => {
      const tierUri = `${uri}/tier/${i}`;
      ctx.tracks.registerTrack({
        uri: tierUri,
        renderer: CounterTrack.create({
          trace: ctx,
          uri: tierUri,
          sqlSource: `SELECT ts, t${i} AS value FROM _ps_tier_count`,
        }),
      });
      group.addChildLast(new TrackNode({uri: tierUri, name: bucket.label}));
    });
  }
}

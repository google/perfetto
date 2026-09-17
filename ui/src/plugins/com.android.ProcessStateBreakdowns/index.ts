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

import {sqliteString} from '../../base/string_utils';
import {
  BreakdownTrackAggType,
  BreakdownTracks,
} from '../../components/tracks/breakdown_tracks';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {PERCEPTIBLE_STATE_SQL, PROCESS_STATE_SQL} from './sql';

/**
 * Visualizes framework process-state transitions (the `process_state_changed`
 * track events emitted by system_server) as one breakdown tree per proc
 * state, ordered by perceptibility: each state gets a concurrency counter
 * broken down package -> uid -> process, with the raw state intervals as
 * slices at the leaves.
 */
export default class implements PerfettoPlugin {
  static readonly id = 'com.android.ProcessStateBreakdowns';

  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query(PROCESS_STATE_SQL);

    // One row per state present in the trace, in perceptibility order.
    // States missing from the rank table (future enum additions) sort
    // between the real states and the UNKNOWN family.
    const states = await ctx.engine.query(`
      SELECT
        i.state AS state,
        IFNULL(r.rank, 500) AS rank,
        IFNULL(r.display_name, REPLACE(i.state, 'PROCESS_STATE_', ''))
          AS displayName
      FROM (
        SELECT DISTINCT state FROM _psb_process_state_intervals
        WHERE state IS NOT NULL
      ) i
      LEFT JOIN _psb_state_rank r USING (state)
      ORDER BY rank
    `);

    const stateRows: Array<{state: string; rank: number; displayName: string}> =
      [];
    for (
      const it = states.iter({state: STR, rank: NUM, displayName: STR});
      it.valid();
      it.next()
    ) {
      stateRows.push({
        state: it.state,
        rank: it.rank,
        displayName: it.displayName,
      });
    }
    // Trace has no process state events: don't add an empty group.
    if (stateRows.length === 0) return;

    // Deliberately NOT isSummary: a summary node's track content is omitted
    // while the group is expanded, but the perceptible-state timeline is the
    // headline content of this group and should stay visible in both states.
    const group = new TrackNode({name: 'Process States'});
    group.uri = await this.createPerceptibleStateTrack(ctx);

    await Promise.all(
      stateRows.map(async ({state, rank, displayName}) => {
        // Rank-keyed name: unique per state and safe as an identifier
        // (state strings come from trace data, so they never reach SQL
        // unquoted).
        const stateTable = `_psb_state_intervals_r${rank}`;
        await ctx.engine.query(`
          CREATE OR REPLACE PERFETTO VIEW ${stateTable} AS
          SELECT * FROM _psb_process_state_intervals
          WHERE state = ${sqliteString(state)}
        `);

        const breakdowns = new BreakdownTracks({
          trace: ctx,
          trackTitle: displayName,
          description:
            `Processes in ${state}, broken down by package, uid and ` +
            `process. The counters show how many matching processes are ` +
            `in this state over time.`,
          aggregationType: BreakdownTrackAggType.COUNT,
          aggregation: {
            columns: ['package_name', 'uid', 'process_name'],
            tableName: stateTable,
          },
          slice: {
            columns: [sqliteString(displayName)],
            tableName: stateTable,
          },
          sliceIdColumn: 'id',
          sortTracks: true,
        });

        const node = await breakdowns.createTracks();
        node.sortOrder = rank;
        group.addChildInOrder(node);
      }),
    );

    ctx.defaultWorkspace.addChildInOrder(group);
  }

  // The group's own track: at every instant, the most perceptible (best
  // ranked) state any tracked process is in, rendered as slices named after
  // the state. This is what shows when the group is collapsed, replacing the
  // "root counter" a plain BreakdownTracks tree would put there.
  private async createPerceptibleStateTrack(ctx: Trace): Promise<string> {
    await ctx.engine.query(PERCEPTIBLE_STATE_SQL);

    const uri = `/process_state_breakdowns_perceptible`;
    ctx.tracks.registerTrack({
      uri,
      description:
        'The most perceptible process state any tracked process is in, ' +
        'at every point in time.',
      renderer: SliceTrack.create({
        trace: ctx,
        uri,
        dataset: new SourceDataset({
          schema: {
            id: NUM,
            ts: LONG,
            dur: LONG,
            name: STR,
          },
          src: `
            SELECT s.id, s.ts, s.dur,
                   IFNULL(r.display_name, 'UNKNOWN') AS name
            FROM _psb_perceptible_slices s
            LEFT JOIN _psb_state_rank r USING (rank)
          `,
        }),
      }),
    });
    return uri;
  }
}

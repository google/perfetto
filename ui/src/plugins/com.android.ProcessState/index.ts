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

import {HSLColor} from '../../base/color';
import {createAggregationTab} from '../../components/aggregation_adapter';
import {
  getColorForSlice,
  GRAY,
  makeColorScheme,
} from '../../components/colorizer';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {sqliteString} from '../../base/string_utils';
import {
  ProcessStateResidencyAggregator,
  ProcessStateTransitionsAggregator,
  processTrackUri,
  TAG_COUNT_TRACK,
  TAG_PROCESS_TRACK,
} from './aggregators';

const PROCESS_STATE_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  upid: NUM,
  pid: NUM,
  uid: NUM_NULL,
  user_id: NUM_NULL,
  process_name: STR_NULL,
  package_name: STR_NULL,
  version_code: NUM_NULL,
  debuggable: NUM_NULL,
  state: STR,
  reason: STR_NULL,
} as const;

const SLATE = makeColorScheme(new HSLColor([210, 18, 48]));

/**
 * Visualizes Android framework process states over the lifetime of a trace.
 *
 * Everything lives under one dedicated top level group rather than under the
 * per-process groups, because in long memory or AOT traces the processes
 * carrying state data often have no other track data at all, and so get no
 * process group to attach to.
 */
export default class ProcessState implements PerfettoPlugin {
  static readonly id = 'com.android.ProcessState';
  static readonly description =
    'Visualizes Android framework process states: per-state concurrency ' +
    'counters and a per-process state timeline.';

  async onTraceLoad(ctx: Trace): Promise<void> {
    if (!(await this.hasProcessStateData(ctx))) {
      return;
    }

    await ctx.engine.query(`INCLUDE PERFETTO MODULE android.process_state;`);

    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessStateResidencyAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessStateTransitionsAggregator(ctx)),
    );

    const group = new TrackNode({
      name: 'Process states',
      isSummary: true,
    });
    group.addChildLast(await this.createConcurrencyTracks(ctx));
    group.addChildLast(await this.createProcessTracks(ctx));
    ctx.defaultWorkspace.addChildInOrder(group);
  }

  // The intrinsic tables only exist if the trace processor build has the
  // android_process_state plugin, and are empty unless the trace actually
  // carries the framework's process state events.
  private async hasProcessStateData(ctx: Trace): Promise<boolean> {
    const result = await ctx.engine.tryQuery(
      `SELECT count() AS cnt FROM __intrinsic_android_process_state`,
    );
    return result.ok && result.value.firstRow({cnt: NUM}).cnt > 0;
  }

  // One counter per proc state, showing how many processes were in that state
  // at any point in time. Ordered by declaration order in the proto.
  private async createConcurrencyTracks(ctx: Trace): Promise<TrackNode> {
    const summary = new TrackNode({
      name: 'By state',
      isSummary: true,
    });

    // States entered and left within the same timestamp never reach a
    // concurrency of 1, so they would only ever render a flat zero track.
    const states = await ctx.engine.query(`
      SELECT
        state,
        state_rank AS rank
      FROM _android_process_state_concurrency
      GROUP BY state
      HAVING max(concurrency) > 0
      ORDER BY rank, state
    `);

    for (const it = states.iter({state: STR}); it.valid(); it.next()) {
      const {state} = it;
      const uri = `${ProcessState.id}#count.${state}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: CounterTrack.create({
          trace: ctx,
          uri,
          sqlSource: `
            SELECT ts, concurrency AS value
            FROM _android_process_state_concurrency
            WHERE state = ${sqliteString(state)}
          `,
        }),
        tags: {type: TAG_COUNT_TRACK, state},
        description: `Number of processes concurrently in ${state}.`,
      });
      summary.addChildLast(new TrackNode({uri, name: state}));
    }

    return summary;
  }

  // One timeline per process. Ordered by uid so that the successive processes
  // of a respawning app sit next to each other - that adjacency is the only
  // thing tying an app's lifetimes together, since each restart gets its own
  // upid.
  private async createProcessTracks(ctx: Trace): Promise<TrackNode> {
    const byProcess = new TrackNode({name: 'By process', isSummary: true});

    const processes = await ctx.engine.query(`
      SELECT
        upid,
        pid,
        process_name AS name
      FROM _android_process_state_intervals
      GROUP BY upid
      ORDER BY
        uid NULLS LAST,
        min(ts),
        pid
    `);

    for (
      const it = processes.iter({upid: NUM, pid: NUM, name: STR_NULL});
      it.valid();
      it.next()
    ) {
      const {upid, pid} = it;
      const name = it.name ?? `${pid}`;
      const uri = processTrackUri(upid);
      ctx.tracks.registerTrack({
        uri,
        renderer: SliceTrack.create({
          trace: ctx,
          uri,
          dataset: new SourceDataset({
            schema: PROCESS_STATE_SCHEMA,
            src: '_android_process_state_intervals',
            filter: {col: 'upid', eq: upid},
          }),
          // These tracks come in their hundreds, so keep them compact.
          sliceLayout: {sliceHeight: 12, titleSizePx: 10},
          sliceName: (row) => row.state,
          colorizer: (row) => {
            if (row.state === 'NONEXISTENT') return SLATE;
            if (row.state === 'EXITED') return GRAY;
            return getColorForSlice(row.state);
          },
          rootTableName: '_android_process_state_intervals',
        }),
        tags: {type: TAG_PROCESS_TRACK, upid},
        description: `Framework process state of ${name} (${pid}) over time.`,
      });
      byProcess.addChildLast(new TrackNode({uri, name: `${name} ${pid}`}));
    }

    return byProcess;
  }
}

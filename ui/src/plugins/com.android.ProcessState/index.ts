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
import {sqliteString} from '../../base/string_utils';
import {createAggregationTab} from '../../components/aggregation_adapter';
import {
  getColorForSlice,
  GRAY,
  makeColorScheme,
} from '../../components/colorizer';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {
  ProcessExitsAggregator,
  ProcessStartsAggregator,
  ProcessStateResidencyAggregator,
  ProcessStateTransitionsAggregator,
  processTrackUri,
  TAG_COUNT_TRACK,
  TAG_PROCESS_TRACK,
} from './aggregators';
import {ProcessStateDetailsPanel} from './details_panel';
import {ProcessStateCountTrack} from './state_count_track';

const BASE_SCHEMA = {
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
  state_change_reason: STR_NULL,
} as const;

const NONEXISTENT_SCHEMA = {
  ...BASE_SCHEMA,
  hosting_type: STR_NULL,
  hosting_name: STR_NULL,
  trigger_type: STR_NULL,
  bind_application_delay_ms: LONG_NULL,
  process_start_delay_ms: LONG_NULL,
} as const;

const EXITED_SCHEMA = {
  ...BASE_SCHEMA,
  exit_reason: STR_NULL,
  exit_subreason: STR_NULL,
} as const;

const PROCESS_STATE_SCHEMA = {...NONEXISTENT_SCHEMA, ...EXITED_SCHEMA} as const;

const SLATE = makeColorScheme(new HSLColor([210, 18, 48]));

// Below every other process-scoped track (-50..30) so it appears first.
const PROCESS_GROUP_SORT_ORDER = -100;

/**
 * Visualizes Android framework process states over the lifetime of a trace.
 *
 * A process's state timeline lives in its process group. It is also mirrored
 * under one dedicated top level group, which in long memory traces is the only
 * place most of them can appear: those processes carry no other track data,
 * and so get no process group at all.
 */
export default class ProcessState implements PerfettoPlugin {
  static readonly id = 'com.android.ProcessState';
  static readonly description =
    'Visualizes Android framework process states: per-state concurrency ' +
    'counters and a per-process state timeline.';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

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
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessStartsAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessExitsAggregator(ctx)),
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
  // at any point in time, ordered by declaration order in the proto, then one
  // per freezer state.
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
        renderer: new ProcessStateCountTrack(ctx, uri, {
          label: `in ${state}`,
          concurrency: `
            SELECT * FROM _android_process_state_concurrency
            WHERE state = ${sqliteString(state)}
          `,
          members: () => `state = ${sqliteString(state)}`,
        }),
        tags: {type: TAG_COUNT_TRACK, state},
        description: `Number of processes concurrently in ${state}.`,
      });
      summary.addChildLast(new TrackNode({uri, name: state}));
    }

    // Only long memory traces carry freezer events.
    const freezerStates = await ctx.engine.query(`
      SELECT state
      FROM _android_process_freezer_concurrency
      GROUP BY state
      HAVING max(concurrency) > 0
      ORDER BY state
    `);

    for (const it = freezerStates.iter({state: STR}); it.valid(); it.next()) {
      const {state} = it;
      const uri = `${ProcessState.id}#count.${state}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: new ProcessStateCountTrack(ctx, uri, {
          label: state.toLowerCase(),
          concurrency: `
            SELECT * FROM _android_process_freezer_concurrency
            WHERE state = ${sqliteString(state)}
          `,
          members: (ts) => `
            upid IN (
              SELECT upid FROM _android_process_freezer_intervals
              WHERE state = ${sqliteString(state)}
                AND ts <= ${ts} AND ts + dur > ${ts}
            )
          `,
        }),
        description: `Number of processes concurrently ${state.toLowerCase()}.`,
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
    const groups = ctx.plugins.getPlugin(ProcessThreadGroupsPlugin);

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
      const processGroup = groups.getGroupForProcess(upid);
      // The process group owns the slices when it exists; the overview row
      // then mirrors it and links to it.
      const overviewUri = processGroup !== undefined ? `${uri}.overview` : uri;
      for (const trackUri of processGroup ? [uri, overviewUri] : [uri]) {
        const isOwner = trackUri === uri;
        ctx.tracks.registerTrack({
          uri: trackUri,
          renderer: SliceTrack.create({
            trace: ctx,
            uri: trackUri,
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
            detailsPanel: (row) => {
              const schema =
                row.state === 'NONEXISTENT'
                  ? NONEXISTENT_SCHEMA
                  : row.state === 'EXITED'
                    ? EXITED_SCHEMA
                    : BASE_SCHEMA;
              return new ProcessStateDetailsPanel(
                ctx,
                schema,
                row,
                isOwner ? undefined : uri,
              );
            },
            rootTableName: isOwner
              ? '_android_process_state_intervals'
              : undefined,
          }),
          tags: {type: TAG_PROCESS_TRACK, upid},
          description: `Framework process state of ${name} (${pid}) over time.`,
        });
      }
      byProcess.addChildLast(
        new TrackNode({uri: overviewUri, name: `${name} ${pid}`}),
      );
      processGroup?.addChildInOrder(
        new TrackNode({
          uri,
          name: 'Process state',
          sortOrder: PROCESS_GROUP_SORT_ORDER,
        }),
      );
    }

    return byProcess;
  }
}

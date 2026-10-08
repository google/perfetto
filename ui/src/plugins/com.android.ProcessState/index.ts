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
import {HSLColor} from '../../base/color';
import {createAggregationTab} from '../../components/aggregation_adapter';
import {
  getColorForSlice,
  GRAY,
  makeColorScheme,
  materialColorScheme,
} from '../../components/colorizer';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SliceTrackDetailsPanel} from '../../components/tracks/slice_track_details_panel';
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
import {sqliteString} from '../../base/string_utils';
import {
  PassDetailsPanel,
  ProcessStateResidencyAggregator,
  ProcessStateTransitionsAggregator,
  processTrackUri,
  TAG_COUNT_TRACK,
  TAG_PROCESS_TRACK,
  TAG_REASON_TRACK,
} from './aggregators';
import {ProcessStateController} from './process_state_controller';
import {ProcessStateDetailsPanel} from './process_state_details_panel';
import {buildProcessState} from './relations';

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
  prev_state: STR_NULL,
  oom_score: NUM_NULL,
  prev_oom_score: NUM_NULL,
  capability_flags: NUM_NULL,
  reason: STR_NULL,
  seq_id: NUM_NULL,
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

const PASS_SCHEMA = {
  id: NUM,
  ts: LONG,
  seq_id: NUM,
  reason: STR,
} as const;

const SLATE = makeColorScheme(new HSLColor([210, 18, 48]));

/**
 * Visualizes Android framework process states and the ActivityManager binding
 * graph over the lifetime of a trace.
 *
 * Everything lives under one dedicated top level group rather than under the
 * per-process groups, because in long memory or AOT traces the processes
 * carrying state data often have no other track data at all, and so get no
 * process group to attach to.
 */
export default class ProcessState implements PerfettoPlugin {
  static readonly id = 'com.android.ProcessState';
  static readonly description =
    'Visualizes Android framework process states, OomAdjuster passes, ' +
    'service/provider binding graphs, and causal trigger events.';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const hasStateIntervals = await this.hasProcessStateData(ctx);
    const snapshotCount = await buildProcessState(ctx.engine);
    if (!hasStateIntervals && snapshotCount === 0) {
      return;
    }

    const controller =
      snapshotCount > 0 ? new ProcessStateController(ctx) : undefined;

    const group = new TrackNode({
      name: 'Process states',
      isSummary: true,
    });

    if (hasStateIntervals) {
      await ctx.engine.query(`INCLUDE PERFETTO MODULE android.process_state;`);

      ctx.selection.registerAreaSelectionTab(
        createAggregationTab(ctx, new ProcessStateResidencyAggregator(ctx)),
      );
      ctx.selection.registerAreaSelectionTab(
        createAggregationTab(ctx, new ProcessStateTransitionsAggregator(ctx)),
      );

      group.addChildLast(await this.createConcurrencyTracks(ctx));
      group.addChildLast(await this.createProcessTracks(ctx, controller));
    }

    group.addChildLast(
      await this.createReasonTracks(ctx, controller, hasStateIntervals),
    );

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

  // One track per OomAdjuster reason, plus a root track with all passes
  // combined. Each instant represents a single OomAdjuster pass (seq_id).
  private async createReasonTracks(
    ctx: Trace,
    controller?: ProcessStateController,
    hasStateIntervals = true,
  ): Promise<TrackNode> {
    const byReason = new TrackNode({
      uri: `${ProcessState.id}#reason`,
      name: 'By reason',
      isSummary: true,
    });

    const passesSql = hasStateIntervals
      ? `
        SELECT
          seq_id AS id,
          min(ts) AS ts,
          seq_id,
          replace(reason, 'OOM_ADJ_REASON_', '') AS reason
        FROM _android_process_state_intervals
        WHERE seq_id IS NOT NULL
        GROUP BY seq_id
      `
      : `
        SELECT
          coalesce(seq_id, id) AS id,
          ts,
          coalesce(seq_id, id) AS seq_id,
          coalesce(replace(reason, 'OOM_ADJ_REASON_', ''), 'SNAPSHOT') AS reason
        FROM _ps_snapshot
      `;

    const makeTrack = (uri: string, reason?: string) =>
      SliceTrack.create({
        trace: ctx,
        uri,
        dataset: new SourceDataset({
          schema: PASS_SCHEMA,
          src: passesSql,
          filter:
            reason !== undefined ? {col: 'reason', eq: reason} : undefined,
        }),
        sliceName: (row) => row.reason,
        colorizer: (row) => materialColorScheme(row.reason),
        detailsPanel: (row) =>
          controller !== undefined
            ? new ProcessStateDetailsPanel(controller, undefined, row)
            : new PassDetailsPanel(ctx, row),
      });

    ctx.tracks.registerTrack({
      uri: `${ProcessState.id}#reason`,
      renderer: makeTrack(`${ProcessState.id}#reason`),
      description: 'All OomAdjuster passes.',
    });

    const reasons = await ctx.engine.query(`
      SELECT DISTINCT reason
      FROM (${passesSql})
      WHERE reason IS NOT NULL
      ORDER BY reason
    `);

    for (const it = reasons.iter({reason: STR}); it.valid(); it.next()) {
      const {reason} = it;
      const uri = `${ProcessState.id}#reason.${reason}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: makeTrack(uri, reason),
        tags: {type: TAG_REASON_TRACK, reason},
        description: `OomAdjuster passes triggered by ${reason}.`,
      });
      byReason.addChildLast(new TrackNode({uri, name: reason}));
    }

    return byReason;
  }

  // One timeline per process. Ordered by uid so that the successive processes
  // of a respawning app sit next to each other - that adjacency is the only
  // thing tying an app's lifetimes together, since each restart gets its own
  // upid.
  private async createProcessTracks(
    ctx: Trace,
    controller?: ProcessStateController,
  ): Promise<TrackNode> {
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
          detailsPanel: (row) => {
            if (controller !== undefined) {
              return new ProcessStateDetailsPanel(controller, {
                id: row.id,
                ts: row.ts,
                dur: row.dur,
                upid: row.upid,
                pid: row.pid,
                uid: row.uid,
                processName: row.process_name,
                packageName: row.package_name,
                state: row.state,
                prevState: row.prev_state,
                oomScore: row.oom_score,
                prevOomScore: row.prev_oom_score,
                capabilityFlags: row.capability_flags,
                reason: row.reason,
                seqId: row.seq_id,
                hostingType: row.hosting_type,
                hostingName: row.hosting_name,
                triggerType: row.trigger_type,
                bindApplicationDelayMs: row.bind_application_delay_ms,
                processStartDelayMs: row.process_start_delay_ms,
                exitReason: row.exit_reason,
                exitSubreason: row.exit_subreason,
              });
            }
            const schema =
              row.state === 'NONEXISTENT'
                ? NONEXISTENT_SCHEMA
                : row.state === 'EXITED'
                  ? EXITED_SCHEMA
                  : BASE_SCHEMA;
            return new SliceTrackDetailsPanel(
              ctx,
              new SourceDataset({
                schema,
                src: '_android_process_state_intervals',
                filter: {col: 'upid', eq: upid},
              }),
              row,
            );
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

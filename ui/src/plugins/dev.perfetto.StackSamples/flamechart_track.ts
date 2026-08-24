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

import m from 'mithril';
import {valueIfAllEqual} from '../../base/array_utils';
import {RECT_PATTERN_SAMPLED} from '../../base/renderer';
import {Icons} from '../../base/semantic_icons';
import {Time, type time} from '../../base/time';
import type {TimeScale} from '../../base/time_scale';
import {GRAY} from '../../components/colorizer';
import {formatDuration} from '../../components/time_utils';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {
  TrackEventDetails,
  TrackEventSelection,
} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {
  SnapPoint,
  Track,
  TrackMouseEvent,
  TrackRenderContext,
  TrackRenderer,
  TrackSetting,
  TrackSettingDescriptor,
} from '../../public/track';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {sqlValueToSqliteString} from '../../trace_processor/sql_utils';
import type {FlamegraphState} from '../../widgets/flamegraph';
import {MenuItem} from '../../widgets/menu';
import {Button} from '../../widgets/button';
import {FlamechartFrameDetailsPanel} from './frame_details_panel';
import {
  createProfilingDetailsPanel,
  DIAMOND_INSTANT_STYLE,
} from './profiling_track';
import {sampleCategorySqlExpr, sampleColorScheme} from './sample_colors';
import {
  STACK_SAMPLE_FLAMECHART_TRACK_KIND,
  STACK_SAMPLE_TRACK_KIND,
} from './track_kinds';

// How many stack frames the track shows under the marker row: none (just a
// "…" band advertising that frames exist), a 4-frame peek, or everything.
// Clicking the "…" cut band jumps straight to all frames; the peek is only
// reachable as a per-thread default or through the setting. Defaults are
// picked per thread: threads which also have instrumented slice tracks
// start at 'none', others at 4.
export type FlamechartView = 0 | 4 | 'full';

export const PEEK_VIEW: FlamechartView = 4;
export const MARKERS_ONLY_VIEW: FlamechartView = 0;

// Track height reported before the lazy init has produced the real track.
const PLACEHOLDER_HEIGHT_PX = 20;

// Nominal duration given to zero-duration runs (stacks first seen at the
// very last sample) so they render as slices rather than instants.
const ZERO_DUR_RUN_NS = 1;

export interface FlamechartTrackConfig {
  readonly source: string;
  readonly utid: number;
  readonly upid: number | undefined;
  readonly processName: string | undefined;
  // Undefined means all sessions; null means samples without a session.
  readonly sessionId?: number | null;
}

const ROW_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  depth: NUM,
  name: STR,
  sampleCount: NUM,
  category: NUM,
  frameId: NUM,
  capped: NUM,
};

type FlamechartRow = {
  id: number;
  ts: bigint;
  dur: bigint;
  depth: number;
  name: string;
  sampleCount: number;
  category: number;
  frameId: number;
  capped: number;
};

function viewLabel(view: FlamechartView | undefined): string {
  if (view === undefined) return 'mixed';
  if (view === 'full') return 'All frames';
  if (view === 0) return 'No frames';
  return `${view} frames`;
}

const viewDescriptor: TrackSettingDescriptor<FlamechartView> = {
  name: 'Stack frames',
  description:
    'How many stack frames to render under the sample markers; hidden ' +
    'frames sit behind a clickable "…" row.',
  render(setter, values) {
    const value = valueIfAllEqual(values);
    const option = (view: FlamechartView) =>
      m(MenuItem, {
        label: viewLabel(view),
        onclick: () => setter(view),
        icon: value === view ? Icons.RadioChecked : Icons.RadioUnchecked,
      });
    return m(
      MenuItem,
      {label: `Stack frames (currently: ${viewLabel(value)})`},
      option(0),
      option(4),
      option('full'),
    );
  },
};

// Renders a thread's sampled stacks as a flame chart over time, replacing
// the per-thread callstack samples ("instants") track. Row 0 holds one
// diamond marker per sample (click to see its callstack); the rows below
// hold the flame chart: one row per stack depth, with segments spanning the
// time ranges over which a frame was continuously present. Segments are
// approximate by construction; frames are colored by origin
// (program/library/kernel) and carry a dot stipple to make that legible. A
// View setting switches back to the plain callstacks (markers-only) view.
//
// All expensive work is deferred to the first render of the track: the
// required modules (including the callstack forest materialization) are only
// included and the runs table only computed once the track actually becomes
// visible. Until then the track renders empty.
//
// Runs are computed over the inline-expanded callstack forest
// (_callstack_spc_forest), so each sample's stack includes inlined frames.
// Zero-duration runs (stacks first seen at the very last sample) are given a
// nominal 1ns duration so they render as slices rather than instants; the
// only dur=0 rows are the sample markers.
export interface FlamechartTrackHandle {
  setDefaultView(view: FlamechartView): void;
}

class FlamechartTrackRenderer implements TrackRenderer, FlamechartTrackHandle {
  private inner?: TrackRenderer;
  private initStarted = false;
  private view: FlamechartView = PEEK_VIEW;
  private viewSetByUser = false;
  private readonly tableName: string;

  constructor(
    private readonly trace: Trace,
    private readonly uri: string,
    private readonly config: FlamechartTrackConfig,
    private readonly detailsPanelState: FlamegraphState | undefined,
    private readonly onDetailsPanelStateChange: (
      state: FlamegraphState,
    ) => void,
  ) {
    const slug = config.source.replace(/[^a-zA-Z0-9]/g, '_');
    const session =
      config.sessionId === undefined
        ? ''
        : config.sessionId === null
          ? '_snone'
          : `_s${config.sessionId}`;
    this.tableName = `__flamechart_runs_${slug}_${config.utid}${session}`;
  }

  get settings(): ReadonlyArray<TrackSetting> {
    const setting = <T>(x: TrackSetting<T>) => x;
    return [
      setting({
        descriptor: viewDescriptor,
        value: this.view,
        update: (value) => {
          this.view = value;
          this.viewSetByUser = true;
        },
      }),
    ];
  }

  setDefaultView(view: FlamechartView): void {
    if (!this.viewSetByUser) {
      this.view = view;
    }
  }

  render(ctx: TrackRenderContext): void {
    if (this.inner === undefined) {
      this.ensureInit();
      return;
    }
    this.inner.render(ctx);
  }

  getSliceVerticalBounds(depth: number) {
    return this.inner?.getSliceVerticalBounds?.(depth);
  }

  getHeight(): number {
    return this.inner?.getHeight?.() ?? PLACEHOLDER_HEIGHT_PX;
  }

  // Replaces SliceTrack's built-in row-collapse button, which would be a
  // second, conflicting depth mechanism on this track.
  getTrackShellButtons(): m.Children {
    return m(Button, {
      className: 'pf-visible-on-hover',
      icon: this.view === 'full' ? Icons.UnfoldLess : Icons.UnfoldMore,
      tooltip:
        this.view === 'full'
          ? 'Hide the stack frames'
          : 'Show all stack frames',
      onclick: () => {
        this.view = this.view === 'full' ? MARKERS_ONLY_VIEW : 'full';
        this.viewSetByUser = true;
      },
    });
  }

  onMouseMove(event: TrackMouseEvent): void {
    this.inner?.onMouseMove?.(event);
  }

  onMouseClick(event: TrackMouseEvent): boolean {
    return this.inner?.onMouseClick?.(event) ?? false;
  }

  onMouseDoubleClick(event: TrackMouseEvent): boolean {
    return this.inner?.onMouseDoubleClick?.(event) ?? false;
  }

  onMouseOut(): void {
    this.inner?.onMouseOut?.();
  }

  getDataset(): SourceDataset | undefined {
    return this.inner?.getDataset?.();
  }

  async getSelectionDetails(
    eventId: number,
  ): Promise<TrackEventDetails | undefined> {
    return this.inner?.getSelectionDetails?.(eventId);
  }

  detailsPanel(sel: TrackEventSelection): TrackEventDetailsPanel | undefined {
    return this.inner?.detailsPanel?.(sel);
  }

  renderTooltip(): m.Children {
    return this.inner?.renderTooltip?.();
  }

  getSnapPoint(
    targetTime: time,
    thresholdPx: number,
    timescale: TimeScale,
  ): SnapPoint | undefined {
    return this.inner?.getSnapPoint?.(targetTime, thresholdPx, timescale);
  }

  private ensureInit(): void {
    if (this.initStarted) return;
    this.initStarted = true;
    this.init().catch((e) => {
      console.error(`flamechart track init failed for ${this.uri}`, e);
    });
  }

  private sampleConstraint(): string {
    const source = sqlValueToSqliteString(this.config.source);
    const parts = [`ss.source = ${source}`, `tc.utid = ${this.config.utid}`];
    if (this.config.sessionId === null) {
      parts.push('ss.session_id is null');
    } else if (this.config.sessionId !== undefined) {
      parts.push(`ss.session_id = ${this.config.sessionId}`);
    }
    return parts.join(' and ');
  }

  private async init(): Promise<void> {
    const engine = this.trace.engine;
    const constraint = this.sampleConstraint();
    await engine.query('include perfetto module callstacks.stack_profile;');
    await engine.query('include perfetto module std.trees.table_conversion;');
    await engine.query('include perfetto module std.trees.flamechart;');
    const category = sampleCategorySqlExpr(
      'mp.name',
      'fr.name',
      this.config.processName,
    );
    const runCategory = sampleCategorySqlExpr(
      'mp.name',
      'f.name',
      this.config.processName,
    );
    // Row 0 holds one marker per sample (dur = 0); the flame-chart runs are
    // shifted down one row. frameDepth keeps the unshifted stack depth for
    // depth capping (-1 for markers, so they always pass the cap filter).
    await engine.query(`
      create or replace perfetto table ${this.tableName} as
      select
        row_number() over (order by ts, depth) as id,
        *
      from (
        select
          ss.ts,
          0 as dur,
          0 as depth,
          '' as name,
          1 as sampleCount,
          ${category} as category,
          -1 as frameId,
          -1 as frameDepth
        from stack_sample ss
        join stack_sample_task_context tc on tc.id = ss.task_context_id
        left join stack_profile_callsite c on c.id = ss.callsite_id
        left join stack_profile_frame fr on fr.id = c.frame_id
        left join stack_profile_mapping mp on mp.id = fr.mapping
        where ${constraint}
        union all
        select
          r.ts,
          iif(r.dur = 0, ${ZERO_DUR_RUN_NS}, r.dur) as dur,
          r.depth + 1 as depth,
          iif(f.name = '', 'unknown', f.name) as name,
          r.sample_count as sampleCount,
          ${runCategory} as category,
          r.id as frameId,
          r.depth as frameDepth
        from _flamechart_runs!(
          _tree_from_table!(
            (select id, parent_id, name from _callstack_spc_forest),
            (name)
          ),
          (
            select ss.ts, fl.id as leaf_id
            from stack_sample ss
            join stack_sample_task_context tc on tc.id = ss.task_context_id
            join _callstack_spc_forest fl
              on fl.callsite_id = ss.callsite_id
              and fl.is_leaf_function_in_callsite_frame
            where ${constraint}
            order by ss.ts
          )
        ) as r
        join _callstack_spc_forest as f on f.id = r.id
        left join stack_profile_mapping mp on mp.id = f.mapping_id
      )
    `);
    const result = await engine.query(
      `select ifnull(max(frameDepth), 0) as maxDepth from ${this.tableName}`,
    );
    const maxDepth = result.firstRow({maxDepth: NUM}).maxDepth;

    this.inner = SliceTrack.create({
      trace: this.trace,
      uri: this.uri,
      dataset: () => this.makeDataset(),
      getKey: () => `view:${this.view}`,
      initialMaxDepth: maxDepth + 2,
      sliceName: (row: FlamechartRow) => (row.capped !== 0 ? '…' : row.name),
      colorizer: (row: FlamechartRow) => {
        if (row.capped !== 0) return GRAY;
        return sampleColorScheme(row.category, row.name);
      },
      slicePattern: (row: FlamechartRow) =>
        row.dur === 0n ? 0 : RECT_PATTERN_SAMPLED,
      instantStyle: DIAMOND_INSTANT_STYLE,
      onSliceClick: ({slice}) => {
        if (slice.row.capped !== 0) {
          this.view = 'full';
          return;
        }
        this.trace.selection.selectTrackEvent(this.uri, slice.id);
      },
      tooltip: (slice) => {
        if (slice.row.dur === 0n) {
          return 'Stack sample';
        }
        if (slice.row.capped !== 0) {
          return [
            m(
              'div',
              this.view === MARKERS_ONLY_VIEW
                ? 'Click to show stack frames'
                : 'Click to show all stack frames',
            ),
            m('div', `First hidden frame: ${slice.row.name}`),
          ];
        }
        return [
          m('div', slice.row.name),
          m('div', `${formatDuration(this.trace, slice.row.dur)} (sampled)`),
          m('div', `${slice.row.sampleCount} samples`),
        ];
      },
      detailsPanel: (row: FlamechartRow) => {
        if (row.dur === 0n) {
          // Sample marker: the callstack of that one sample.
          return createProfilingDetailsPanel(
            this.trace,
            Time.fromRaw(row.ts),
            {
              callsiteQuery: (ts) => `
                select ss.callsite_id
                from stack_sample ss
                join stack_sample_task_context tc
                  on tc.id = ss.task_context_id
                where ss.ts = ${ts} and ${constraint}
              `,
              sqlModule: 'callstacks.stack_profile',
              metricName: 'Samples',
              panelTitle: 'Callstack',
              sliceName: 'Sample',
            },
            this.detailsPanelState,
            this.onDetailsPanelStateChange,
          );
        }
        return new FlamechartFrameDetailsPanel(this.trace, {
          frameId: row.frameId,
          name: row.name,
          ts: Time.fromRaw(row.ts),
          dur: row.dur,
          category: row.category,
          sampleCount: row.sampleCount,
          trackUri: this.uri,
        });
      },
    });
  }

  private makeDataset(): SourceDataset<FlamechartRow> {
    const view = this.view;
    if (view === 'full') {
      return new SourceDataset({
        schema: ROW_SCHEMA,
        src: `
          select
            id, ts, dur, depth, name, sampleCount, category, frameId,
            0 as capped
          from ${this.tableName}
        `,
      });
    }
    // Frames above the cap collapse into "…" cut rows one row below it: each
    // such row is the first hidden frame of its subtree, so it both covers
    // the hidden time range and names the first omitted frame on hover.
    return new SourceDataset({
      schema: ROW_SCHEMA,
      src: `
        select
          id, ts, dur, depth, name, sampleCount, category, frameId,
          (frameDepth >= ${view}) as capped
        from ${this.tableName}
        where frameDepth <= ${view}
      `,
    });
  }
}

// Creates the per-thread sampled-stacks flamechart track. See
// FlamechartTrackRenderer for the behaviour.
export function createFlamechartTrack(
  trace: Trace,
  uri: string,
  config: FlamechartTrackConfig,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
): {track: Track; handle: FlamechartTrackHandle} {
  const renderer = new FlamechartTrackRenderer(
    trace,
    uri,
    config,
    detailsPanelState,
    onDetailsPanelStateChange,
  );
  const track: Track = {
    uri,
    tags: {
      // Tagged as a stack-sample track so area selection, select-all and
      // auto-selection flows treat it as the thread's callstacks track.
      kinds: [STACK_SAMPLE_TRACK_KIND, STACK_SAMPLE_FLAMECHART_TRACK_KIND],
      utid: config.utid,
      upid: config.upid,
      stackSampleSource: config.source,
      ...(config.sessionId !== undefined &&
        config.sessionId !== null && {
          stackSampleSessionId: config.sessionId,
        }),
      ...(config.sessionId === null && {stackSampleNullSession: true}),
    },
    renderer,
  };
  return {track, handle: renderer};
}

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
import {Icons} from '../../base/semantic_icons';
import {getColorForSlice} from '../../components/colorizer';
import {MenuItem} from '../../widgets/menu';
import {AsyncMemo} from '../../base/async_memo';
import {uuidv4Sql} from '../../base/uuid';
import {Time, type time} from '../../base/time';
import {formatDuration} from '../../components/time_utils';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import type {
  Track,
  TrackRenderContext,
  TrackRenderer,
  TrackMouseEvent,
  SnapPoint,
  TrackSetting,
  TrackSettingDescriptor,
} from '../../public/track';
import type {TrackEventSelection} from '../../public/selection';
import type {TimeScale} from '../../base/time_scale';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  NUM,
  STR,
  type InferRowType,
} from '../../trace_processor/query_result';
import {sqlValueToSqliteString} from '../../trace_processor/sql_utils';
import {FlamechartFrameDetailsPanel} from './frame_details_panel';
import {sampleColorScheme} from './sample_colors';
import {
  STACK_SAMPLE_FLAMECHART_TRACK_KIND,
  STACK_SAMPLE_TRACK_KIND,
} from './track_kinds';

type FlamechartColorBy = 'mapping' | 'function';

const colorByDescriptor: TrackSettingDescriptor<FlamechartColorBy> = {
  name: 'Color by',
  description: 'Color frames by code mapping or function name.',
  render(setter, values) {
    const value = valueIfAllEqual(values);
    return m(MenuItem, {label: 'Color by'}, [
      m(MenuItem, {
        label: 'Mapping',
        icon: value === 'mapping' ? Icons.RadioChecked : Icons.RadioUnchecked,
        onclick: () => setter('mapping'),
      }),
      m(MenuItem, {
        label: 'Function name',
        icon: value === 'function' ? Icons.RadioChecked : Icons.RadioUnchecked,
        onclick: () => setter('function'),
      }),
    ]);
  },
};

export interface CallstackTrackConfig {
  readonly source: string;
  readonly utid: number;
  readonly upid: number | undefined;
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
  mappingName: STR,
  frameId: NUM,
};

type FlamechartRow = InferRowType<typeof ROW_SCHEMA>;

// Owns sampled-stack preparation, keeping the shared SliceTrack API unchanged.
export class CallstackTrack implements TrackRenderer {
  private readonly inner: SliceTrack<typeof ROW_SCHEMA>;
  private colorBy: FlamechartColorBy = 'function';
  private readonly initializationSlot = new AsyncMemo<boolean>();
  private initializationPromise?: Promise<void>;
  private initialized = false;
  private readonly tableName = `__flamechart_runs_${uuidv4Sql()}`;

  constructor(
    private readonly trace: Trace,
    uri: string,
    private readonly config: CallstackTrackConfig,
  ) {
    this.inner = SliceTrack.create({
      trace,
      uri,
      dataset: new SourceDataset({schema: ROW_SCHEMA, src: this.tableName}),
      sliceLayout: {collapsed: true},
      getKey: () => this.colorBy,
      colorizer: (row: FlamechartRow) =>
        this.colorBy === 'function'
          ? getColorForSlice(row.name, {stripTrailingDigits: false})
          : sampleColorScheme(row.category, row.mappingName),
      tooltip: (slice) => {
        return [
          m('div', slice.row.name),
          m(
            'div',
            slice.row.dur === -1n
              ? 'Incomplete (sampled)'
              : `${formatDuration(trace, slice.row.dur)} (sampled)`,
          ),
          m('div', `${slice.row.sampleCount} samples`),
        ];
      },
      detailsPanel: (row: FlamechartRow) => {
        return new FlamechartFrameDetailsPanel(trace, {
          frameId: row.frameId,
          name: row.name,
          ts: Time.fromRaw(row.ts),
          dur: row.dur,
          category: row.category,
          sampleCount: row.sampleCount,
          trackUri: uri,
        });
      },
    });
  }

  get settings(): ReadonlyArray<TrackSetting> {
    const colorBy: TrackSetting<FlamechartColorBy> = {
      descriptor: colorByDescriptor,
      value: this.colorBy,
      update: (value) => {
        if (value === this.colorBy) return;
        this.colorBy = value;
        this.trace.raf.scheduleFullRedraw();
      },
    };
    return [colorBy];
  }

  private initialize(): Promise<void> {
    return (this.initializationPromise ??= this.prepare().then(() => {
      this.initialized = true;
      this.trace.raf.scheduleFullRedraw();
    }));
  }

  render(ctx: TrackRenderContext): void {
    if (!this.initialized) {
      this.initializationSlot.use({
        key: {},
        compute: async () => {
          await this.initialize();
          return true;
        },
      });
      return;
    }
    this.inner.render(ctx);
  }

  getHeight() {
    return this.inner.getHeight();
  }
  getSliceVerticalBounds(depth: number) {
    return this.inner.getSliceVerticalBounds(depth);
  }
  getTrackShellButtons() {
    return this.inner.getTrackShellButtons();
  }
  onMouseMove(event: TrackMouseEvent) {
    this.inner.onMouseMove(event);
  }
  onMouseOut() {
    this.inner.onMouseOut();
  }
  onMouseClick(event: TrackMouseEvent) {
    return this.inner.onMouseClick(event);
  }
  onMouseDoubleClick(event: TrackMouseEvent) {
    return this.inner.onMouseDoubleClick(event);
  }
  renderTooltip() {
    return this.inner.renderTooltip();
  }
  getSnapPoint(
    targetTime: time,
    thresholdPx: number,
    timescale: TimeScale,
  ): SnapPoint | undefined {
    return this.inner.getSnapPoint(targetTime, thresholdPx, timescale);
  }
  getDataset() {
    return this.initialized ? this.inner.getDataset() : undefined;
  }
  async getSelectionDetails(id: number) {
    await this.initialize();
    return this.inner.getSelectionDetails(id);
  }
  detailsPanel(selection: TrackEventSelection) {
    return this.initialized ? this.inner.detailsPanel(selection) : undefined;
  }

  private sampleConstraint(): string {
    const {config} = this;
    const source = sqlValueToSqliteString(config.source);
    const parts = [`ss.source = ${source}`, `tc.utid = ${config.utid}`];
    if (config.sessionId === null) {
      parts.push('ss.session_id is null');
    } else if (config.sessionId !== undefined) {
      parts.push(`ss.session_id = ${config.sessionId}`);
    }
    return parts.join(' and ');
  }

  private async prepare(): Promise<void> {
    const {trace, tableName} = this;
    const engine = trace.engine;
    const constraint = this.sampleConstraint();
    await engine.query('include perfetto module callstacks.stack_profile;');
    await engine.query('include perfetto module std.trees.table_conversion;');
    await engine.query('include perfetto module std.stack_sample.flamechart;');
    await engine.query('include perfetto module std.stack_sample.mapping;');
    await engine.query(`
      create perfetto table ${tableName} as
      select row_number() over (order by ts, depth) as id, *
      from (
        select
          r.ts,
          r.dur,
          r.depth as depth,
          iif(f.name = '', 'unknown', f.name) as name,
          r.sample_count as sampleCount,
          coalesce(mp.category, 3) as category,
          coalesce(mp.name, '') as mappingName,
          r.id as frameId
        from _stack_sample_flamechart_runs!(
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
        left join _stack_sample_mapping_classification mp on mp.id = f.mapping_id
      )
    `);
  }
}

export function createCallstackTrack(
  trace: Trace,
  uri: string,
  config: CallstackTrackConfig,
): Track {
  const renderer = new CallstackTrack(trace, uri, config);
  const track: Track = {
    uri,
    description:
      'This track shows sampled callstacks. Each row is a stack depth, and ' +
      'each block represents a frame observed in consecutive samples. ' +
      'Unlike instrumented slices, block boundaries and durations are inferred ' +
      'from samples, not recorded function entry and exit times. Calls between ' +
      'samples may be missed. Frames still open at the last sample extend ' +
      'to trace end as incomplete.',
    tags: {
      // Preserve stack-sample area selection for the child as well.
      // Automatic sample-track discovery excludes the flamechart kind.
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
  return track;
}

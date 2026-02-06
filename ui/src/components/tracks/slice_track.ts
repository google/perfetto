// Copyright (C) 2023 The Android Open Source Project
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
import {drawIncompleteSlice} from '../../base/canvas_utils';
import {colorCompare} from '../../base/color';
import {ColorScheme} from '../../base/color_scheme';
import {Point2D, Size2D, VerticalBounds} from '../../base/geom';
import {HighPrecisionTime} from '../../base/high_precision_time';
import {assertExists} from '../../base/logging';
import {clamp, floatEqual} from '../../base/math_utils';
import {Monitor} from '../../base/monitor';
import {
  CancellationSignal,
  QuerySlot,
  QUERY_CANCELLED,
  SerialTaskQueue,
} from '../../base/query_slot';
import {cropText} from '../../base/string_utils';
import {duration, Time, time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {exists} from '../../base/utils';
import {deferChunkedTask} from '../../base/chunked_task';
import {featureFlags} from '../../core/feature_flags';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventDetails, TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {
  SnapPoint,
  TrackMouseEvent,
  TrackRenderContext,
  TrackRenderer,
} from '../../public/track';
import {DatasetSchema, SourceDataset} from '../../trace_processor/dataset';
import {
  SqlValue,
  LONG,
  NUM,
  LONG_NULL,
} from '../../trace_processor/query_result';
import {
  createPerfettoTable,
  createVirtualTable,
} from '../../trace_processor/sql_utils';
import {checkerboardExcept} from '../checkerboard';
import {getColorForSlice} from '../colorizer';
import {formatDuration} from '../time_utils';
import {BufferedBounds} from './buffered_bounds';
import {CHUNKED_TASK_BACKGROUND_PRIORITY} from './feature_flags';
import {SliceTrackDetailsPanel} from './slice_track_details_panel';

// Slice flags
export const SLICE_FLAGS_INCOMPLETE = 1;
export const SLICE_FLAGS_INSTANT = 2;

export interface Slice {
  // These properties are updated only once per query result when the Slice
  // object is created and don't change afterwards.
  readonly id: number;
  readonly startNs: time;
  readonly endNs: time;
  readonly durNs: duration;
  readonly ts: time;
  readonly count: number;
  readonly dur: duration;
  readonly depth: number;
  readonly flags: number;

  // Each slice can represent some extra numerical information by rendering a
  // portion of the slice with a lighter tint.
  // |fillRatio| describes the ratio of the normal area to the tinted area
  // width of the slice, normalized between 0.0 -> 1.0.
  // 0.0 means the whole slice is tinted.
  // 1.0 means none of the slice is tinted.
  // E.g. If |fillRatio| = 0.65 the slice will be rendered like this:
  // [############|*******]
  // ^------------^-------^
  //     Normal     Light
  readonly fillRatio: number;

  // These can be changed by the Impl.
  title?: string;
  subTitle: string;
  colorScheme: ColorScheme;
  isHighlighted: boolean;

  // Pattern flags for slice rendering (e.g., RECT_PATTERN_HATCHED for RT threads)
  pattern: number;

  // Controls which color from colorScheme to use for rendering
  colorVariant: 'base' | 'variant' | 'disabled';
}

// The minimal set of columns that any table/view must expose to render tracks.
export const BASE_ROW = {
  id: NUM, // The slice ID, for selection / lookups.
  ts: LONG, // True ts in nanoseconds.
  dur: LONG, // True duration in nanoseconds. -1 = incomplete, 0 = instant.
  tsQ: LONG, // Quantized start time in nanoseconds.
  durQ: LONG, // Quantized duration in nanoseconds.
  count: NUM, // Number of slices that were merged to create this slice.
  depth: NUM, // Vertical depth.
};

export type BaseRow = typeof BASE_ROW;

export interface SliceLayout {
  // Vertical spacing between slices and track.
  readonly padding: number;

  // Spacing between rows.
  readonly rowGap: number;

  // Height of each slice (i.e. height of each row).
  readonly sliceHeight: number;

  // Title font size.
  readonly titleSizePx: number;

  // Subtitle font size.
  readonly subtitleSizePx: number;
}

// Callback argument types
export interface OnSliceOverArgs<S extends Slice> {
  slice: S;
  tooltip?: string[];
}

export interface OnSliceOutArgs<S extends Slice> {
  slice: S;
}

export interface OnSliceClickArgs<S extends Slice> {
  slice: S;
}

// Utility function for filtering visible slices - exported for testing
function filterVisibleSlices<S extends Slice>(
  slices: S[],
  start: time,
  end: time,
): S[] {
  return slices.filter((slice) => slice.startNs <= end && slice.endNs >= start);
}

export const filterVisibleSlicesForTesting = filterVisibleSlices;

const BUCKETS_PER_PIXEL = 2;
const SLICE_MIN_WIDTH_FOR_TEXT_PX = 5;
const SLICE_MIN_WIDTH_PX = 1 / BUCKETS_PER_PIXEL;
const SLICE_MIN_WIDTH_FADED_PX = 0.1;
const CHEVRON_WIDTH_PX = 10;
const INCOMPLETE_SLICE_WIDTH_PX = 20;

export const CROP_INCOMPLETE_SLICE_FLAG = featureFlags.register({
  id: 'cropIncompleteSlice',
  name: 'Crop incomplete slices',
  description: 'Display incomplete slices in short form',
  defaultValue: false,
});

export const FADE_THIN_SLICES_FLAG = featureFlags.register({
  id: 'fadeThinSlices',
  name: 'Fade thin slices',
  description: 'Display sub-pixel slices in a faded way',
  defaultValue: false,
});

export interface InstantStyle {
  /**
   * Defines the width of an instant event. This, combined with the row height,
   * defines the event's hitbox. This width is forwarded to the render function.
   */
  readonly width: number;

  /**
   * Customize how instant events are rendered.
   *
   * @param ctx - CanvasRenderingContext to draw to.
   * @param rect - Position of the TL corner & size of the instant event's
   * bounding box.
   */
  render(ctx: CanvasRenderingContext2D, rect: Size2D & Point2D): void;
}

export interface SliceTrackAttrs<T extends DatasetSchema> {
  /**
   * The trace object used by the track for accessing the query engine and other
   * trace-related resources.
   */
  readonly trace: Trace;

  /**
   * The URI of this track, which must match the URI specified in the track
   * descriptor.
   */
  readonly uri: string;

  /**
   * The source dataset defining the content of this track.
   *
   * A source dataset consists of a SQL select statement or table name with a
   * column schema and optional filtering information. It represents a set of
   * instructions to extract slice-like rows from trace processor that
   * represents the content of this track, which avoids the need to materialize
   * all slices into JavaScript beforehand.
   *
   * Required columns:
   * - `ts` (LONG): Timestamp of each event (in nanoseconds).
   *
   * Auto-generated columns (if not provided):
   * - `id` (NUM): Unique identifier for slices in the track.
   *
   * Optional columns:
   * - `dur` (LONG): Duration of each event (in nanoseconds).
   * - `depth` (NUM): Depth of each event, used for vertical arrangement.
   * - `layer` (NUM): Layer value for mipmap function.
   */
  readonly dataset: SourceDataset<T> | (() => SourceDataset<T>);

  /**
   * An optional initial estimate for the maximum depth value.
   */
  readonly initialMaxDepth?: number;

  /**
   * An optional root table name for the track's data source.
   */
  readonly rootTableName?: string;

  /**
   * Override the default geometry and layout of the slices.
   */
  readonly sliceLayout?: Partial<SliceLayout>;

  /**
   * Override the appearance of instant events.
   */
  readonly instantStyle?: InstantStyle;

  /**
   * Forces events to be rendered in timestamp order instead of color order.
   */
  readonly forceTsRenderOrder?: boolean;

  /**
   * Override the color scheme for each event.
   */
  colorizer?(row: T): ColorScheme;

  /**
   * Override the text displayed on each event (title).
   */
  sliceName?(row: T): string;

  /**
   * Override the subtitle displayed on each event.
   */
  sliceSubtitle?(row: T): string;

  /**
   * Override the tooltip content for each event.
   */
  tooltip?(slice: SliceWithRow<T>): m.Children;

  /**
   * Customize the details panel for events on this track.
   */
  detailsPanel?(row: T): TrackEventDetailsPanel;

  /**
   * Define the fill ratio for slices (0.0 to 1.0).
   */
  fillRatio?(row: T): number;

  /**
   * Override the pattern for each slice (e.g., RECT_PATTERN_HATCHED for RT threads).
   */
  slicePattern?(row: T): number;

  /**
   * Define buttons displayed on the track shell.
   */
  shellButtons?(): m.Children;

  /**
   * Called once per render cycle before drawing. Use this to batch-update
   * slice properties (colorVariant, pattern, etc.) based on global state.
   */
  onUpdatedSlices?(slices: Array<SliceWithRow<T>>): void;

  /**
   * Called when a slice is hovered.
   */
  onSliceOver?(args: OnSliceOverArgs<SliceWithRow<T>>): void;

  /**
   * Called when hover leaves a slice.
   */
  onSliceOut?(args: OnSliceOutArgs<SliceWithRow<T>>): void;

  /**
   * Called when a slice is clicked. Return false to prevent default selection.
   */
  onSliceClick?(args: OnSliceClickArgs<SliceWithRow<T>>): void;
}

export type RowSchema = {
  readonly id?: number;
  readonly ts: bigint;
  readonly dur?: bigint | null;
  readonly depth?: number;
  readonly layer?: number;
} & DatasetSchema;

// We attach a copy of our rows to each slice, so that the tooltip can be
// resolved properly.
export type SliceWithRow<T> = Slice & {row: T};

// Internal slice properties for rendering
interface SliceInternal {
  x: number;
  w: number;
  startRelNs: number;
}

type CastInternal<S extends Slice> = S & SliceInternal;

// Result from table creation
interface MipmapTableResult<SliceT> extends AsyncDisposable {
  tableName: string;
  incomplete: SliceT[];
}

// Result from data fetching
interface SliceDataResult<SliceT> {
  slices: SliceT[];
  refStart: time;
}

function getDataset<T extends DatasetSchema>(
  attrs: SliceTrackAttrs<T>,
): SourceDataset<T> {
  const dataset = attrs.dataset;
  return typeof dataset === 'function' ? dataset() : dataset;
}

export class SliceTrack<T extends RowSchema> implements TrackRenderer {
  readonly rootTableName?: string;
  protected readonly trace: Trace;
  protected readonly uri: string;
  protected readonly sliceLayout: SliceLayout;
  protected hoveredSlice?: SliceWithRow<T>;

  private readonly attrs: SliceTrackAttrs<T>;
  private readonly rowSpec: BaseRow & T;
  private readonly depthGuess: number;
  private readonly instantWidthPx: number;
  private readonly forceTimestampRenderOrder: boolean;

  // QuerySlot infrastructure
  private readonly queue = new SerialTaskQueue();
  private readonly tableSlot = new QuerySlot<
    MipmapTableResult<CastInternal<SliceWithRow<T>>>
  >(this.queue);
  private readonly dataSlot = new QuerySlot<
    SliceDataResult<CastInternal<SliceWithRow<T>>>
  >(this.queue);

  // Buffered bounds tracking
  private readonly bufferedBounds = new BufferedBounds();

  // Reference start time for relative timestamp calculations
  private dataRefStart: time = Time.ZERO;

  // Cached slices
  private slices = new Array<CastInternal<SliceWithRow<T>>>();
  private incomplete = new Array<CastInternal<SliceWithRow<T>>>();
  private selectedSlice?: CastInternal<SliceWithRow<T>>;

  private extraSqlColumns: string[];
  private charWidth = -1;
  private readonly hoverMonitor = new Monitor([() => this.hoveredSlice?.id]);
  private maxDataDepth = 0;
  private computedTrackHeight = 0;

  /**
   * Factory function to create a SliceTrack.
   */
  static create<T extends RowSchema>(attrs: SliceTrackAttrs<T>): SliceTrack<T> {
    return new SliceTrack(attrs);
  }

  /**
   * Async factory function to create a SliceTrack with a materialized dataset.
   */
  static async createMaterialized<T extends RowSchema>(
    attrs: SliceTrackAttrs<T>,
  ): Promise<SliceTrack<T>> {
    const originalDataset = getDataset(attrs);
    const materializedTable = await createPerfettoTable({
      engine: attrs.trace.engine,
      as: generateRenderQuery(originalDataset),
    });

    const materializedDataset = new SourceDataset({
      src: materializedTable.name,
      schema: {
        ...originalDataset.schema,
        id: NUM,
        layer: NUM,
        depth: NUM,
        dur: LONG,
      },
    });

    return new SliceTrack({
      ...attrs,
      dataset: materializedDataset,
    });
  }

  private constructor(attrs: SliceTrackAttrs<T>) {
    this.attrs = attrs;
    this.trace = attrs.trace;
    this.uri = attrs.uri;
    this.rootTableName = attrs.rootTableName;
    this.depthGuess = attrs.initialMaxDepth ?? 0;
    this.instantWidthPx = attrs.instantStyle?.width ?? CHEVRON_WIDTH_PX;
    this.forceTimestampRenderOrder = attrs.forceTsRenderOrder ?? false;

    const dataset = getDataset(attrs);
    this.rowSpec = {...BASE_ROW, ...dataset.schema} as BaseRow & T;

    // Work out the extra columns
    const allCols = Object.keys(this.rowSpec);
    const baseCols = Object.keys(BASE_ROW);
    this.extraSqlColumns = allCols.filter((key) => !baseCols.includes(key));

    const sliceLayout = attrs.sliceLayout ?? {};
    this.sliceLayout = {
      padding: sliceLayout.padding ?? 3,
      rowGap: sliceLayout.rowGap ?? 0,
      sliceHeight: sliceLayout.sliceHeight ?? 18,
      titleSizePx: sliceLayout.titleSizePx ?? 12,
      subtitleSizePx: sliceLayout.subtitleSizePx ?? 8,
    };
  }

  private getSqlSource(): string {
    const dataset = getDataset(this.attrs);
    return generateRenderQuery(dataset);
  }

  getDataset() {
    return getDataset(this.attrs);
  }

  private get engine() {
    return this.trace.engine;
  }

  private getTitleFont(): string {
    return `${this.sliceLayout.titleSizePx}px Roboto Condensed`;
  }

  private getSubtitleFont(): string {
    return `${this.sliceLayout.subtitleSizePx}px Roboto Condensed`;
  }

  // Creates the mipmap table and fetches incomplete slices
  private async createMipmapTable(): Promise<
    MipmapTableResult<CastInternal<SliceWithRow<T>>>
  > {
    const rowCount = await this.getRowCount();

    const extraCols = this.extraSqlColumns.join(',');
    let queryRes;
    if (CROP_INCOMPLETE_SLICE_FLAG.get()) {
      queryRes = await this.engine.query(`
        select
          depth,
          ts as tsQ,
          ts,
          1 as count,
          -1 as durQ,
          -1 as dur,
          id
          ${extraCols ? ',' + extraCols : ''}
        from (${this.getSqlSource()})
        where dur = -1;
      `);
    } else {
      queryRes = await this.engine.query(`
        select
          depth,
          max(ts) as tsQ,
          ts,
          1 as count,
          -1 as durQ,
          -1 as dur,
          id
          ${extraCols ? ',' + extraCols : ''}
        from (${this.getSqlSource()})
        group by 1
        having dur = -1
      `);
    }

    const incomplete = new Array<CastInternal<SliceWithRow<T>>>(
      queryRes.numRows(),
    );
    const it = queryRes.iter(this.rowSpec);
    for (let i = 0; it.valid(); it.next(), ++i) {
      incomplete[i] = this.rowToSliceInternal(it);
    }
    this.onUpdatedSlices(incomplete);

    const table = await createVirtualTable({
      engine: this.engine,
      using: `__intrinsic_slice_mipmap((
        select id, ts, dur, ((layer * ${rowCount ?? 1}) + depth) as depth
        from (${this.getSqlSource()})
        where dur != -1
      ))`,
    });

    return {
      tableName: table.name,
      incomplete,
      [Symbol.asyncDispose]: async () => {
        await table[Symbol.asyncDispose]();
      },
    };
  }

  private async getRowCount(): Promise<number | undefined> {
    const result = await this.engine.query(`
      SELECT
        IFNULL(depth, 0) + 1 AS rowCount
      FROM (${this.getSqlSource()})
      ORDER BY depth DESC
      LIMIT 1
    `);
    return result.maybeFirstRow({rowCount: NUM})?.rowCount;
  }

  private useData(trackCtx: TrackRenderContext): boolean {
    const {size, visibleWindow} = trackCtx;

    const tableResult = this.tableSlot.use({
      key: {sqlSource: this.getSqlSource()},
      queryFn: () => this.createMipmapTable(),
    });

    const table = tableResult.data;
    if (table === undefined) return false;

    this.incomplete = table.incomplete;

    const visibleSpan = visibleWindow.toTimeSpan();
    const windowSizePx = Math.max(1, size.width);
    const bucketSize = this.computeBucketSize(
      visibleSpan.duration,
      windowSizePx,
    );
    const bounds = this.bufferedBounds.update(
      visibleSpan,
      bucketSize,
      this.slices.length > 0,
    );

    const queryStart = bounds.start;
    const dataResult = this.dataSlot.use({
      key: {
        start: bounds.start,
        end: bounds.end,
        resolution: bounds.resolution,
      },
      queryFn: async (signal) => {
        const result = await this.trace.taskTracker.track(
          this.fetchSlices(
            table.tableName,
            queryStart,
            bounds.end,
            bounds.resolution,
            signal,
          ),
          'Loading slices',
        );
        this.trace.raf.scheduleFullRedraw();
        return {slices: result, refStart: queryStart};
      },
      retainOn: ['start', 'end', 'resolution'],
    });

    if (dataResult.data !== undefined) {
      this.slices = dataResult.data.slices;
      this.dataRefStart = dataResult.data.refStart;

      for (const slice of this.incomplete) {
        slice.startRelNs = Number(slice.startNs - this.dataRefStart);
      }
    }

    return this.slices.length > 0 || this.incomplete.length > 0;
  }

  private computeBucketSize(
    spanDuration: duration,
    windowSizePx: number,
  ): duration {
    const nsPerPx = Math.max(1, Number(spanDuration) / windowSizePx);
    const bucketNs = nsPerPx / BUCKETS_PER_PIXEL;
    const exp = Math.ceil(Math.log2(Math.max(1, bucketNs)));
    return BigInt(Math.pow(2, exp)) as duration;
  }

  private async fetchSlices(
    tableName: string,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
  ): Promise<CastInternal<SliceWithRow<T>>[]> {
    const slices = new Array<CastInternal<SliceWithRow<T>>>();

    const extraCols = this.extraSqlColumns.join(',');
    const queryRes = await this.engine.query(`
      SELECT
        (z.ts / ${resolution}) * ${resolution} as tsQ,
        ((z.dur + ${resolution - 1n}) / ${resolution}) * ${resolution} as durQ,
        z.count as count,
        s.ts as ts,
        s.dur as dur,
        s.id,
        s.depth
        ${extraCols ? ',' + extraCols : ''}
      FROM ${tableName}(
        ${start},
        ${end},
        ${resolution}
      ) z
      CROSS JOIN (${this.getSqlSource()}) s using (id)
    `);

    if (signal.isCancelled) throw QUERY_CANCELLED;

    const priority = CHUNKED_TASK_BACKGROUND_PRIORITY.get()
      ? 'background'
      : undefined;
    const task = await deferChunkedTask({priority});

    const it = queryRes.iter(this.rowSpec);

    let maxDataDepth = this.maxDataDepth;
    for (let i = 0; it.valid(); it.next(), ++i) {
      if (signal.isCancelled) throw QUERY_CANCELLED;
      if (i % 50 === 0 && task.shouldYield()) {
        await task.yield();
      }

      if (it.dur === -1n) {
        continue;
      }

      maxDataDepth = Math.max(maxDataDepth, it.depth);
      slices.push(this.rowToSliceInternal(it));
    }
    for (const incomplete of this.incomplete) {
      maxDataDepth = Math.max(maxDataDepth, incomplete.depth);
    }
    this.maxDataDepth = maxDataDepth;

    for (const slice of slices) {
      slice.startRelNs = Number(slice.startNs - start);
    }

    this.onUpdatedSlices(slices);
    return slices;
  }

  private rowToSliceInternal(row: BaseRow & T): CastInternal<SliceWithRow<T>> {
    const slice = this.rowToSlice(row);

    if (this.selectedSlice?.id === slice.id) {
      this.selectedSlice = undefined;
    }

    return {
      ...slice,
      x: -1,
      w: -1,
      startRelNs: 0,
    };
  }

  private rowToSlice(row: BaseRow & T): SliceWithRow<T> {
    let flags = 0;
    if (row.dur === -1n) {
      flags |= SLICE_FLAGS_INCOMPLETE;
    } else if (row.dur === 0n) {
      flags |= SLICE_FLAGS_INSTANT;
    }

    const title = this.getTitle(row);
    const subTitle = this.getSubtitle(row);
    const colorScheme = this.getColor(row, title);
    const dataset = getDataset(this.attrs);

    // Clone the row with only schema keys
    const clonedRow: Record<string, SqlValue> = {};
    for (const k in dataset.schema) {
      clonedRow[k] = row[k];
    }

    return {
      id: row.id,
      startNs: Time.fromRaw(row.tsQ),
      endNs: Time.fromRaw(row.tsQ + row.durQ),
      durNs: row.durQ,
      ts: Time.fromRaw(row.ts),
      count: row.count,
      dur: row.dur,
      flags,
      depth: row.depth,
      title,
      subTitle,
      fillRatio: this.attrs.fillRatio?.(row) ?? 1,
      colorScheme,
      isHighlighted: false,
      pattern: this.attrs.slicePattern?.(row) ?? 0,
      colorVariant: 'base',
      row: clonedRow as T,
    };
  }

  private getTitle(row: T): string | undefined {
    if (this.attrs.sliceName) return this.attrs.sliceName(row);
    if ('name' in row && typeof row.name === 'string') return row.name;
    return undefined;
  }

  private getSubtitle(row: T): string {
    if (this.attrs.sliceSubtitle) return this.attrs.sliceSubtitle(row);
    return '';
  }

  private getColor(row: T, title: string | undefined): ColorScheme {
    if (this.attrs.colorizer) return this.attrs.colorizer(row);
    if (title) return getColorForSlice(title);
    return getColorForSlice(`${row.id}`);
  }

  onFullRedraw(): void {
    this.onUpdatedSlices(this.slices);
    this.onUpdatedSlices(this.incomplete);
    if (this.selectedSlice !== undefined) {
      this.onUpdatedSlices([this.selectedSlice]);
    }
  }

  private onUpdatedSlices(slices: Array<CastInternal<SliceWithRow<T>>>): void {
    if (this.attrs.onUpdatedSlices) {
      this.attrs.onUpdatedSlices(slices);
    } else {
      this.highlightHoveredAndSameTitle(slices);
    }
  }

  protected highlightHoveredAndSameTitle(slices: Slice[]) {
    const highlightedSliceId = this.trace.timeline.highlightedSliceId;
    const hoveredTitle = this.hoveredSlice?.title;
    for (const slice of slices) {
      const isHovering =
        highlightedSliceId === slice.id ||
        (hoveredTitle && hoveredTitle === slice.title);
      slice.isHighlighted = Boolean(isHovering);
    }
  }

  render(trackCtx: TrackRenderContext): void {
    const {ctx, size, timescale, colors, renderer} = trackCtx;

    if (!this.useData(trackCtx)) {
      return;
    }

    let charWidth = this.charWidth;
    if (charWidth < 0) {
      ctx.font = this.getTitleFont();
      charWidth = this.charWidth = ctx.measureText('dbpqaouk').width / 8;
    }

    const vizSlices = this.getVisibleSlicesInternal();

    const selection = this.trace.selection.selection;
    const selectedId =
      selection.kind === 'track_event' && selection.trackUri === this.uri
        ? selection.eventId
        : undefined;

    if (selectedId === undefined) {
      this.selectedSlice = undefined;
    }
    let discoveredSelection: CastInternal<SliceWithRow<T>> | undefined;

    const sliceHeight = this.sliceLayout.sliceHeight;
    const padding = this.sliceLayout.padding;
    const rowSpacing = this.sliceLayout.rowGap;
    const pxEnd = size.width;
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(this.dataRefStart);

    // First pass: compute geometry
    for (const slice of vizSlices) {
      slice.x = slice.startRelNs * pxPerNs + baseOffsetPx;
      slice.w = Number(slice.durNs) * pxPerNs;

      if (slice.flags & SLICE_FLAGS_INSTANT) {
        slice.x -= this.instantWidthPx / 2;
        slice.w = this.instantWidthPx;
      } else if (slice.flags & SLICE_FLAGS_INCOMPLETE) {
        let widthPx;
        if (CROP_INCOMPLETE_SLICE_FLAG.get()) {
          widthPx =
            slice.x > 0
              ? Math.min(pxEnd, INCOMPLETE_SLICE_WIDTH_PX)
              : Math.max(0, INCOMPLETE_SLICE_WIDTH_PX + slice.x);
          slice.x = Math.max(slice.x, 0);
        } else {
          slice.x = Math.max(slice.x, 0);
          widthPx = pxEnd - slice.x;
        }
        slice.w = widthPx;
      } else {
        const sliceVizLimit = Math.min(slice.x + slice.w, pxEnd);
        slice.x = Math.max(slice.x, -1);
        slice.w = sliceVizLimit - slice.x;
      }

      if (selectedId === slice.id) {
        discoveredSelection = slice;
      }
    }

    // Second pass: fill slices by color
    const vizSlicesByColor = vizSlices.slice();
    if (!this.forceTimestampRenderOrder) {
      vizSlicesByColor.sort((a, b) =>
        colorCompare(a.colorScheme.base, b.colorScheme.base),
      );
    }

    for (const slice of vizSlices) {
      const color = slice.colorScheme[slice.colorVariant];
      const y = padding + slice.depth * (sliceHeight + rowSpacing);

      if (slice.flags & SLICE_FLAGS_INSTANT) {
        renderer.drawMarker(
          slice.x,
          y,
          CHEVRON_WIDTH_PX,
          sliceHeight,
          color,
          () => this.drawChevron(ctx, slice.x, y, sliceHeight),
        );
      } else if (slice.flags & SLICE_FLAGS_INCOMPLETE) {
        const w = CROP_INCOMPLETE_SLICE_FLAG.get()
          ? slice.w
          : Math.max(slice.w - 2, 2);
        renderer.flush();
        drawIncompleteSlice(
          ctx,
          slice.x,
          y,
          w,
          sliceHeight,
          color,
          !CROP_INCOMPLETE_SLICE_FLAG.get(),
        );
      } else {
        const w = Math.max(
          slice.w,
          FADE_THIN_SLICES_FLAG.get()
            ? SLICE_MIN_WIDTH_FADED_PX
            : SLICE_MIN_WIDTH_PX,
        );
        renderer.drawRect(
          slice.x,
          y,
          slice.x + w,
          y + sliceHeight,
          color,
          slice.pattern,
        );
      }
    }

    renderer.flush();

    // Pass 2.5: Draw fillRatio light section
    ctx.fillStyle = `#FFFFFF50`;
    for (const slice of vizSlicesByColor) {
      if (slice.flags & (SLICE_FLAGS_INCOMPLETE | SLICE_FLAGS_INSTANT)) {
        continue;
      }

      const fillRatio = clamp(slice.fillRatio, 0, 1);
      if (floatEqual(fillRatio, 1)) {
        continue;
      }

      const sliceDrawWidth = Math.max(slice.w, SLICE_MIN_WIDTH_PX);
      const lightSectionDrawWidth = sliceDrawWidth * (1 - fillRatio);
      if (lightSectionDrawWidth < 1) {
        continue;
      }

      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      const x = slice.x + (sliceDrawWidth - lightSectionDrawWidth);
      ctx.fillRect(x, y, lightSectionDrawWidth, sliceHeight);
    }

    // Third pass: draw titles
    ctx.textAlign = 'center';
    ctx.font = this.getTitleFont();
    ctx.textBaseline = 'middle';
    for (const slice of vizSlices) {
      if (
        slice.flags & SLICE_FLAGS_INSTANT ||
        !slice.title ||
        slice.w < SLICE_MIN_WIDTH_FOR_TEXT_PX
      ) {
        continue;
      }

      const textColor =
        slice.colorVariant === 'base'
          ? slice.colorScheme.textBase
          : slice.colorVariant === 'variant'
            ? slice.colorScheme.textVariant
            : slice.colorScheme.textDisabled;
      ctx.fillStyle = textColor.cssString;
      const title = cropText(slice.title, charWidth, slice.w);
      const rectXCenter = slice.x + slice.w / 2;
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      const yDiv = slice.subTitle ? 3 : 2;
      const yMidPoint = Math.floor(y + sliceHeight / yDiv) + 0.5;
      ctx.fillText(title, rectXCenter, yMidPoint);
    }

    // Fourth pass: draw subtitles
    ctx.fillStyle = 'rgba(255, 255, 255, 0.6)';
    ctx.font = this.getSubtitleFont();
    for (const slice of vizSlices) {
      if (
        slice.w < SLICE_MIN_WIDTH_FOR_TEXT_PX ||
        !slice.subTitle ||
        slice.flags & SLICE_FLAGS_INSTANT
      ) {
        continue;
      }
      const rectXCenter = slice.x + slice.w / 2;
      const subTitle = cropText(slice.subTitle, charWidth, slice.w);
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      const yMidPoint = Math.ceil(y + (sliceHeight * 2) / 3) + 1.5;
      ctx.fillText(subTitle, rectXCenter, yMidPoint);
    }

    // Draw selection
    if (discoveredSelection !== undefined) {
      this.selectedSlice = discoveredSelection;
      const slice = discoveredSelection;
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      ctx.strokeStyle = colors.COLOR_TIMELINE_OVERLAY;
      ctx.beginPath();
      const THICKNESS = 3;
      ctx.lineWidth = THICKNESS;
      ctx.strokeRect(
        slice.x,
        y - THICKNESS / 2,
        slice.w,
        sliceHeight + THICKNESS,
      );
      ctx.closePath();
    }

    // Checkerboard for loading areas
    const loadedBounds = this.bufferedBounds.bounds;
    const loadedEndPx = timescale.timeToPx(loadedBounds.end);
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      baseOffsetPx,
      loadedEndPx,
    );
  }

  renderTooltip(): m.Children {
    if (!this.hoveredSlice) {
      return undefined;
    }
    return (
      this.attrs.tooltip?.(this.hoveredSlice) ??
      renderTooltip(this.trace, this.hoveredSlice)
    );
  }

  protected drawChevron(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    h: number,
  ) {
    if (this.attrs.instantStyle?.render) {
      this.attrs.instantStyle.render(ctx, {
        x,
        y,
        height: h,
        width: this.attrs.instantStyle.width,
      });
    } else {
      const HALF_CHEVRON_WIDTH_PX = CHEVRON_WIDTH_PX / 2;
      const midX = x + HALF_CHEVRON_WIDTH_PX;
      ctx.beginPath();
      ctx.moveTo(midX, y);
      ctx.lineTo(x + CHEVRON_WIDTH_PX, y + h);
      ctx.lineTo(midX, y + h - HALF_CHEVRON_WIDTH_PX);
      ctx.lineTo(x, y + h);
      ctx.lineTo(midX, y);
      ctx.closePath();
      ctx.fill();
    }
  }

  private getVisibleSlicesInternal(): Array<CastInternal<SliceWithRow<T>>> {
    const slices = this.slices.concat(this.incomplete);
    if (this.selectedSlice && !this.slices.includes(this.selectedSlice)) {
      slices.push(this.selectedSlice);
    }
    return slices;
  }

  private updateSliceAndTrackHeight() {
    const rows = Math.max(this.maxDataDepth, this.depthGuess) + 1;
    const {padding = 2, sliceHeight = 12, rowGap = 0} = this.sliceLayout;
    this.computedTrackHeight = 2 * padding + rows * (sliceHeight + rowGap);
  }

  getHeight(): number {
    this.updateSliceAndTrackHeight();
    return this.computedTrackHeight;
  }

  getSliceVerticalBounds(depth: number): VerticalBounds | undefined {
    this.updateSliceAndTrackHeight();
    const totalSliceHeight =
      this.sliceLayout.rowGap + this.sliceLayout.sliceHeight;
    const top = this.sliceLayout.padding + depth * totalSliceHeight;
    return {
      top,
      bottom: top + this.sliceLayout.sliceHeight,
    };
  }

  private findSlice({
    x,
    y,
    timescale,
  }: TrackMouseEvent): undefined | SliceWithRow<T> {
    const trackHeight = this.computedTrackHeight;
    const sliceHeight = this.sliceLayout.sliceHeight;
    const padding = this.sliceLayout.padding;
    const rowGap = this.sliceLayout.rowGap;

    if (sliceHeight === 0) {
      return undefined;
    }

    const depth = Math.floor((y - padding) / (sliceHeight + rowGap));

    if (y >= padding && y <= trackHeight - padding) {
      for (const slice of this.slices) {
        if (slice.depth === depth && slice.x <= x && x <= slice.x + slice.w) {
          return slice;
        }
      }
    }

    for (const slice of this.incomplete) {
      const startPx = CROP_INCOMPLETE_SLICE_FLAG.get()
        ? timescale.timeToPx(slice.startNs)
        : slice.x;
      const cropUnfinishedSlicesCondition = CROP_INCOMPLETE_SLICE_FLAG.get()
        ? startPx + INCOMPLETE_SLICE_WIDTH_PX >= x
        : true;

      if (
        slice.depth === depth &&
        startPx <= x &&
        cropUnfinishedSlicesCondition
      ) {
        return slice;
      }
    }

    return undefined;
  }

  onMouseMove(e: TrackMouseEvent): void {
    const prevHoveredSlice = this.hoveredSlice;
    this.hoveredSlice = this.findSlice(e);
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.timeline.highlightedSliceId = this.hoveredSlice?.id;
      if (this.hoveredSlice === undefined) {
        if (this.attrs.onSliceOut) {
          this.attrs.onSliceOut({slice: assertExists(prevHoveredSlice)});
        }
      } else {
        if (this.attrs.onSliceOver) {
          this.attrs.onSliceOver({slice: this.hoveredSlice});
        }
      }
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseOut(): void {
    const prevHoveredSlice = this.hoveredSlice;
    this.hoveredSlice = undefined;
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.timeline.highlightedSliceId = undefined;
      if (this.attrs.onSliceOut && prevHoveredSlice) {
        this.attrs.onSliceOut({slice: prevHoveredSlice});
      }
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseClick(event: TrackMouseEvent): boolean {
    const slice = this.findSlice(event);
    if (slice === undefined) {
      return false;
    }
    if (this.attrs.onSliceClick) {
      this.attrs.onSliceClick({slice});
    } else {
      this.trace.selection.selectTrackEvent(this.uri, slice.id);
    }
    return true;
  }

  getSnapPoint(
    targetTime: time,
    thresholdPx: number,
    timescale: TimeScale,
  ): SnapPoint | undefined {
    const thresholdNs = timescale.pxToDuration(thresholdPx);
    const hpTargetTime = new HighPrecisionTime(targetTime);
    const hpSearchStart = hpTargetTime.addNumber(-thresholdNs);
    const hpSearchEnd = hpTargetTime.addNumber(thresholdNs);
    const searchStart = hpSearchStart.toTime();
    const searchEnd = hpSearchEnd.toTime();

    let closestSnap: SnapPoint | undefined = undefined;
    let closestDistNs = thresholdNs;

    const checkBoundary = (boundaryTime: time) => {
      if (boundaryTime < searchStart || boundaryTime > searchEnd) {
        return;
      }
      const hpBoundary = new HighPrecisionTime(boundaryTime);
      const distNs = Math.abs(hpTargetTime.sub(hpBoundary).toNumber());
      if (distNs < closestDistNs) {
        closestSnap = {time: boundaryTime};
        closestDistNs = distNs;
      }
    };

    for (const slice of this.slices) {
      checkBoundary(slice.ts);
      if (slice.dur > 0n) {
        checkBoundary(Time.add(slice.ts, slice.dur));
      }
    }

    for (const slice of this.incomplete) {
      checkBoundary(slice.ts);
    }

    return closestSnap;
  }

  detailsPanel(sel: TrackEventSelection): TrackEventDetailsPanel | undefined {
    if (this.attrs.detailsPanel) {
      return this.attrs.detailsPanel(sel as unknown as T);
    } else {
      const dataset = getDataset(this.attrs);
      return new SliceTrackDetailsPanel(
        this.trace,
        dataset,
        sel as unknown as T,
      );
    }
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const dataset = getDataset(this.attrs);

    const query = (function () {
      if (dataset.implements({id: NUM})) {
        return dataset.query();
      } else {
        return `
          SELECT
            ROW_NUMBER() OVER (ORDER BY ts) AS id,
            *
          FROM (${dataset.query()})
        `;
      }
    })();

    const result = await this.trace.engine.query(`
      SELECT *
      FROM (${query})
      WHERE id = ${id}
    `);

    const row = result.iter(dataset.schema);
    if (!row.valid()) return undefined;

    const data: {[key: string]: SqlValue} = {};
    for (const col of result.columns()) {
      data[col] = row.get(col);
    }

    return {
      ...data,
      ts: Time.fromRaw(row.ts),
    };
  }

  getTrackShellButtons(): m.Children {
    return this.attrs.shellButtons?.();
  }
}

// Helper functions

export function renderTooltip<T>(
  trace: Trace,
  slice: SliceWithRow<T>,
  opts: {readonly title?: string; readonly extras?: m.Children} = {},
): m.Children {
  const durationFormatted = formatDurationForTooltip(trace, slice);
  const {title = slice.title, extras} = opts;
  return [
    m('', exists(durationFormatted) && m('b', durationFormatted), ' ', title),
    extras,
    slice.count > 1 && m('div', `and ${slice.count - 1} other events`),
  ];
}

function formatDurationForTooltip(
  trace: Trace,
  slice: Slice,
): string | undefined {
  const {dur, flags} = slice;
  if (flags & SLICE_FLAGS_INCOMPLETE) {
    return '[Incomplete]';
  } else if (flags & SLICE_FLAGS_INSTANT) {
    return undefined;
  } else {
    return formatDuration(trace, dur);
  }
}

export function generateRenderQuery<T extends DatasetSchema>(
  dataset: SourceDataset<T>,
): string {
  const hasId = dataset.implements({id: NUM});
  const hasLayer = dataset.implements({layer: NUM});

  const extraCols = Object.fromEntries(
    Object.keys(dataset.schema).map((key) => [key, key]),
  );

  const cols = {
    ...extraCols,
    id: hasId ? 'id' : 'ROW_NUMBER() OVER (ORDER BY ts)',
    ts: 'ts',
    layer: hasLayer ? 'layer' : 0,
    depth: getDepthExpression(dataset),
    dur: getDurExpression(dataset),
  } as const;

  return `SELECT ${Object.entries(cols)
    .map(([key, value]) => `${value} AS ${key}`)
    .join(', ')} FROM (${dataset.query()})`;
}

function getDepthExpression<T extends DatasetSchema>(
  dataset: SourceDataset<T>,
): string {
  const hasDepth = dataset.implements({depth: NUM});
  const hasDur = dataset.implements({dur: LONG});
  const hasNullableDur = dataset.implements({dur: LONG_NULL});

  if (hasDepth) {
    return 'depth';
  } else if (hasDur) {
    return `internal_layout(ts, dur) OVER (ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)`;
  } else if (hasNullableDur) {
    return `internal_layout(ts, COALESCE(dur, -1)) OVER (ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)`;
  } else {
    return '0';
  }
}

function getDurExpression<T extends DatasetSchema>(
  dataset: SourceDataset<T>,
): string {
  const hasDur = dataset.implements({dur: LONG});
  const hasNullableDur = dataset.implements({dur: LONG_NULL});

  if (hasDur) {
    return 'dur';
  } else if (hasNullableDur) {
    return 'COALESCE(dur, -1)';
  } else {
    return '0';
  }
}

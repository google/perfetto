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
import {
  Dataset,
  DatasetSchema,
  SourceDataset,
} from '../../trace_processor/dataset';
import {
  SqlValue,
  LONG,
  NUM,
  LONG_NULL,
} from '../../trace_processor/query_result';
import {
  createPerfettoTable,
  createVirtualTable,
  DisposableSqlEntity,
} from '../../trace_processor/sql_utils';
import {checkerboardExcept} from '../checkerboard';
import {getColorForSlice} from '../colorizer';
import {formatDuration} from '../time_utils';
import {BufferedBounds} from './buffered_bounds';
import {CHUNKED_TASK_BACKGROUND_PRIORITY} from './feature_flags';
import {SliceTrackDetailsPanel} from './slice_track_details_panel';
import {RECT_PATTERN_FADE_RIGHT} from '../../base/renderer';

// Slice flags
export const SLICE_FLAGS_INCOMPLETE = 1;
export const SLICE_FLAGS_INSTANT = 2;

const SLICE_MIN_WIDTH_FOR_TEXT_PX = 5;
const SLICE_MIN_WIDTH_PX = 1;
const SLICE_MIN_WIDTH_FADED_PX = 0.1;
const CHEVRON_WIDTH_PX = 10;

const FADE_THIN_SLICES_FLAG = featureFlags.register({
  id: 'fadeThinSlices',
  name: 'Fade thin slices',
  description: 'Display sub-pixel slices in a faded way',
  defaultValue: false,
});

export const enum ColorVariant {
  BASE = 0,
  VARIANT = 1,
  DISABLED = 2,
}

const VARIANT_FILL: readonly ('base' | 'variant' | 'disabled')[] = [
  'base',
  'variant',
  'disabled',
];

// Columnar storage for slice data. Numeric fields use Float32Array for
// cache-friendly, monomorphic access in the render hot path.
// `ids` uses number[] to preserve full float64 precision for large row IDs.
export interface SliceColumns<T> {
  readonly starts: Float32Array;
  readonly durs: Float32Array;
  readonly ids: readonly number[];
  readonly counts: Float32Array;
  readonly depths: Float32Array;
  readonly titles: readonly string[];
  readonly subTitles: readonly string[];
  readonly colorSchemes: readonly ColorScheme[];
  readonly patterns: Float32Array;
  readonly fillRatios: Float32Array;
  readonly rows: readonly T[];
  readonly length: number;
}

// Reconstruct a single slice object at index i (for callbacks/tooltips).
function sliceAt<T>(cols: SliceColumns<T>, i: number): SliceWithRow<T> {
  return {
    start: cols.starts[i],
    dur: cols.durs[i],
    id: cols.ids[i],
    count: cols.counts[i],
    depth: cols.depths[i],
    title: cols.titles[i],
    subTitle: cols.subTitles[i],
    colorScheme: cols.colorSchemes[i],
    pattern: cols.patterns[i],
    fillRatio: cols.fillRatios[i],
    row: cols.rows[i],
  };
}

// Single slice shape, used for reconstructed objects passed to callbacks.
interface Slice {
  readonly start: number;
  readonly dur: number;
  readonly id: number;
  readonly count: number;
  readonly depth: number;
  readonly title: string;
  readonly subTitle: string;
  readonly colorScheme: ColorScheme;
  readonly pattern: number;
  readonly fillRatio: number;
}

export type SliceWithRow<T> = Slice & {readonly row: T};

interface DataFrame<T> {
  readonly start: time;
  readonly end: time;
  readonly slices: SliceColumns<T>;
}

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

// Callback argument types - use SliceBase to support both complete and incomplete slices
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
   * Called once per render cycle before drawing. Return an array of
   * ColorVariant values (one per slice) to control each slice's color.
   */
  onUpdatedSlices?(slices: SliceColumns<T>): ColorVariant[];

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
  protected hoveredSlice?: SliceWithRow<T & Required<RowSchema>>;

  private readonly attrs: SliceTrackAttrs<T>;
  private readonly instantWidthPx: number;
  private readonly forceTimestampRenderOrder: boolean;

  private readonly queue = new SerialTaskQueue();
  private readonly mipmapTableSlot = new QuerySlot<{
    mipmapTable: DisposableSqlEntity;
    incompleteTable: DisposableSqlEntity;
  }>(this.queue);
  private readonly dataFrameSlot = new QuerySlot<
    DataFrame<T & Required<RowSchema>>
  >(this.queue);
  private readonly bufferedBounds = new BufferedBounds();
  private readonly hoverMonitor = new Monitor([() => this.hoveredSlice?.id]);

  private charWidth = {title: -1, subtitle: -1};
  private computedTrackHeight = 0;
  private currentDataFrame?: DataFrame<T & Required<RowSchema>>;
  private rowCount: number;

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
    this.rowCount = attrs.initialMaxDepth ?? 1;
    this.instantWidthPx = attrs.instantStyle?.width ?? CHEVRON_WIDTH_PX;
    this.forceTimestampRenderOrder = attrs.forceTsRenderOrder ?? false;

    const sliceLayout = attrs.sliceLayout ?? {};
    this.sliceLayout = {
      padding: sliceLayout.padding ?? 3,
      rowGap: sliceLayout.rowGap ?? 0,
      sliceHeight: sliceLayout.sliceHeight ?? 18,
      titleSizePx: sliceLayout.titleSizePx ?? 12,
      subtitleSizePx: sliceLayout.subtitleSizePx ?? 10,
    };
  }

  render(trackCtx: TrackRenderContext): void {
    const {ctx, size, timescale, colors, renderer} = trackCtx;

    const dataFrame = this.useData(trackCtx);
    this.currentDataFrame = dataFrame;
    if (!dataFrame) {
      return;
    }

    // Allow callbacks to compute per-slice color variants
    const colorVariants = this.onUpdatedSlices(dataFrame.slices);

    const charWidth = this.measureCharWidth(ctx);

    const selection = this.trace.selection.selection;
    const selectedId =
      selection.kind === 'track_event' && selection.trackUri === this.uri
        ? selection.eventId
        : undefined;

    let discoveredSelectionIdx = -1;

    const sliceHeight = this.sliceLayout.sliceHeight;
    const padding = this.sliceLayout.padding;
    const rowSpacing = this.sliceLayout.rowGap;
    const pxEnd = size.width;
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(dataFrame.start);

    // Destructure columnar arrays for direct access in hot loops
    const cols = dataFrame.slices;
    const n = cols.length;
    const {
      starts,
      durs,
      ids,
      depths,
      titles,
      subTitles,
      colorSchemes,
      patterns,
      fillRatios,
    } = cols;

    // Calculate each slice's pixel geometry and collect visible indices
    const xs = new Float32Array(n);
    const ws = new Float32Array(n);
    for (let i = 0; i < n; ++i) {
      const dur = durs[i];
      let x = starts[i] * pxPerNs + baseOffsetPx;
      let w: number;

      if (dur === -1) {
        x = Math.max(x, -1);
        w = pxEnd - x;
      } else if (dur === 0) {
        x -= this.instantWidthPx / 2;
        w = this.instantWidthPx;
      } else {
        w = dur * pxPerNs;
        const sliceVizLimit = Math.min(x + w, pxEnd);
        x = Math.max(x, -1);
        w = sliceVizLimit - x;
      }
      xs[i] = x;
      ws[i] = w;
    }

    // First pass: draw slice fills
    const minSliceWidthPx = FADE_THIN_SLICES_FLAG.get()
      ? SLICE_MIN_WIDTH_FADED_PX
      : SLICE_MIN_WIDTH_PX;
    const indiciesWithText: number[] = [];
    for (let i = 0; i < n; i++) {
      const color = colorSchemes[i][VARIANT_FILL[colorVariants[i]]];
      const y = padding + depths[i] * (sliceHeight + rowSpacing);
      const w = ws[i];
      const x = xs[i];

      // Skip any slices that are completely offscreen
      if (x + w <= 0 || x >= pxEnd) {
        continue;
      }

      if (durs[i] === 0) {
        // Instant slice - draw chevron
        renderer.drawMarker(x, y, w, sliceHeight, color, () =>
          this.drawChevron(ctx, x, y, sliceHeight),
        );
      } else {
        // Normal slice
        const drawW = Math.max(w, minSliceWidthPx);
        renderer.drawRect(x, y, x + drawW, y + sliceHeight, color, patterns[i]);

        if (w >= SLICE_MIN_WIDTH_FOR_TEXT_PX) {
          indiciesWithText.push(i);
        }
      }

      if (selectedId === ids[i]) {
        discoveredSelectionIdx = i;
      }
    }

    renderer.flush();

    // Draw fillRatio light sections
    ctx.fillStyle = `#FFFFFF50`;
    for (let i = 0; i < n; i++) {
      const w = ws[i];
      if (w < 2) continue;
      const x = xs[i];

      // Skip any slices that are completely offscreen
      if (x + w <= 0 || x >= pxEnd) {
        continue;
      }

      if (durs[i] === 0) continue; // Skip instants

      const fillRatio = clamp(fillRatios[i], 0, 1);
      if (floatEqual(fillRatio, 1)) continue;

      const sliceDrawWidth = Math.max(w, SLICE_MIN_WIDTH_PX);
      const lightSectionDrawWidth = sliceDrawWidth * (1 - fillRatio);
      if (lightSectionDrawWidth < 1) continue;

      const y = padding + depths[i] * (sliceHeight + rowSpacing);
      const lightX = x + (sliceDrawWidth - lightSectionDrawWidth);
      ctx.fillRect(lightX, y, lightSectionDrawWidth, sliceHeight);
    }

    // Draw titles
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.font = this.getTitleFont();
    for (const i of indiciesWithText) {
      const title = titles[i];
      if (!title) continue;

      const w = ws[i];
      const x = xs[i];
      const cv = colorVariants[i];
      const cs = colorSchemes[i];

      const textColor =
        cv === ColorVariant.BASE
          ? cs.textBase
          : cv === ColorVariant.VARIANT
            ? cs.textVariant
            : cs.textDisabled;
      ctx.fillStyle = textColor.cssString;

      const rectXCenter = x + w / 2;
      const titleCropped = cropText(title, charWidth.title, w);
      const y = padding + depths[i] * (sliceHeight + rowSpacing);
      const yDiv = subTitles[i] ? 3 : 2;
      const yMidPoint = Math.floor(y + sliceHeight / yDiv) + 0.5;
      ctx.fillText(titleCropped, rectXCenter, yMidPoint);
    }

    // Draw subtitles
    ctx.font = this.getSubtitleFont();
    for (const i of indiciesWithText) {
      const subTitle = subTitles[i];
      if (!subTitle) continue;

      const w = ws[i];
      const x = xs[i];
      const cv = colorVariants[i];
      const cs = colorSchemes[i];

      const textColor =
        cv === ColorVariant.BASE
          ? cs.textBase
          : cv === ColorVariant.VARIANT
            ? cs.textVariant
            : cs.textDisabled;
      ctx.fillStyle = textColor.cssString;

      const rectXCenter = x + w / 2;
      const subTitleCropped = cropText(subTitle, charWidth.subtitle, w);
      const y = padding + depths[i] * (sliceHeight + rowSpacing);
      const yMidPoint = Math.ceil(y + (sliceHeight * 2) / 3) + 1.5;
      ctx.fillText(subTitleCropped, rectXCenter, yMidPoint);
    }

    // Draw selection highlight
    if (discoveredSelectionIdx >= 0) {
      const w = ws[discoveredSelectionIdx];
      const x = xs[discoveredSelectionIdx];
      const y =
        padding + depths[discoveredSelectionIdx] * (sliceHeight + rowSpacing);
      ctx.strokeStyle = colors.COLOR_TIMELINE_OVERLAY;
      ctx.beginPath();
      const THICKNESS = 3;
      ctx.lineWidth = THICKNESS;
      ctx.strokeRect(x, y - THICKNESS / 2, w, sliceHeight + THICKNESS);
      ctx.closePath();
    }

    // Checkerboard for loading areas
    const frameStartPx = timescale.timeToPx(dataFrame.start);
    const frameEndPx = timescale.timeToPx(dataFrame.end);
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      frameStartPx,
      frameEndPx,
    );
  }

  private measureCharWidth(ctx: CanvasRenderingContext2D) {
    const charWidth = this.charWidth;
    if (charWidth.title < 0) {
      ctx.font = this.getTitleFont();
      charWidth.title = ctx.measureText('dbpqaouk').width / 8;
      ctx.font = this.getSubtitleFont();
      charWidth.subtitle = ctx.measureText('dbpqaouk').width / 8;
    }
    return charWidth;
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

  // Creates the mipmap table for efficient slice queries
  // Also pre-computes incomplete slices with their next_ts
  private async createMipmapTable(sqlSource: string) {
    const rowCount = await this.getRowCount(sqlSource);
    this.rowCount = rowCount;

    const mipmapTable = await createVirtualTable({
      engine: this.engine,
      using: `__intrinsic_slice_mipmap((
        select id, ts, dur, ((layer * ${rowCount ?? 1}) + depth) as depth
        from (${sqlSource})
        where dur != -1
      ))`,
    });

    // Pre-compute incomplete slices with LEAD() to find next_ts
    // We compute LEAD over ALL slices first, then filter to incomplete ones
    // This ensures next_ts is the next slice at the same depth (complete or incomplete)
    const incompleteTable = await createPerfettoTable({
      engine: this.engine,
      as: `
        SELECT id, ts, depth, next_ts
        FROM (
          SELECT id, ts, dur, depth, LEAD(ts) OVER (PARTITION BY depth ORDER BY ts) as next_ts
          FROM (${sqlSource})
        )
        WHERE dur = -1
      `,
    });

    return {mipmapTable, incompleteTable};
  }

  private async getRowCount(sqlSource: string): Promise<number> {
    const result = await this.engine.query(`
      SELECT
        IFNULL(depth, 0) + 1 AS rowCount
      FROM (${sqlSource})
      ORDER BY depth DESC
      LIMIT 1
    `);
    return result.maybeFirstRow({rowCount: NUM})?.rowCount ?? 0;
  }

  private useData(
    trackCtx: TrackRenderContext,
  ): DataFrame<T & Required<RowSchema>> | undefined {
    const {resolution, visibleWindow} = trackCtx;

    const dataset = this.getDataset();
    const sqlSource = generateRenderQuery(dataset);

    // 1. Create the mipmap and incomplete tables, which only depend on the SQL source.
    const {data: tables} = this.mipmapTableSlot.use({
      key: {sqlSource},
      queryFn: () => this.createMipmapTable(sqlSource),
    });

    // Can't do anything until we have the tables.
    if (!tables) return undefined;

    // 2. Load the slices into a dataframe based on the visible window and
    // resolution, which can change every frame.
    const visibleSpan = visibleWindow.toTimeSpan();
    const bounds = this.bufferedBounds.update(visibleSpan, resolution);

    const {data: dataFrame} = this.dataFrameSlot.use({
      key: {
        start: bounds.start,
        end: bounds.end,
        resolution: bounds.resolution,
      },
      queryFn: async (signal) => {
        const promise = (async () => {
          // Load complete and incomplete slices in a single query
          const slices = await this.getSlices(
            tables.mipmapTable.name,
            tables.incompleteTable.name,
            bounds.start,
            bounds.end,
            bounds.resolution,
            signal,
            dataset,
          );

          return {
            start: bounds.start,
            end: bounds.end,
            slices,
          };
        })();
        const result = await this.trace.taskTracker.track(
          promise,
          'Loading slices',
        );
        this.trace.raf.scheduleFullRedraw();
        return result;
      },
      retainOn: ['start', 'end', 'resolution'],
    });

    return dataFrame;
  }

  private async getSlices(
    mipmapTableName: string,
    incompleteTableName: string,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
    dataset: Dataset<T>,
  ): Promise<SliceColumns<T & Required<RowSchema>>> {
    const sqlSource = generateRenderQuery(dataset as SourceDataset<T>);
    const extraCols = Object.keys(dataset.schema)
      .map((c) => `s.${c} as ${c}`)
      .join(',');

    // Query complete slices from mipmap + incomplete slices in one query
    // Incomplete slices use pre-computed next_ts from incompleteTableName
    const queryRes = await this.engine.query(`
      -- Complete slices from mipmap
      SELECT
        ((z.ts / ${resolution}) * ${resolution}) - ${start} as __ts,
        ((z.dur + ${resolution - 1n}) / ${resolution}) * ${resolution} as __dur,
        s.id as __id,
        z.count as __count,
        s.depth as __depth,
        0 as __incomplete,
        ${extraCols}
      FROM ${mipmapTableName}(
        ${start},
        ${end},
        ${resolution}
      ) z
      CROSS JOIN (${sqlSource}) s using (id)
      UNION ALL
      -- Incomplete slices with pre-computed next_ts
      SELECT
        MAX(i.ts, ${start}) - ${start} as __ts,
        CASE
          WHEN i.next_ts IS NOT NULL AND i.next_ts <= ${end}
          THEN i.next_ts - MAX(i.ts, ${start})
          ELSE -1
        END as __dur,
        s.id as __id,
        1 as __count,
        i.depth as __depth,
        1 as __incomplete,
        ${extraCols}
      FROM ${incompleteTableName} i
      JOIN (${sqlSource}) s ON i.id = s.id
      WHERE i.ts < ${end}
    `);

    if (signal.isCancelled) throw QUERY_CANCELLED;

    const priority = CHUNKED_TASK_BACKGROUND_PRIORITY.get()
      ? 'background'
      : undefined;
    const task = await deferChunkedTask({priority});

    const it = queryRes.iter({
      __id: NUM,
      __ts: NUM,
      __dur: NUM,
      __count: NUM,
      __depth: NUM,
      __incomplete: NUM,
      ...dataset.schema,
    });

    // Build temporary arrays (need sorting before typed-array extraction)
    const tmpStarts: number[] = [];
    const tmpDurs: number[] = [];
    const tmpIds: number[] = [];
    const tmpCounts: number[] = [];
    const tmpDepths: number[] = [];
    const tmpTitles: string[] = [];
    const tmpSubTitles: string[] = [];
    const tmpColorSchemes: ColorScheme[] = [];
    const tmpPatterns: number[] = [];
    const tmpFillRatios: number[] = [];
    const tmpRows: (T & Required<RowSchema>)[] = [];

    for (let i = 0; it.valid(); it.next(), ++i) {
      if (i % 32 === 0) {
        if (signal.isCancelled) throw QUERY_CANCELLED;
        if (task.shouldYield()) await task.yield();
      }

      // Clone the raw row with only schema keys from the dataset
      const row: Record<string, SqlValue> = {};
      // eslint-disable-next-line guard-for-in
      for (const k in dataset.schema) {
        row[k] = it[k];
      }

      const title = this.getTitle(it as unknown as T);
      const subTitle = this.getSubtitle(it as unknown as T);
      const colorScheme = this.getColor(it as unknown as T, title);
      const isIncomplete = it.__incomplete === 1;

      tmpStarts.push(it.__ts);
      tmpDurs.push(it.__dur);
      tmpIds.push(it.__id);
      tmpCounts.push(it.__count);
      tmpDepths.push(it.__depth);
      tmpTitles.push(title);
      tmpSubTitles.push(subTitle);
      tmpColorSchemes.push(colorScheme);
      tmpPatterns.push(
        isIncomplete
          ? RECT_PATTERN_FADE_RIGHT
          : this.attrs.slicePattern?.(it as unknown as T) ?? 0,
      );
      tmpFillRatios.push(this.attrs.fillRatio?.(it as unknown as T) ?? 1);
      tmpRows.push(row as T & Required<RowSchema>);
    }

    // Sort by color for batch rendering (unless forced ts order).
    // Build a sort index so all columns stay in sync.
    const n = tmpStarts.length;
    const idx = Array.from({length: n}, (_, i) => i);
    if (!this.forceTimestampRenderOrder) {
      idx.sort((a, b) =>
        colorCompare(tmpColorSchemes[a].base, tmpColorSchemes[b].base),
      );
    }

    // Extract into typed arrays in sorted order
    const starts = new Float32Array(n);
    const durs = new Float32Array(n);
    const ids: number[] = new Array(n);
    const counts = new Float32Array(n);
    const depths = new Float32Array(n);
    const titles: string[] = new Array(n);
    const subTitles: string[] = new Array(n);
    const colorSchemes: ColorScheme[] = new Array(n);
    const patterns = new Float32Array(n);
    const fillRatios = new Float32Array(n);
    const rows: (T & Required<RowSchema>)[] = new Array(n);
    for (let i = 0; i < n; i++) {
      const j = idx[i];
      starts[i] = tmpStarts[j];
      durs[i] = tmpDurs[j];
      ids[i] = tmpIds[j];
      counts[i] = tmpCounts[j];
      depths[i] = tmpDepths[j];
      titles[i] = tmpTitles[j];
      subTitles[i] = tmpSubTitles[j];
      colorSchemes[i] = tmpColorSchemes[j];
      patterns[i] = tmpPatterns[j];
      fillRatios[i] = tmpFillRatios[j];
      rows[i] = tmpRows[j];
    }

    return {
      starts,
      durs,
      ids,
      counts,
      depths,
      titles,
      subTitles,
      colorSchemes,
      patterns,
      fillRatios,
      rows,
      length: n,
    };
  }

  private getTitle(row: T): string {
    if (this.attrs.sliceName) return this.attrs.sliceName(row);
    if ('name' in row && typeof row.name === 'string') return row.name;
    return '';
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

  private onUpdatedSlices(
    slices: SliceColumns<T & Required<RowSchema>>,
  ): ColorVariant[] {
    if (this.attrs.onUpdatedSlices) {
      return this.attrs.onUpdatedSlices(slices);
    } else {
      return this.highlightHoveredAndSameTitle(slices);
    }
  }

  protected highlightHoveredAndSameTitle(
    slices: SliceColumns<T & Required<RowSchema>>,
  ): ColorVariant[] {
    const highlightedSliceId = this.trace.timeline.highlightedSliceId;
    const hoveredTitle = this.hoveredSlice?.title;
    const isHovering =
      hoveredTitle !== undefined || highlightedSliceId !== undefined;
    const n = slices.length;
    const variants = new Array<ColorVariant>(n);
    const {ids, titles} = slices;
    for (let i = 0; i < n; i++) {
      if (!isHovering) {
        variants[i] = ColorVariant.BASE;
      } else {
        const isMatch =
          highlightedSliceId === ids[i] ||
          (hoveredTitle !== undefined && hoveredTitle === titles[i]);
        variants[i] = isMatch ? ColorVariant.BASE : ColorVariant.DISABLED;
      }
    }
    return variants;
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

  private updateSliceAndTrackHeight() {
    // maxDataDepth is 0-indexed, so add 1 to get the number of rows
    const {padding = 2, sliceHeight = 12, rowGap = 0} = this.sliceLayout;
    this.computedTrackHeight =
      2 * padding + this.rowCount * (sliceHeight + rowGap);
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
  }: TrackMouseEvent): undefined | SliceWithRow<T & Required<RowSchema>> {
    if (!this.currentDataFrame) return undefined;

    const trackHeight = this.computedTrackHeight;
    const sliceHeight = this.sliceLayout.sliceHeight;
    const padding = this.sliceLayout.padding;
    const rowGap = this.sliceLayout.rowGap;

    if (sliceHeight === 0) {
      return undefined;
    }

    const depth = Math.floor((y - padding) / (sliceHeight + rowGap));
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(this.currentDataFrame.start);

    if (y >= padding && y <= trackHeight - padding) {
      const cols = this.currentDataFrame.slices;
      const {starts, durs, depths} = cols;
      const n = cols.length;
      for (let i = 0; i < n; i++) {
        if (depths[i] !== depth) continue;

        const sliceX = starts[i] * pxPerNs + baseOffsetPx;

        if (durs[i] === -1) {
          // Incomplete slice extends to the end of the window
          if (sliceX <= x) {
            return sliceAt(cols, i);
          }
        } else {
          const sliceW = durs[i] * pxPerNs;
          if (sliceX <= x && x <= sliceX + sliceW) {
            return sliceAt(cols, i);
          }
        }
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
    if (!this.currentDataFrame) return undefined;

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

    const frameStart = this.currentDataFrame.start;
    const {starts, durs, length: n} = this.currentDataFrame.slices;
    for (let i = 0; i < n; i++) {
      // Convert relative start to absolute time
      const sliceStart = Time.add(frameStart, BigInt(starts[i]));
      checkBoundary(sliceStart);
      // Incomplete slices (dur = -1) have no end to snap to
      if (durs[i] > 0) {
        const sliceEnd = Time.add(frameStart, BigInt(starts[i] + durs[i]));
        checkBoundary(sliceEnd);
      }
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

export function renderTooltip(
  trace: Trace,
  slice: SliceWithRow<{dur: bigint | null}>,
  opts: {readonly title?: string; readonly extras?: m.Children} = {},
): m.Children {
  const durationFormatted = formatDurationForTooltip(trace, slice.row.dur);
  const {title = slice.title, extras} = opts;
  return [
    m('', exists(durationFormatted) && m('b', durationFormatted), ' ', title),
    extras,
    slice.count > 1 && m('div', `and ${slice.count - 1} other events`),
  ];
}

function formatDurationForTooltip(
  trace: Trace,
  dur: bigint | null,
): string | undefined {
  if (dur === -1n) {
    return '[Incomplete]';
  }
  if (dur === null || dur === 0n) {
    return undefined; // Instant event
  }
  return formatDuration(trace, BigInt(dur));
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

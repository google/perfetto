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
import {ColorScheme} from '../../base/color_scheme';
import {Point2D, Size2D, Transform2D, VerticalBounds} from '../../base/geom';
import {assertExists} from '../../base/assert';
import {Monitor} from '../../base/monitor';
import {
  CancellationSignal,
  QuerySlot,
  QUERY_CANCELLED,
  SerialTaskQueue,
} from '../../base/query_slot';
import {duration, Time, time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {exists} from '../../base/utils';
import {deferChunkedTask} from '../../base/chunked_task';
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
  DisposableSqlEntity,
} from '../../trace_processor/sql_utils';
import {checkerboardExcept} from '../checkerboard';
import {getColorForSlice} from '../colorizer';
import {formatDuration} from '../time_utils';
import {BufferedBounds} from './buffered_bounds';
import {CHUNKED_TASK_BACKGROUND_PRIORITY} from './feature_flags';
import {SliceTrackDetailsPanel} from './slice_track_details_panel';
import {RECT_PATTERN_FADE_RIGHT} from '../../base/renderer';
import {cropText} from '../../base/string_utils';

const SLICE_MIN_WIDTH_FOR_TEXT_PX = 5;
const CHEVRON_WIDTH_PX = 10;

export const enum ColorVariant {
  BASE = 0,
  VARIANT = 1,
  DISABLED = 2,
}

interface Slice<T> {
  readonly id: number;
  readonly title: string;
  readonly subtitle: string;
  readonly count: number; // Number of slices in this bucket
  readonly colorScheme: ColorScheme;
  readonly fillRatio: number;
  readonly row: T; // The raw dataset row
}

interface SliceBuffers<T> {
  readonly xs: Float32Array;
  readonly ys: Float32Array;
  readonly ws: Float32Array;
  readonly patterns: Uint8Array;
  readonly slices: readonly Slice<T>[];
  readonly count: number;
}

interface Instant<T> {
  readonly id: number;
  readonly title: string;
  readonly subtitle: string;
  readonly count: number; // Number of slices in this bucket
  readonly colorScheme: ColorScheme;
  readonly row: T; // The raw dataset row
}

interface InstantBuffers<T> {
  readonly xs: Float32Array;
  readonly ys: Float32Array;
  readonly instants: readonly Instant<T>[];
  readonly count: number;
}

interface DataFrame<T> {
  readonly start: time;
  readonly end: time;
  readonly slices: SliceBuffers<T>;
  readonly instants: InstantBuffers<T>;
}

type SliceOrInstant<T> = Slice<T> | Instant<T>;

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
export interface OnSliceOverArgs<T> {
  slice: SliceOrInstant<T>;
  tooltip?: string[];
}

export interface OnSliceOutArgs<T> {
  slice: SliceOrInstant<T>;
}

export interface OnSliceClickArgs<T> {
  slice: SliceOrInstant<T>;
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
  tooltip?(slice: SliceOrInstant<T>): m.Children;

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
  onUpdatedSlices?(slices: readonly SliceOrInstant<T>[]): ColorVariant[];

  /**
   * Called when a slice is hovered.
   */
  onSliceOver?(args: OnSliceOverArgs<T>): void;

  /**
   * Called when hover leaves a slice.
   */
  onSliceOut?(args: OnSliceOutArgs<T>): void;

  /**
   * Called when a slice is clicked. Return false to prevent default selection.
   */
  onSliceClick?(args: OnSliceClickArgs<T>): void;
}

interface Tables extends AsyncDisposable {
  readonly slicesMipmapTable: DisposableSqlEntity;
  readonly instantsMipmapTable: DisposableSqlEntity;
  readonly incompleteSlicesTable: DisposableSqlEntity;
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
  private readonly trace: Trace;
  private readonly uri: string;
  private readonly sliceLayout: SliceLayout;
  private readonly attrs: SliceTrackAttrs<T>;
  private readonly instantWidthPx: number;
  private readonly queue = new SerialTaskQueue();
  private readonly tablesSlot = new QuerySlot<Tables>(this.queue);
  private readonly dataFrameSlot = new QuerySlot<DataFrame<T>>(this.queue);
  private readonly bufferedBounds = new BufferedBounds();
  private readonly hoverMonitor = new Monitor([() => this.hoveredSlice?.id]);

  private hoveredSlice?: SliceOrInstant<T>;
  private charWidth = {title: -1, subtitle: -1};
  private computedTrackHeight = 0;
  private currentDataFrame?: DataFrame<T>;
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
    if (attrs.initialMaxDepth !== undefined) {
      // Row count is max depth + 1
      this.rowCount = attrs.initialMaxDepth + 1;
    } else {
      // Assume at least one row
      this.rowCount = 1;
    }
    this.instantWidthPx = attrs.instantStyle?.width ?? CHEVRON_WIDTH_PX;

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
    const {ctx, size, timescale} = trackCtx;

    // Query for new data given the current state or reuse cache
    const dataFrame = this.useData(trackCtx);

    // Cache the current data frame for use in event handlers
    this.currentDataFrame = dataFrame;

    // If we have no data, we can't render anything
    if (!dataFrame) return;

    const sliceHeight = this.sliceLayout.sliceHeight;
    const pxEnd = size.width;
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(dataFrame.start);
    const charWidth = this.measureCharWidth(ctx);
    const selection = this.trace.selection.selection;
    const selectedId =
      selection.kind === 'track_event' && selection.trackUri === this.uri
        ? selection.eventId
        : undefined;

    const dataTransform: Transform2D = {
      scaleX: pxPerNs,
      offsetX: baseOffsetPx,
      scaleY: this.sliceLayout.sliceHeight + this.sliceLayout.rowGap,
      offsetY: this.sliceLayout.padding,
    };

    this.renderSlices(
      trackCtx,
      dataFrame.slices,
      dataTransform,
      sliceHeight,
      pxEnd,
      pxPerNs,
      baseOffsetPx,
      charWidth,
      selectedId,
    );

    // Render instants after slices so they appear on top
    this.renderInstants(
      trackCtx,
      dataFrame.instants,
      dataTransform,
      sliceHeight,
      pxPerNs,
      baseOffsetPx,
      selectedId,
    );

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

  private renderSlices(
    trackCtx: TrackRenderContext,
    sliceBuffers: SliceBuffers<T>,
    dataTransform: Transform2D,
    sliceHeight: number,
    pxEnd: number,
    pxPerNs: number,
    baseOffsetPx: number,
    charWidth: {title: number; subtitle: number},
    selectedId: number | undefined,
  ): void {
    const {ctx, renderer} = trackCtx;
    const {xs, ys, ws, patterns, slices, count} = sliceBuffers;

    // Collect text labels to render in a second pass
    const textLabels: Array<{
      title: string;
      subTitle: string;
      textColor: string;
      rectXCenter: number;
      titleY: number;
      subTitleY: number;
    }> = [];

    // Recreate the colors array every time as this could have changed
    // TODO(stevegolton): Find a way to avoid having to do this every frame.
    const colorVariants = this.onUpdatedSlices(slices);
    const colors = new Uint32Array(count);
    let selectedIdx = -1;

    for (let j = 0; j < count; j++) {
      const slice = slices[j];
      const colorVariant = colorVariants[j];
      const cs = slice.colorScheme;
      const color =
        colorVariant === ColorVariant.BASE
          ? cs.base
          : colorVariant === ColorVariant.VARIANT
            ? cs.variant
            : cs.disabled;
      colors[j] = color.rgba;

      // Track selected slice index
      if (selectedId !== undefined && slice.id === selectedId) {
        selectedIdx = j;
      }

      // Collect text labels
      const w = ws[j];
      const wPx = w * pxPerNs;

      // Skip slices that are too narrow to show text
      if (wPx < SLICE_MIN_WIDTH_FOR_TEXT_PX) continue;

      const x = xs[j];
      const xPx = x * pxPerNs + baseOffsetPx;

      // Skip slices that are completely offscreen
      if (xPx + wPx <= 0 || xPx >= pxEnd) continue;

      // Collect text label if wide enough (using screen-space width)
      const y = ys[j] * dataTransform.scaleY + dataTransform.offsetY;
      const title = slice.title;
      const subTitle = slice.subtitle;
      if (title || subTitle) {
        const textColor =
          colorVariant === ColorVariant.BASE
            ? cs.textBase
            : colorVariant === ColorVariant.VARIANT
              ? cs.textVariant
              : cs.textDisabled;

        // Clamp slice bounds to visible window for text positioning
        const clampedLeft = Math.max(xPx, 0);
        const clampedRight = Math.min(xPx + wPx, pxEnd);
        const clampedW = clampedRight - clampedLeft;
        const rectXCenter = clampedLeft + clampedW / 2;
        const yCenter = sliceHeight / 2;
        const titleOffset = subTitle ? -4 : 1; // Move title up if there's a subtitle
        const titleY = Math.floor(y + yCenter) + titleOffset;
        const subTitleY = Math.floor(y + yCenter) + 6;

        textLabels.push({
          title: cropText(title, charWidth.title, clampedW),
          subTitle: cropText(subTitle, charWidth.subtitle, clampedW),
          textColor: textColor.cssString,
          rectXCenter,
          titleY,
          subTitleY,
        });
      }
    }

    renderer.drawRects(
      {
        xs,
        ys,
        ws,
        h: sliceHeight,
        colors,
        count,
        patterns,
      },
      dataTransform,
    );

    // Draw text labels
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (const label of textLabels) {
      ctx.fillStyle = label.textColor;
      if (label.title) {
        ctx.font = this.getTitleFont();
        ctx.fillText(label.title, label.rectXCenter, label.titleY);
      }
      if (label.subTitle) {
        ctx.globalAlpha = 0.6; // Slightly fade subtitles for visual hierarchy
        ctx.font = this.getSubtitleFont();
        ctx.fillText(label.subTitle, label.rectXCenter, label.subTitleY);
        ctx.globalAlpha = 1;
      }
    }

    // Draw selection highlight
    if (selectedIdx !== -1) {
      const selX = xs[selectedIdx] * pxPerNs + baseOffsetPx;
      const selW = ws[selectedIdx] * pxPerNs;
      const selY =
        ys[selectedIdx] * dataTransform.scaleY + dataTransform.offsetY;
      const THICKNESS = 3;
      ctx.strokeStyle = trackCtx.colors.COLOR_TIMELINE_OVERLAY;
      ctx.lineWidth = THICKNESS;
      ctx.strokeRect(selX, selY - THICKNESS / 2, selW, sliceHeight + THICKNESS);
    }
  }

  private renderInstants(
    trackCtx: TrackRenderContext,
    instantBuffers: InstantBuffers<T>,
    dataTransform: Transform2D,
    sliceHeight: number,
    pxPerNs: number,
    baseOffsetPx: number,
    selectedId: number | undefined,
  ): void {
    const {ctx, renderer} = trackCtx;
    const {xs, ys, instants, count} = instantBuffers;

    // Recreate the colors array every time as this could have changed
    // TODO(stevegolton): Find a way to avoid having to do this every frame.
    const colorVariants = this.onUpdatedSlices(instants);
    const colors = new Uint32Array(count);
    let selectedIdx = -1;

    for (let j = 0; j < count; j++) {
      const instant = instants[j];
      const colorVariant = colorVariants[j];
      const cs = instant.colorScheme;
      const color =
        colorVariant === ColorVariant.BASE
          ? cs.base
          : colorVariant === ColorVariant.VARIANT
            ? cs.variant
            : cs.disabled;
      colors[j] = color.rgba;

      // Track selected instant index
      if (selectedId !== undefined && instant.id === selectedId) {
        selectedIdx = j;
      }
    }

    renderer.drawMarkers(
      {
        xs,
        ys,
        w: this.instantWidthPx,
        h: sliceHeight,
        colors,
        count,
      },
      dataTransform,
      (ctx, x, y, _w, h) => this.drawChevron(ctx, x, y, h),
    );

    // Draw selection highlight for instants
    if (selectedIdx !== -1) {
      const selX =
        xs[selectedIdx] * pxPerNs + baseOffsetPx - this.instantWidthPx / 2;
      const selY =
        ys[selectedIdx] * dataTransform.scaleY + dataTransform.offsetY;
      const THICKNESS = 3;
      ctx.strokeStyle = trackCtx.colors.COLOR_TIMELINE_OVERLAY;
      ctx.lineWidth = THICKNESS;
      ctx.strokeRect(
        selX,
        selY - THICKNESS / 2,
        this.instantWidthPx,
        sliceHeight + THICKNESS,
      );
    }
  }

  getDataset() {
    return getDataset(this.attrs);
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
  private async createTables(sqlSource: string): Promise<Tables> {
    const engine = this.trace.engine;

    const rowCount = await this.getRowCount(sqlSource);
    this.rowCount = rowCount;

    const slicesMipmapTable = await createVirtualTable({
      engine,
      using: `__intrinsic_slice_mipmap((
        select id, ts, dur, ((layer * ${rowCount ?? 1}) + depth) as depth
        from (${sqlSource})
        where dur > 0
      ))`,
    });

    const instantsMipmapTable = await createVirtualTable({
      engine,
      using: `__intrinsic_slice_mipmap((
        select id, ts, dur, ((layer * ${rowCount ?? 1}) + depth) as depth
        from (${sqlSource})
        where dur = 0
      ))`,
    });

    // Pre-compute incomplete slices with LEAD() to find next_ts
    // We compute LEAD over ALL slices first, then filter to incomplete ones
    // This ensures next_ts is the next slice at the same depth (complete or incomplete)
    const incompleteSlicesTable = await createPerfettoTable({
      engine,
      as: `
        SELECT id, ts, depth, next_ts
        FROM (
          SELECT id, ts, dur, depth, LEAD(ts) OVER (PARTITION BY depth ORDER BY ts) as next_ts
          FROM (${sqlSource})
        )
        WHERE dur = -1
      `,
    });

    return {
      slicesMipmapTable,
      instantsMipmapTable,
      incompleteSlicesTable,
      [Symbol.asyncDispose]: async () => {
        await slicesMipmapTable[Symbol.asyncDispose]();
        await instantsMipmapTable[Symbol.asyncDispose]();
        await incompleteSlicesTable[Symbol.asyncDispose]();
      },
    };
  }

  private async getRowCount(sqlSource: string): Promise<number> {
    const engine = this.trace.engine;
    const result = await engine.query(`
      SELECT
        IFNULL(depth, 0) + 1 AS rowCount
      FROM (${sqlSource})
      ORDER BY depth DESC
      LIMIT 1
    `);
    return result.maybeFirstRow({rowCount: NUM})?.rowCount ?? 0;
  }

  private useData(trackCtx: TrackRenderContext): DataFrame<T> | undefined {
    const {resolution, visibleWindow} = trackCtx;

    const dataset = this.getDataset();
    const sqlSource = generateRenderQuery(dataset);

    // 1. Create the mipmap tables which only depend on the sql query source
    const {data: tables} = this.tablesSlot.use({
      key: {sqlSource},
      queryFn: () => this.createTables(sqlSource),
    });

    // Can't do anything until we have the tables.
    if (!tables) return undefined;

    // 2. Load the slices into a data frame based on the visible window and
    // resolution, which could change every frame.
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
          const instants = await this.getInstantBuffers(
            tables.instantsMipmapTable.name,
            bounds.start,
            bounds.end,
            bounds.resolution,
            signal,
            dataset,
          );

          const slices = await this.getSliceBuffers(
            tables.slicesMipmapTable.name,
            tables.incompleteSlicesTable.name,
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
            instants,
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

  private async getInstantBuffers(
    mipmapTableName: string,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
    dataset: SourceDataset<T>,
  ): Promise<InstantBuffers<T>> {
    const sqlSource = generateRenderQuery(dataset);
    const extraCols = Object.keys(dataset.schema)
      .map((c) => `s.${c} as ${c}`)
      .join(',');

    const queryResult = await this.engine.query(`
      SELECT
        s.id as __id,
        ((z.ts / ${resolution}) * ${resolution}) - ${start} as __ts,
        z.count as __count,
        s.depth as __depth,
        ${extraCols}
      FROM ${mipmapTableName}(
        ${start},
        ${end},
        ${resolution}
      ) z
      CROSS JOIN (${sqlSource}) s using (id)
    `);

    if (signal.isCancelled) throw QUERY_CANCELLED;
    const task = await this.deferChunkedTask();

    // Initialize buffersx
    const count = queryResult.numRows();
    const xs = new Float32Array(count);
    const ys = new Float32Array(count);
    const instants = new Array<Instant<T>>(count);

    const it = queryResult.iter({
      __id: NUM,
      __ts: NUM,
      __count: NUM,
      __depth: NUM,
      ...dataset.schema,
    });

    for (let i = 0; it.valid(); it.next(), ++i) {
      if (i % 64 === 0) {
        if (signal.isCancelled) throw QUERY_CANCELLED;
        if (task.shouldYield()) await task.yield();
      }

      const id = it.__id;
      const ts = it.__ts;
      const count = it.__count;
      const depth = it.__depth;
      const title = this.getTitle(it);
      const subtitle = this.getSubtitle(it);
      const colorScheme = this.getColor(it, title);
      const row = this.extractKeys(it, dataset.schema);

      xs[i] = ts;
      ys[i] = depth;
      instants[i] = {
        id,
        title,
        subtitle,
        colorScheme,
        count,
        row,
      };
    }

    return {
      xs,
      ys,
      instants,
      count,
    };
  }

  private async getSliceBuffers(
    mipmapTableName: string,
    incompleteTableName: string,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
    dataset: SourceDataset<T>,
  ): Promise<SliceBuffers<T>> {
    const engine = this.trace.engine;
    const sqlSource = generateRenderQuery(dataset);
    const extraCols = Object.keys(dataset.schema)
      .map((c) => `s.${c} as ${c}`)
      .join(',');

    // Query complete slices from mipmap + incomplete slices in one query
    // Incomplete slices use pre-computed next_ts from incompleteTableName
    const sliceQueryRes = await engine.query(`
      -- Complete slices
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

      -- Incomplete slices
      SELECT
        MAX(i.ts, ${start}) - ${start} as __ts,
        MIN(IFNULL(i.next_ts, ${end}), ${end}) - MAX(i.ts, ${start}) as __dur,
        s.id as __id,
        1 as __count,
        i.depth as __depth,
        1 as __incomplete,
        ${extraCols}
      FROM ${incompleteTableName} i
      JOIN (${sqlSource}) s ON i.id = s.id
      WHERE i.ts < ${end} AND IFNULL(i.next_ts, ${end}) > ${start}
    `);

    if (signal.isCancelled) throw QUERY_CANCELLED;
    const task = await this.deferChunkedTask();

    const count = sliceQueryRes.numRows();
    const xs = new Float32Array(count);
    const ys = new Float32Array(count);
    const ws = new Float32Array(count);
    const patterns = new Uint8Array(count);
    const slices = new Array<Slice<T>>(count);

    const it = sliceQueryRes.iter({
      __id: NUM,
      __ts: NUM,
      __dur: NUM,
      __count: NUM,
      __depth: NUM,
      __incomplete: NUM,
      ...dataset.schema,
    });

    for (let i = 0; it.valid(); it.next(), ++i) {
      if (i % 64 === 0) {
        if (signal.isCancelled) throw QUERY_CANCELLED;
        if (task.shouldYield()) await task.yield();
      }

      const count = it.__count;
      const id = it.__id;
      const ts = it.__ts;
      const dur = it.__dur;
      const depth = it.__depth;
      const title = this.getTitle(it);
      const subtitle = this.getSubtitle(it);
      const colorScheme = this.getColor(it, title);
      const isIncomplete = it.__incomplete === 1;
      const row = this.extractKeys(it, dataset.schema);

      xs[i] = ts;
      ys[i] = depth;
      ws[i] = dur;
      patterns[i] = isIncomplete
        ? RECT_PATTERN_FADE_RIGHT
        : this.attrs.slicePattern?.(it) ?? 0;
      slices[i] = {
        id,
        title,
        subtitle,
        colorScheme,
        count,
        fillRatio: this.attrs.fillRatio?.(it) ?? 1,
        row,
      };
    }

    return {
      xs,
      ys,
      ws,
      patterns,
      slices,
      count,
    };
  }

  // Efficiently copy a sebset of keys from a raw value based on some template.
  // Note: Only the template's keys are used, the values are ignored (hence the
  // unknown value types).
  private extractKeys(from: T, template: Record<keyof T, unknown>): T {
    const result = {} as T;
    // eslint-disable-next-line guard-for-in
    for (const k in template) {
      result[k] = from[k];
    }
    return result;
  }

  private async deferChunkedTask() {
    const priority = CHUNKED_TASK_BACKGROUND_PRIORITY.get()
      ? 'background'
      : undefined;
    return await deferChunkedTask({priority});
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
    slices: readonly SliceOrInstant<T>[],
  ): ColorVariant[] {
    if (this.attrs.onUpdatedSlices) {
      return this.attrs.onUpdatedSlices(slices);
    } else {
      return this.highlightHoveredAndSameTitle(slices);
    }
  }

  private highlightHoveredAndSameTitle(
    slices: readonly SliceOrInstant<T>[],
  ): ColorVariant[] {
    const highlightedSliceId = this.trace.timeline.highlightedSliceId;
    const hoveredTitle = this.hoveredSlice?.title;
    const isHovering =
      hoveredTitle !== undefined || highlightedSliceId !== undefined;
    const n = slices.length;
    const variants = new Array<ColorVariant>(n);
    for (let i = 0; i < n; i++) {
      if (!isHovering) {
        variants[i] = ColorVariant.BASE;
      } else {
        const slice = slices[i];
        const isMatch =
          highlightedSliceId === slice.id ||
          (hoveredTitle !== undefined && hoveredTitle === slice.title);
        variants[i] = isMatch ? ColorVariant.VARIANT : ColorVariant.BASE;
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
  }: TrackMouseEvent): undefined | SliceOrInstant<T> {
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
      // Check slices
      const sliceBufs = this.currentDataFrame.slices;
      for (let i = 0; i < sliceBufs.count; i++) {
        if (sliceBufs.ys[i] !== depth) continue;

        const sliceX = sliceBufs.xs[i] * pxPerNs + baseOffsetPx;
        const sliceW = sliceBufs.ws[i];

        if (sliceW === -1) {
          // Incomplete slice extends to the end of the window
          if (sliceX <= x) {
            return sliceBufs.slices[i];
          }
        } else {
          const sliceWPx = sliceW * pxPerNs;
          if (sliceX <= x && x <= sliceX + sliceWPx) {
            return sliceBufs.slices[i];
          }
        }
      }

      // Check instants
      const instantBufs = this.currentDataFrame.instants;
      const halfWidth = this.instantWidthPx / 2;
      for (let i = 0; i < instantBufs.count; i++) {
        if (instantBufs.ys[i] !== depth) continue;

        const instantX = instantBufs.xs[i] * pxPerNs + baseOffsetPx;
        if (x >= instantX - halfWidth && x <= instantX + halfWidth) {
          return instantBufs.instants[i];
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
    const targetNs = Number(targetTime);
    const searchStartNs = targetNs - thresholdNs;
    const searchEndNs = targetNs + thresholdNs;

    let closestSnap: SnapPoint | undefined = undefined;
    let closestDistNs = thresholdNs;

    const checkBoundary = (boundaryNs: number) => {
      if (boundaryNs < searchStartNs || boundaryNs > searchEndNs) {
        return;
      }
      const distNs = Math.abs(targetNs - boundaryNs);
      if (distNs < closestDistNs) {
        closestSnap = {time: Time.fromRaw(BigInt(Math.round(boundaryNs)))};
        closestDistNs = distNs;
      }
    };

    const frameStartNs = Number(this.currentDataFrame.start);

    // Check slices
    const {xs, ws, count} = this.currentDataFrame.slices;
    for (let i = 0; i < count; i++) {
      // Convert relative start to absolute time
      const sliceStartNs = frameStartNs + xs[i];
      checkBoundary(sliceStartNs);
      // Incomplete slices (dur <= 0) have no end to snap to
      if (ws[i] > 0) {
        const sliceEndNs = sliceStartNs + ws[i];
        checkBoundary(sliceEndNs);
      }
    }

    // Check instants
    const instants = this.currentDataFrame.instants;
    for (let i = 0; i < instants.count; i++) {
      const instantNs = frameStartNs + instants.xs[i];
      checkBoundary(instantNs);
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
  slice: SliceOrInstant<RowSchema>,
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
  dur: bigint | null | undefined,
): string | undefined {
  if (dur === -1n) {
    return '[Incomplete]';
  }
  if (dur === null || dur === undefined || dur === 0n) {
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

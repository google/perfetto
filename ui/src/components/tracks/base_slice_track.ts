// Copyright (C) 2021 The Android Open Source Project
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
import {Monitor} from '../../base/monitor';
import {VerticalBounds} from '../../base/geom';
import {assertExists} from '../../base/logging';
import {clamp, floatEqual} from '../../base/math_utils';
import {cropText} from '../../base/string_utils';
import {HighPrecisionTime} from '../../base/high_precision_time';
import {duration, Time, time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {featureFlags} from '../../core/feature_flags';
import {Trace} from '../../public/trace';
import {
  Slice,
  SnapPoint,
  TrackMouseEvent,
  TrackRenderContext,
  TrackRenderer,
} from '../../public/track';
import {LONG, NUM} from '../../trace_processor/query_result';
import {checkerboardExcept} from '../checkerboard';
import {UNEXPECTED_PINK} from '../colorizer';
import {deferChunkedTask} from '../../base/chunked_task';
import {CHUNKED_TASK_BACKGROUND_PRIORITY} from './feature_flags';
import {
  CancellationSignal,
  QuerySlot,
  QUERY_CANCELLED,
  SerialTaskQueue,
} from '../../base/query_slot';
import {createVirtualTable} from '../../trace_processor/sql_utils';
import {BufferedBounds} from './buffered_bounds';

const BUCKETS_PER_PIXEL = 2;

// The common class that underpins all tracks drawing slices.
export const SLICE_FLAGS_INCOMPLETE = 1;
export const SLICE_FLAGS_INSTANT = 2;

// Slices smaller than this don't get any text:
const SLICE_MIN_WIDTH_FOR_TEXT_PX = 5;
const SLICE_MIN_WIDTH_PX = 1 / BUCKETS_PER_PIXEL;
const SLICE_MIN_WIDTH_FADED_PX = 0.1;

const CHEVRON_WIDTH_PX = 10;
const DEFAULT_SLICE_COLOR = UNEXPECTED_PINK;
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

// Exposed and standalone to allow for testing without making this
// visible to subclasses.
function filterVisibleSlices<S extends Slice>(
  slices: S[],
  start: time,
  end: time,
): S[] {
  // Here we aim to reduce the number of slices we have to draw
  // by ignoring those that are not visible. A slice is visible iff:
  //   slice.endNsQ >= start && slice.startNsQ <= end
  // It's allowable to include slices which aren't visible but we
  // must not exclude visible slices.
  // We could filter this.slices using this condition but since most
  // often we should have the case where there are:
  // - First a bunch of non-visible slices to the left of the viewport
  // - Then a bunch of visible slices within the viewport
  // - Finally a second bunch of non-visible slices to the right of the
  //   viewport.
  // It seems more sensible to identify the left-most and right-most
  // visible slices then 'slice' to select these slices and everything
  // between.

  // We do not need to handle non-ending slices (where dur = -1
  // but the slice is drawn as 'infinite' length) as this is handled
  // by a special code path. See 'incomplete' in maybeRequestData.

  // While the slices are guaranteed to be ordered by timestamp we must
  // consider async slices (which are not perfectly nested). This is to
  // say if we see slice A then B it is guaranteed the A.start <= B.start
  // but there is no guarantee that (A.end < B.start XOR A.end >= B.end).
  // Due to this is not possible to use binary search to find the first
  // visible slice. Consider the following situation:
  //         start V            V end
  //     AAA  CCC       DDD   EEEEEEE
  //      BBBBBBBBBBBB            GGG
  //                           FFFFFFF
  // B is visible but A and C are not. In general there could be
  // arbitrarily many slices between B and D which are not visible.

  // You could binary search to find D (i.e. the first slice which
  // starts after |start|) then work backwards to find B.
  // The last visible slice is simpler, since the slices are sorted
  // by timestamp you can binary search for the last slice such
  // that slice.start <= end.

  return slices.filter((slice) => slice.startNs <= end && slice.endNs >= start);
}

export const filterVisibleSlicesForTesting = filterVisibleSlices;

// The minimal set of columns that any table/view must expose to render tracks.
// Note: this class assumes that, at the SQL level, slices are:
// - Not temporally overlapping (unless they are nested at inner depth).
// - Strictly stacked (i.e. a slice at depth N+1 cannot be larger than any
//   slices at depth 0..N.
// If you need temporally overlapping slices, look at AsyncSliceTrack, which
// merges several tracks into one visual track.
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

// These properties change @ 60FPS and shouldn't be touched by the subclass.
// since the Impl doesn't see every frame attempting to reason on them in a
// subclass will run in to issues.
interface SliceInternal {
  x: number;
  w: number;
  // Pre-computed relative timestamp (nanoseconds relative to dataStart) for
  // fast pixel calculation in render loop.
  startRelNs: number;
}

// We use this to avoid exposing subclasses to the properties that live on
// SliceInternal. Within BaseSliceTrack the underlying storage and private
// methods use CastInternal<S> (i.e. whatever the subclass requests
// plus our implementation fields) but when we call 'virtual' methods that
// the subclass should implement we use just S hiding x & w.
type CastInternal<S extends Slice> = S & SliceInternal;

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

// Result from table creation - contains the mipmap table and incomplete slices
interface MipmapTableResult<SliceT> extends AsyncDisposable {
  tableName: string;
  incomplete: SliceT[];
}

// Result from data fetching - includes slices and the reference time they're relative to
interface SliceDataResult<SliceT> {
  slices: SliceT[];
  refStart: time; // The start time that startRelNs values are relative to
}

export abstract class BaseSliceTrack<
  SliceT extends Slice = Slice,
  RowT extends BaseRow = BaseRow,
> implements TrackRenderer
{
  protected readonly sliceLayout: SliceLayout;

  // QuerySlot infrastructure
  private readonly queue = new SerialTaskQueue();
  private readonly tableSlot = new QuerySlot<
    MipmapTableResult<CastInternal<SliceT>>
  >(this.queue);
  private readonly initSlot = new QuerySlot<AsyncDisposable | void>(this.queue);
  private readonly dataSlot = new QuerySlot<
    SliceDataResult<CastInternal<SliceT>>
  >(this.queue);

  // Buffered bounds tracking
  private readonly bufferedBounds = new BufferedBounds();

  // Reference start time for relative timestamp calculations
  private dataRefStart: time = Time.ZERO;

  // This is the currently 'cached' slices:
  private slices = new Array<CastInternal<SliceT>>();

  // Incomplete slices (dur = -1). Rather than adding a lot of logic to
  // the SQL queries to handle this case we materialise them one off
  // then unconditionally render them. This should be efficient since
  // there are at most |depth| slices.
  private incomplete = new Array<CastInternal<SliceT>>();

  // The currently selected slice.
  // TODO(hjd): We should fetch this from the underlying data rather
  // than just remembering it when we see it.
  private selectedSlice?: CastInternal<SliceT>;

  private extraSqlColumns: string[];

  private charWidth = -1;
  protected hoveredSlice?: SliceT;

  // Monitor for local hover state (triggers DOM redraw for tooltip).
  private readonly hoverMonitor = new Monitor([() => this.hoveredSlice?.id]);

  private maxDataDepth = 0;

  // Computed layout.
  private computedTrackHeight = 0;

  // Extension points.
  // Each extension point should take a dedicated argument type (e.g.,
  // OnSliceOverArgs {slice?: S}) so it makes future extensions
  // non-API-breaking (e.g. if we want to add the X position).

  // onInit hook lets you do asynchronous set up e.g. creating a table
  // etc. We guarantee that this will be resolved before doing any
  // queries using the result of getSqlSource(). All persistent
  // state in trace_processor should be cleaned up when dispose is
  // called on the returned hook. In the common case of where
  // the data for this track is a SQL fragment this does nothing.
  async onInit(): Promise<AsyncDisposable | void> {}

  // This should be an SQL expression returning all the columns listed
  // mentioned by getRowSpec() excluding tsq and tsqEnd.
  // For example you might return an SQL expression of the form:
  // `select id, ts, dur, 0 as depth from foo where bar = 'baz'`
  abstract getSqlSource(): string;

  // Override me if you want to define what is rendered on the tooltip. Called
  // every DOM render cycle. The raw slice data is passed to this function
  protected renderTooltipForSlice(_: SliceT): m.Children {
    return undefined;
  }

  onSliceOver(_args: OnSliceOverArgs<SliceT>): void {}
  onSliceOut(_args: OnSliceOutArgs<SliceT>): void {}

  // By default, clicked slices create track selections
  onSliceClick({slice}: OnSliceClickArgs<Slice>): void {
    this.trace.selection.selectTrackEvent(this.uri, slice.id);
  }

  // The API contract of onUpdatedSlices() is:
  //  - I am going to draw these slices in the near future.
  //  - I am not going to draw any slice that I haven't passed here first.
  //  - This is guaranteed to be called at least once on every global
  //    state update.
  //  - This is NOT guaranteed to be called on every frame. For instance you
  //    cannot use this to do some colour-based animation.
  onUpdatedSlices(slices: Array<SliceT>): void {
    this.highlightHoveredAndSameTitle(slices);
  }

  // TODO(hjd): Remove.
  drawSchedLatencyArrow(
    _: CanvasRenderingContext2D,
    _selectedSlice?: SliceT,
  ): void {}

  constructor(
    protected readonly trace: Trace,
    protected readonly uri: string,
    protected readonly rowSpec: RowT,
    sliceLayout: Partial<SliceLayout> = {},
    protected readonly depthGuess: number = 0,
    protected readonly instantWidthPx: number = CHEVRON_WIDTH_PX,
    protected readonly forceTimestampRenderOrder: boolean = false,
  ) {
    // Work out the extra columns.
    // This is the union of the embedder-defined columns and the base columns
    // we know about (ts, dur, ...).
    const allCols = Object.keys(rowSpec);
    const baseCols = Object.keys(BASE_ROW);
    this.extraSqlColumns = allCols.filter((key) => !baseCols.includes(key));

    this.sliceLayout = {
      padding: sliceLayout.padding ?? 3,
      rowGap: sliceLayout.rowGap ?? 0,
      sliceHeight: sliceLayout.sliceHeight ?? 18,
      titleSizePx: sliceLayout.titleSizePx ?? 12,
      subtitleSizePx: sliceLayout.subtitleSizePx ?? 8,
    };
  }

  onFullRedraw(): void {
    // Give a chance to the embedder to change colors and other stuff.
    this.onUpdatedSlices(this.slices);
    this.onUpdatedSlices(this.incomplete);
    if (this.selectedSlice !== undefined) {
      this.onUpdatedSlices([this.selectedSlice]);
    }
  }

  private getTitleFont(): string {
    const size = this.sliceLayout.titleSizePx;
    return `${size}px Roboto Condensed`;
  }

  private getSubtitleFont(): string {
    const size = this.sliceLayout.subtitleSizePx;
    return `${size}px Roboto Condensed`;
  }

  // Creates the mipmap table and fetches incomplete slices
  // Called declaratively from render via QuerySlot
  private async createMipmapTable(): Promise<
    MipmapTableResult<CastInternal<SliceT>>
  > {
    // Call onInit hook (for subclass setup like creating tables)
    const initResult = await this.onInit();

    // Calc the number of rows based on the depth col.
    const rowCount = await this.getRowCount();

    // Fetch incomplete slices (dur = -1)
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
    const incomplete = new Array<CastInternal<SliceT>>(queryRes.numRows());
    const it = queryRes.iter(this.rowSpec);
    for (let i = 0; it.valid(); it.next(), ++i) {
      incomplete[i] = this.rowToSliceInternal(it);
    }
    this.onUpdatedSlices(incomplete);

    // Create the virtual mipmap table
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
        if (initResult) {
          await initResult[Symbol.asyncDispose]();
        }
      },
    };
  }

  /**
   * Calculate the number of rows in the track from the max depth value.
   *
   * @returns The number of rows in the track, or undefined if track is empty.
   */
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

  /**
   * Declaratively fetches data for the track. Updates internal state
   * (slices, incomplete, dataRefStart) when data is available.
   * @returns true if data is ready for rendering, false otherwise
   */
  private useData(trackCtx: TrackRenderContext): boolean {
    const {size, visibleWindow} = trackCtx;

    // Step 0: Call onInit with a constant key
    const initResult = this.initSlot.use({
      key: {init: true},
      queryFn: () => this.onInit(),
    });

    if (initResult.isPending) {
      return false;
    }

    // Step 1: Get the mipmap table (created once per SQL source)
    const tableResult = this.tableSlot.use({
      key: {sqlSource: this.getSqlSource()},
      queryFn: () => this.createMipmapTable(),
    });

    const table = tableResult.data;
    if (table === undefined) return false;

    // Update incomplete slices from table result
    this.incomplete = table.incomplete;

    // Step 2: Calculate buffered bounds and fetch slices
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

    // Step 3: Fetch slice data using QuerySlot
    // Capture bounds.start in a local variable so the queryFn closure
    // uses the correct reference time for the slices it fetches.
    const queryStart = bounds.start;
    const dataResult = this.dataSlot.use({
      key: {
        start: bounds.start,
        end: bounds.end,
        resolution: bounds.resolution,
      },
      queryFn: async (signal) => {
        const result = await this.fetchSlices(
          table.tableName,
          queryStart,
          bounds.end,
          bounds.resolution,
          signal,
        );
        this.trace.raf.scheduleFullRedraw();
        return {slices: result, refStart: queryStart};
      },
      retainOn: ['start', 'end', 'resolution'],
    });

    // Update slices when new data arrives
    if (dataResult.data !== undefined) {
      this.slices = dataResult.data.slices;
      this.dataRefStart = dataResult.data.refStart;

      // Update relative timestamps for incomplete slices
      for (const slice of this.incomplete) {
        slice.startRelNs = Number(slice.startNs - this.dataRefStart);
      }
    }

    // Return true if we have data to render
    return this.slices.length > 0 || this.incomplete.length > 0;
  }

  render(trackCtx: TrackRenderContext): void {
    const {ctx, size, timescale, colors, renderer} = trackCtx;

    // Fetch data declaratively - updates internal state
    if (!this.useData(trackCtx)) {
      return; // No data ready yet
    }

    // TODO(hjd): fonts and colors should come from the CSS and not hardcoded
    // here.

    // In any case, draw whatever we have (which might be stale/incomplete).
    let charWidth = this.charWidth;
    if (charWidth < 0) {
      // TODO(hjd): Centralize font measurement/invalidation.
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
    let discoveredSelection: CastInternal<SliceT> | undefined;

    // Believe it or not, doing 4xO(N) passes is ~2x faster than trying to draw
    // everything in one go. The key is that state changes operations on the
    // canvas (e.g., color, fonts) dominate any number crunching we do in JS.

    const sliceHeight = this.sliceLayout.sliceHeight;
    const padding = this.sliceLayout.padding;
    const rowSpacing = this.sliceLayout.rowGap;

    // First pass: compute geometry of slices.

    // pxEnd is the last visible pixel in the visible viewport. Drawing
    // anything < 0 or > pxEnd doesn't produce any visible effect as it goes
    // beyond the visible portion of the canvas.
    const pxEnd = size.width;

    // Pre-compute conversion factors for fast timestamp-to-pixel conversion.
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(this.dataRefStart);

    for (const slice of vizSlices) {
      // Compute the basic geometry for any visible slice, even if only
      // partially visible. This might end up with a negative x if the
      // slice starts before the visible time or with a width that overflows
      // pxEnd.
      slice.x = slice.startRelNs * pxPerNs + baseOffsetPx;
      slice.w = Number(slice.durNs) * pxPerNs;

      if (slice.flags & SLICE_FLAGS_INSTANT) {
        // In the case of an instant slice, set the slice geometry on the
        // bounding box that will contain the chevron.
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
        // If the slice is an actual slice, intersect the slice geometry with
        // the visible viewport (this affects only the first and last slice).
        // This is so that text is always centered even if we are zoomed in.
        // Visually if we have
        //                   [    visible viewport   ]
        //  [         slice         ]
        // The resulting geometry will be:
        //                   [slice]
        // So that the slice title stays within the visible region.
        const sliceVizLimit = Math.min(slice.x + slice.w, pxEnd);
        slice.x = Math.max(slice.x, -1);
        slice.w = sliceVizLimit - slice.x;
      }

      if (selectedId === slice.id) {
        discoveredSelection = slice;
      }
    }

    // Second pass: fill slices by color.
    const vizSlicesByColor = vizSlices.slice();
    if (!this.forceTimestampRenderOrder) {
      vizSlicesByColor.sort((a, b) =>
        colorCompare(a.colorScheme.base, b.colorScheme.base),
      );
    }
    for (const slice of vizSlices) {
      const color = slice.isHighlighted
        ? slice.colorScheme.variant
        : slice.colorScheme.base;
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
        // Draw incomplete slice
        const w = CROP_INCOMPLETE_SLICE_FLAG.get()
          ? slice.w
          : Math.max(slice.w - 2, 2);
        // Flush renderer before accessing the canvas2d context directly to
        // synchronize any reordering and invalidate caches.
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
        renderer.drawRect(slice.x, y, slice.x + w, y + sliceHeight, color);
      }
    }

    // Flush renderer before accessing the canvas2d context directly to
    // synchronize any reordering and invalidate caches.
    renderer.flush();

    // Pass 2.5: Draw fillRatio light section.
    ctx.fillStyle = `#FFFFFF50`;
    for (const slice of vizSlicesByColor) {
      // Can't draw fill ratio on incomplete or instant slices.
      if (slice.flags & (SLICE_FLAGS_INCOMPLETE | SLICE_FLAGS_INSTANT)) {
        continue;
      }

      // Clamp fillRatio between 0.0 -> 1.0
      const fillRatio = clamp(slice.fillRatio, 0, 1);

      // Don't draw anything if the fill ratio is 1.0ish
      if (floatEqual(fillRatio, 1)) {
        continue;
      }

      // Work out the width of the light section
      const sliceDrawWidth = Math.max(slice.w, SLICE_MIN_WIDTH_PX);
      const lightSectionDrawWidth = sliceDrawWidth * (1 - fillRatio);

      // Don't draw anything if the light section is smaller than 1 px
      if (lightSectionDrawWidth < 1) {
        continue;
      }

      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      const x = slice.x + (sliceDrawWidth - lightSectionDrawWidth);
      ctx.fillRect(x, y, lightSectionDrawWidth, sliceHeight);
    }

    // Third pass, draw the titles (e.g., process name for sched slices).
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

      // Change the title color dynamically depending on contrast.
      const textColor = slice.isHighlighted
        ? slice.colorScheme.textVariant
        : slice.colorScheme.textBase;
      ctx.fillStyle = textColor.cssString;
      const title = cropText(slice.title, charWidth, slice.w);
      const rectXCenter = slice.x + slice.w / 2;
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      const yDiv = slice.subTitle ? 3 : 2;
      const yMidPoint = Math.floor(y + sliceHeight / yDiv) + 0.5;
      ctx.fillText(title, rectXCenter, yMidPoint);
    }

    // Fourth pass, draw the subtitles (e.g., thread name for sched slices).
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

    // Here we need to ensure we never draw a slice that hasn't been
    // updated via the math above so we don't use this.selectedSlice
    // directly.
    if (discoveredSelection !== undefined) {
      this.selectedSlice = discoveredSelection;

      // Draw a thicker border around the selected slice (or chevron).
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

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
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

    // TODO(hjd): Remove this.
    // The only thing this does is drawing the sched latency arrow. We should
    // have some abstraction for that arrow (ideally the same we'd use for
    // flows).
    this.drawSchedLatencyArrow(ctx, this.selectedSlice);
  }

  renderTooltip() {
    const hoveredSlice = this.hoveredSlice;
    if (hoveredSlice) {
      return this.renderTooltipForSlice(hoveredSlice);
    }
    return undefined;
  }

  // Compute bucket size for a given time span and pixel width
  private computeBucketSize(
    spanDuration: duration,
    windowSizePx: number,
  ): duration {
    // Calculate ns per pixel, then round up to nearest power of 2
    const nsPerPx = Math.max(1, Number(spanDuration) / windowSizePx);
    const bucketNs = nsPerPx / BUCKETS_PER_PIXEL;
    const exp = Math.ceil(Math.log2(Math.max(1, bucketNs)));
    return BigInt(Math.pow(2, exp)) as duration;
  }

  // Fetches slices for the given bounds - called from QuerySlot
  private async fetchSlices(
    tableName: string,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
  ): Promise<CastInternal<SliceT>[]> {
    const slices = new Array<CastInternal<SliceT>>();

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
      CROSS JOIN (${this.getSqlSource()}) s using (id) jalskdfjlsakdfjlsakdf
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

    // Pre-compute relative timestamps for fast pixel calculation in render.
    for (const slice of slices) {
      slice.startRelNs = Number(slice.startNs - start);
    }

    this.onUpdatedSlices(slices);
    return slices;
  }

  private rowToSliceInternal(row: RowT): CastInternal<SliceT> {
    const slice = this.rowToSlice(row);

    // If this is a more updated version of the selected slice throw
    // away the old one.
    if (this.selectedSlice?.id === slice.id) {
      this.selectedSlice = undefined;
    }

    return {
      ...slice,
      x: -1,
      w: -1,
      startRelNs: 0, // Will be computed in maybeRequestData
    };
  }

  protected abstract rowToSlice(row: RowT): SliceT;

  protected rowToSliceBase(row: RowT): Slice {
    let flags = 0;
    if (row.dur === -1n) {
      flags |= SLICE_FLAGS_INCOMPLETE;
    } else if (row.dur === 0n) {
      flags |= SLICE_FLAGS_INSTANT;
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
      title: '',
      subTitle: '',
      fillRatio: 1,

      // The derived class doesn't need to initialize these. They are
      // rewritten on every renderCanvas() call. We just need to initialize
      // them to something.
      colorScheme: DEFAULT_SLICE_COLOR,
      isHighlighted: false,
    };
  }

  private findSlice({x, y, timescale}: TrackMouseEvent): undefined | SliceT {
    const trackHeight = this.computedTrackHeight;
    const sliceHeight = this.sliceLayout.sliceHeight;
    const padding = this.sliceLayout.padding;
    const rowGap = this.sliceLayout.rowGap;

    // Need at least a draw pass to resolve the slice layout.
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
        this.onSliceOut({slice: assertExists(prevHoveredSlice)});
      } else {
        this.onSliceOver({slice: this.hoveredSlice});
      }
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseOut(): void {
    const prevHoveredSlice = this.hoveredSlice;
    this.hoveredSlice = undefined;
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.timeline.highlightedSliceId = undefined;
      this.onSliceOut({slice: assertExists(prevHoveredSlice)});
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseClick(event: TrackMouseEvent): boolean {
    const slice = this.findSlice(event);
    if (slice === undefined) {
      return false;
    }
    const args: OnSliceClickArgs<SliceT> = {slice};
    this.onSliceClick(args);
    return true;
  }

  private getVisibleSlicesInternal(): Array<CastInternal<SliceT>> {
    const slices = this.slices.concat(this.incomplete);
    // The selected slice is always visible:
    if (this.selectedSlice && !this.slices.includes(this.selectedSlice)) {
      slices.push(this.selectedSlice);
    }
    return slices;
  }

  private updateSliceAndTrackHeight() {
    const rows = Math.max(this.maxDataDepth, this.depthGuess) + 1;
    const {padding = 2, sliceHeight = 12, rowGap = 0} = this.sliceLayout;

    // Compute the track height.
    const trackHeight = 2 * padding + rows * (sliceHeight + rowGap);

    // Compute the slice height.
    this.computedTrackHeight = trackHeight;
  }

  protected drawChevron(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    h: number,
  ) {
    // Draw an upward facing chevrons, in order: A, B, C, D, and back to A.
    // . (x, y)
    //      A
    //     ###
    //    ##C##
    //   ##   ##
    //  D       B
    //            . (x + CHEVRON_WIDTH_PX, y + h)
    const HALF_CHEVRON_WIDTH_PX = CHEVRON_WIDTH_PX / 2;
    const midX = x + HALF_CHEVRON_WIDTH_PX;
    ctx.beginPath();
    ctx.moveTo(midX, y); // A.
    ctx.lineTo(x + CHEVRON_WIDTH_PX, y + h); // B.
    ctx.lineTo(midX, y + h - HALF_CHEVRON_WIDTH_PX); // C.
    ctx.lineTo(x, y + h); // D.
    ctx.lineTo(midX, y); // Back to A.
    ctx.closePath();
    ctx.fill();
  }

  // This is a good default implementation for highlighting slices. By default
  // onUpdatedSlices() calls this. However, if the XxxSliceTrack impl overrides
  // onUpdatedSlices() this gives them a chance to call the highlighting without
  // having to reimplement it.
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

  getSnapPoint(
    targetTime: time,
    thresholdPx: number,
    timescale: TimeScale,
  ): SnapPoint | undefined {
    // Convert pixel threshold to time duration (in nanoseconds as number)
    const thresholdNs = timescale.pxToDuration(thresholdPx);

    // Use HighPrecisionTime to handle time arithmetic with fractional nanoseconds
    const hpTargetTime = new HighPrecisionTime(targetTime);
    const hpSearchStart = hpTargetTime.addNumber(-thresholdNs);
    const hpSearchEnd = hpTargetTime.addNumber(thresholdNs);

    // Convert back to time for comparisons
    const searchStart = hpSearchStart.toTime();
    const searchEnd = hpSearchEnd.toTime();

    let closestSnap: SnapPoint | undefined = undefined;
    let closestDistNs = thresholdNs;

    // Helper function to check a boundary
    const checkBoundary = (boundaryTime: time) => {
      // Skip if outside search window
      if (boundaryTime < searchStart || boundaryTime > searchEnd) {
        return;
      }

      // Calculate distance using HighPrecisionTime for accuracy
      const hpBoundary = new HighPrecisionTime(boundaryTime);
      const distNs = Math.abs(hpTargetTime.sub(hpBoundary).toNumber());

      if (distNs < closestDistNs) {
        closestSnap = {
          time: boundaryTime,
        };
        closestDistNs = distNs;
      }
    };

    // Check regular slices
    for (const slice of this.slices) {
      // Check start boundary using precise timestamp
      checkBoundary(slice.ts);

      // Check end boundary (if non-zero duration)
      if (slice.dur > 0n) {
        const endTime = Time.add(slice.ts, slice.dur);
        checkBoundary(endTime);
      }
    }

    // Check incomplete slices
    for (const slice of this.incomplete) {
      // Use precise timestamp for incomplete slices too
      checkBoundary(slice.ts);

      // Incomplete slices have dur = -1, so we don't check end
    }

    return closestSnap;
  }

  protected get engine() {
    return this.trace.engine;
  }
}

// This is the argument passed to onSliceOver(args).
// This is really a workaround for the fact that TypeScript doesn't allow
// inner types within a class (whether the class is templated or not).
export interface OnSliceOverArgs<S extends Slice> {
  // Input args (BaseSliceTrack -> Impl):
  slice: S; // The slice being hovered.

  // Output args (Impl -> BaseSliceTrack):
  tooltip?: string[]; // One entry per row, up to a max of 2.
}

export interface OnSliceOutArgs<S extends Slice> {
  // Input args (BaseSliceTrack -> Impl):
  slice: S; // The slice which is not hovered anymore.
}

export interface OnSliceClickArgs<S extends Slice> {
  // Input args (BaseSliceTrack -> Impl):
  slice: S; // The slice which is clicked.
}

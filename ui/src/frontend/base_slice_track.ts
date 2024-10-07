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

import {assertExists} from '../base/logging';
import {clamp, floatEqual} from '../base/math_utils';
import {Duration, Time, time} from '../base/time';
import {exists, Optional} from '../base/utils';
import {Actions} from '../common/actions';
import {drawIncompleteSlice, drawTrackHoverTooltip} from '../base/canvas_utils';
import {cropText} from '../base/string_utils';
import {colorCompare} from '../public/color';
import {UNEXPECTED_PINK} from '../core/colorizer';
import {TrackEventDetails} from '../public/selection';
import {featureFlags} from '../core/feature_flags';
import {raf} from '../core/raf_scheduler';
import {Track} from '../public/track';
import {Slice} from '../public/track';
import {LONG, NUM} from '../trace_processor/query_result';
import {checkerboardExcept} from './checkerboard';
import {globals} from './globals';
import {DEFAULT_SLICE_LAYOUT, SliceLayout} from './slice_layout';
import {NewTrackArgs} from './track';
import {BUCKETS_PER_PIXEL, CacheKey} from '../core/timeline_cache';
import {uuidv4Sql} from '../base/uuid';
import {AsyncDisposableStack} from '../base/disposable_stack';
import {TrackMouseEvent, TrackRenderContext} from '../public/track';
import {Point2D, VerticalBounds} from '../base/geom';
import {Trace} from '../public/trace';

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

  // One specific edge case that will come up often is when:
  // For all slice in slices: slice.startNsQ > end (e.g. all slices are
  // to the right).
  // Since the slices are sorted by startS we can check this easily:
  const maybeFirstSlice: S | undefined = slices[0];
  if (exists(maybeFirstSlice) && maybeFirstSlice.startNs > end) {
    return [];
  }

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
  depth: NUM, // Vertical depth.
};

export type BaseRow = typeof BASE_ROW;

// These properties change @ 60FPS and shouldn't be touched by the subclass.
// since the Impl doesn't see every frame attempting to reason on them in a
// subclass will run in to issues.
interface SliceInternal {
  x: number;
  w: number;
}

// We use this to avoid exposing subclasses to the properties that live on
// SliceInternal. Within BaseSliceTrack the underlying storage and private
// methods use CastInternal<S> (i.e. whatever the subclass requests
// plus our implementation fields) but when we call 'virtual' methods that
// the subclass should implement we use just S hiding x & w.
type CastInternal<S extends Slice> = S & SliceInternal;

export abstract class BaseSliceTrack<
  SliceT extends Slice = Slice,
  RowT extends BaseRow = BaseRow,
> implements Track
{
  protected sliceLayout: SliceLayout = {...DEFAULT_SLICE_LAYOUT};
  protected trace: Trace;
  protected uri: string;
  protected trackUuid = uuidv4Sql();

  // This is the over-skirted cached bounds:
  private slicesKey: CacheKey = CacheKey.zero();

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
  private hoverPos?: Point2D;
  protected hoveredSlice?: SliceT;
  private hoverTooltip: string[] = [];
  private maxDataDepth = 0;

  // Computed layout.
  private computedTrackHeight = 0;
  private computedSliceHeight = 0;
  private computedRowSpacing = 0;

  private readonly trash: AsyncDisposableStack;

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

  protected abstract getRowSpec(): RowT;
  onSliceOver(_args: OnSliceOverArgs<SliceT>): void {}
  onSliceOut(_args: OnSliceOutArgs<SliceT>): void {}
  onSliceClick(_args: OnSliceClickArgs<SliceT>): void {}

  // The API contract of onUpdatedSlices() is:
  //  - I am going to draw these slices in the near future.
  //  - I am not going to draw any slice that I haven't passed here first.
  //  - This is guaranteed to be called at least once on every global
  //    state update.
  //  - This is NOT guaranteed to be called on every frame. For instance you
  //    cannot use this to do some colour-based animation.
  onUpdatedSlices(slices: Array<SliceT>): void {
    this.highlightHovererdAndSameTitle(slices);
  }

  // TODO(hjd): Remove.
  drawSchedLatencyArrow(
    _: CanvasRenderingContext2D,
    _selectedSlice?: SliceT,
  ): void {}

  constructor(args: NewTrackArgs) {
    this.trace = args.trace;
    this.uri = args.uri;
    // Work out the extra columns.
    // This is the union of the embedder-defined columns and the base columns
    // we know about (ts, dur, ...).
    const allCols = Object.keys(this.getRowSpec());
    const baseCols = Object.keys(BASE_ROW);
    this.extraSqlColumns = allCols.filter((key) => !baseCols.includes(key));

    this.trash = new AsyncDisposableStack();
  }

  setSliceLayout(sliceLayout: SliceLayout) {
    if (
      sliceLayout.isFlat &&
      sliceLayout.depthGuess !== undefined &&
      sliceLayout.depthGuess !== 0
    ) {
      const {isFlat, depthGuess} = sliceLayout;
      throw new Error(
        `if isFlat (${isFlat}) then depthGuess (${depthGuess}) must be 0 if defined`,
      );
    }
    this.sliceLayout = sliceLayout;
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
    const size = this.sliceLayout.titleSizePx ?? 12;
    return `${size}px Roboto Condensed`;
  }

  private getSubtitleFont(): string {
    const size = this.sliceLayout.subtitleSizePx ?? 8;
    return `${size}px Roboto Condensed`;
  }

  private getTableName(): string {
    return `slice_${this.trackUuid}`;
  }

  async onCreate(): Promise<void> {
    const result = await this.onInit();
    result && this.trash.use(result);

    // TODO(hjd): Consider case below:
    // raw:
    // 0123456789
    //   [A     did not end)
    //     [B ]
    //
    //
    // quantised:
    // 0123456789
    //   [A     did not end)
    // [     B  ]
    // Does it lead to odd results?
    const extraCols = this.extraSqlColumns.join(',');
    let queryRes;
    if (CROP_INCOMPLETE_SLICE_FLAG.get()) {
      queryRes = await this.engine.query(`
          select
            ${this.depthColumn()},
            ts as tsQ,
            ts,
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
          ${this.depthColumn()},
          max(ts) as tsQ,
          ts,
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
    const it = queryRes.iter(this.getRowSpec());
    for (let i = 0; it.valid(); it.next(), ++i) {
      incomplete[i] = this.rowToSliceInternal(it);
    }
    this.onUpdatedSlices(incomplete);
    this.incomplete = incomplete;

    await this.engine.query(`
      create virtual table ${this.getTableName()}
      using __intrinsic_slice_mipmap((
        select id, ts, dur, ${this.depthColumn()}
        from (${this.getSqlSource()})
        where dur != -1
      ));
    `);

    this.trash.defer(async () => {
      await this.engine.tryQuery(`drop table ${this.getTableName()}`);
    });
  }

  async onUpdate({visibleWindow, size}: TrackRenderContext): Promise<void> {
    const windowSizePx = Math.max(1, size.width);
    const timespan = visibleWindow.toTimeSpan();
    const rawSlicesKey = CacheKey.create(
      timespan.start,
      timespan.end,
      windowSizePx,
    );

    // If the visible time range is outside the cached area, requests
    // asynchronously new data from the SQL engine.
    await this.maybeRequestData(rawSlicesKey);
  }

  render({ctx, size, visibleWindow, timescale}: TrackRenderContext): void {
    // TODO(hjd): fonts and colors should come from the CSS and not hardcoded
    // here.

    // In any case, draw whatever we have (which might be stale/incomplete).
    let charWidth = this.charWidth;
    if (charWidth < 0) {
      // TODO(hjd): Centralize font measurement/invalidation.
      ctx.font = this.getTitleFont();
      charWidth = this.charWidth = ctx.measureText('dbpqaouk').width / 8;
    }

    // Filter only the visible slices. |this.slices| will have more slices than
    // needed because maybeRequestData() over-fetches to handle small pan/zooms.
    // We don't want to waste time drawing slices that are off screen.
    const vizSlices = this.getVisibleSlicesInternal(
      visibleWindow.start.toTime('floor'),
      visibleWindow.end.toTime('ceil'),
    );

    const selection = globals.selectionManager.selection;
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

    const sliceHeight = this.computedSliceHeight;
    const padding = this.sliceLayout.padding;
    const rowSpacing = this.computedRowSpacing;

    // First pass: compute geometry of slices.

    // pxEnd is the last visible pixel in the visible viewport. Drawing
    // anything < 0 or > pxEnd doesn't produce any visible effect as it goes
    // beyond the visible portion of the canvas.
    const pxEnd = size.width;

    for (const slice of vizSlices) {
      // Compute the basic geometry for any visible slice, even if only
      // partially visible. This might end up with a negative x if the
      // slice starts before the visible time or with a width that overflows
      // pxEnd.
      slice.x = timescale.timeToPx(slice.startNs);
      slice.w = timescale.durationToPx(slice.durNs);

      if (slice.flags & SLICE_FLAGS_INSTANT) {
        // In the case of an instant slice, set the slice geometry on the
        // bounding box that will contain the chevron.
        slice.x -= CHEVRON_WIDTH_PX / 2;
        slice.w = CHEVRON_WIDTH_PX;
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
        slice.x = Math.max(slice.x, 0);
        slice.w = sliceVizLimit - slice.x;
      }

      if (selectedId === slice.id) {
        discoveredSelection = slice;
      }
    }

    // Second pass: fill slices by color.
    const vizSlicesByColor = vizSlices.slice();
    vizSlicesByColor.sort((a, b) =>
      colorCompare(a.colorScheme.base, b.colorScheme.base),
    );
    let lastColor = undefined;
    for (const slice of vizSlicesByColor) {
      const color = slice.isHighlighted
        ? slice.colorScheme.variant.cssString
        : slice.colorScheme.base.cssString;
      if (color !== lastColor) {
        lastColor = color;
        ctx.fillStyle = color;
      }
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      if (slice.flags & SLICE_FLAGS_INSTANT) {
        this.drawChevron(ctx, slice.x, y, sliceHeight);
      } else if (slice.flags & SLICE_FLAGS_INCOMPLETE) {
        const w = CROP_INCOMPLETE_SLICE_FLAG.get()
          ? slice.w
          : Math.max(slice.w - 2, 2);
        drawIncompleteSlice(
          ctx,
          slice.x,
          y,
          w,
          sliceHeight,
          !CROP_INCOMPLETE_SLICE_FLAG.get(),
        );
      } else {
        const w = Math.max(
          slice.w,
          FADE_THIN_SLICES_FLAG.get()
            ? SLICE_MIN_WIDTH_FADED_PX
            : SLICE_MIN_WIDTH_PX,
        );
        ctx.fillRect(slice.x, y, w, sliceHeight);
      }
    }

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
      const color = slice.colorScheme;
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      ctx.strokeStyle = color.base.setHSL({s: 100, l: 10}).cssString;
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
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      timescale.timeToPx(this.slicesKey.start),
      timescale.timeToPx(this.slicesKey.end),
    );

    // TODO(hjd): Remove this.
    // The only thing this does is drawing the sched latency arrow. We should
    // have some abstraction for that arrow (ideally the same we'd use for
    // flows).
    this.drawSchedLatencyArrow(ctx, this.selectedSlice);

    // If a slice is hovered, draw the tooltip.
    const tooltip = this.hoverTooltip;
    if (
      this.hoveredSlice !== undefined &&
      tooltip.length > 0 &&
      this.hoverPos !== undefined
    ) {
      if (tooltip.length === 1) {
        drawTrackHoverTooltip(ctx, this.hoverPos, size, tooltip[0]);
      } else {
        drawTrackHoverTooltip(ctx, this.hoverPos, size, tooltip[0], tooltip[1]);
      }
    } // if (hoveredSlice)
  }

  async onDestroy(): Promise<void> {
    await this.trash.asyncDispose();
  }

  // This method figures out if the visible window is outside the bounds of
  // the cached data and if so issues new queries (i.e. sorta subsumes the
  // onBoundsChange).
  private async maybeRequestData(rawSlicesKey: CacheKey) {
    if (rawSlicesKey.isCoveredBy(this.slicesKey)) {
      return; // We have the data already, no need to re-query
    }

    // Determine the cache key:
    const slicesKey = rawSlicesKey.normalize();
    if (!rawSlicesKey.isCoveredBy(slicesKey)) {
      throw new Error(
        `Normalization error ${slicesKey.toString()} ${rawSlicesKey.toString()}`,
      );
    }

    const resolution = rawSlicesKey.bucketSize;
    const extraCols = this.extraSqlColumns.join(',');
    const queryRes = await this.engine.query(`
      SELECT
        (z.ts / ${resolution}) * ${resolution} as tsQ,
        ((z.dur + ${resolution - 1n}) / ${resolution}) * ${resolution} as durQ,
        s.ts as ts,
        s.dur as dur,
        s.id,
        z.depth
        ${extraCols ? ',' + extraCols : ''}
      FROM ${this.getTableName()}(
        ${slicesKey.start},
        ${slicesKey.end},
        ${slicesKey.bucketSize}
      ) z
      CROSS JOIN (${this.getSqlSource()}) s using (id)
    `);

    // Here convert each row to a Slice. We do what we can do
    // generically in the base class, and delegate the rest to the impl
    // via that rowToSlice() abstract call.
    const slices = new Array<CastInternal<SliceT>>();
    const it = queryRes.iter(this.getRowSpec());

    let maxDataDepth = this.maxDataDepth;
    this.slicesKey = slicesKey;
    for (let i = 0; it.valid(); it.next(), ++i) {
      if (it.dur === -1n) {
        continue;
      }

      maxDataDepth = Math.max(maxDataDepth, it.depth);
      // Construct the base slice. The Impl will construct and return
      // the full derived T["slice"] (e.g. CpuSlice) in the
      // rowToSlice() method.
      slices.push(this.rowToSliceInternal(it));
    }
    this.maxDataDepth = maxDataDepth;
    this.onUpdatedSlices(slices);
    this.slices = slices;

    raf.scheduleRedraw();
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
    const sliceHeight = this.computedSliceHeight;
    const padding = this.sliceLayout.padding;
    const rowSpacing = this.computedRowSpacing;

    // Need at least a draw pass to resolve the slice layout.
    if (sliceHeight === 0) {
      return undefined;
    }

    const depth = Math.floor((y - padding) / (sliceHeight + rowSpacing));

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

  private isFlat(): boolean {
    return this.sliceLayout.isFlat ?? false;
  }

  private depthColumn(): string {
    return this.isFlat() ? '0 as depth' : 'depth';
  }

  onMouseMove(event: TrackMouseEvent): void {
    const {x, y} = event;
    this.hoverPos = {x, y};
    this.updateHoveredSlice(this.findSlice(event));
  }

  onMouseOut(): void {
    this.updateHoveredSlice(undefined);
  }

  private updateHoveredSlice(slice?: SliceT): void {
    const lastHoveredSlice = this.hoveredSlice;
    this.hoveredSlice = slice;

    // Only notify the Impl if the hovered slice changes:
    if (slice === lastHoveredSlice) return;

    if (this.hoveredSlice === undefined) {
      globals.dispatch(Actions.setHighlightedSliceId({sliceId: -1}));
      this.onSliceOut({slice: assertExists(lastHoveredSlice)});
      this.hoverTooltip = [];
      this.hoverPos = undefined;
    } else {
      const args: OnSliceOverArgs<SliceT> = {slice: this.hoveredSlice};
      globals.dispatch(
        Actions.setHighlightedSliceId({sliceId: this.hoveredSlice.id}),
      );
      this.onSliceOver(args);
      this.hoverTooltip = args.tooltip || [];
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

  private getVisibleSlicesInternal(
    start: time,
    end: time,
  ): Array<CastInternal<SliceT>> {
    // Slice visibility is computed using tsq / endTsq. The means an
    // event at ts=100n can end up with tsq=90n depending on the bucket
    // calculation. start and end here are the direct unquantised
    // boundaries so when start=100n we should see the event at tsq=90n
    // Ideally we would quantize start and end via the same calculation
    // we used for slices but since that calculation happens in SQL
    // this is hard. Instead we increase the range by +1 bucket in each
    // direction. It's fine to overestimate since false positives
    // (incorrectly marking a slice as visible) are not a problem it's
    // only false negatives we have to avoid.
    start = Time.sub(start, this.slicesKey.bucketSize);
    end = Time.add(end, this.slicesKey.bucketSize);

    let slices = filterVisibleSlices<CastInternal<SliceT>>(
      this.slices,
      start,
      end,
    );
    slices = slices.concat(this.incomplete);
    // The selected slice is always visible:
    if (this.selectedSlice && !this.slices.includes(this.selectedSlice)) {
      slices.push(this.selectedSlice);
    }
    return slices;
  }

  private updateSliceAndTrackHeight() {
    const lay = this.sliceLayout;
    const rows = Math.max(this.maxDataDepth, lay.depthGuess ?? 0) + 1;

    // Compute the track height.
    let trackHeight;
    if (lay.heightMode === 'FIXED') {
      trackHeight = lay.fixedHeight;
    } else {
      trackHeight = 2 * lay.padding + rows * (lay.sliceHeight + lay.rowSpacing);
    }

    // Compute the slice height.
    let sliceHeight: number;
    let rowSpacing: number = lay.rowSpacing;
    if (lay.heightMode === 'FIXED') {
      const rowHeight = (trackHeight - 2 * lay.padding) / rows;
      sliceHeight = Math.floor(Math.max(rowHeight - lay.rowSpacing, 0.5));
      rowSpacing = Math.max(lay.rowSpacing, rowHeight - sliceHeight);
      rowSpacing = Math.floor(rowSpacing * 2) / 2;
    } else {
      sliceHeight = lay.sliceHeight;
    }
    this.computedSliceHeight = sliceHeight;
    this.computedTrackHeight = trackHeight;
    this.computedRowSpacing = rowSpacing;
  }

  private drawChevron(
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
  protected highlightHovererdAndSameTitle(slices: Slice[]) {
    for (const slice of slices) {
      const isHovering =
        globals.state.highlightedSliceId === slice.id ||
        (this.hoveredSlice && this.hoveredSlice.title === slice.title);
      slice.isHighlighted = !!isHovering;
    }
  }

  getHeight(): number {
    this.updateSliceAndTrackHeight();
    return this.computedTrackHeight;
  }

  getSliceVerticalBounds(depth: number): Optional<VerticalBounds> {
    this.updateSliceAndTrackHeight();

    const totalSliceHeight = this.computedRowSpacing + this.computedSliceHeight;
    const top = this.sliceLayout.padding + depth * totalSliceHeight;

    return {
      top,
      bottom: top + this.computedSliceHeight,
    };
  }

  protected get engine() {
    return this.trace.engine;
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const query = `
      SELECT
        ts,
        dur
      FROM (${this.getSqlSource()})
      WHERE id = ${id}
    `;

    const result = await this.engine.query(query);
    if (result.numRows() === 0) {
      return undefined;
    }
    const row = result.iter({
      ts: LONG,
      dur: LONG,
    });
    return {ts: Time.fromRaw(row.ts), dur: Duration.fromRaw(row.dur)};
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

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
import {Actions} from '../common/actions';
import {cropText, drawIncompleteSlice} from '../common/canvas_utils';
import {
  colorCompare,
  colorToStr,
  UNEXPECTED_PINK_COLOR,
} from '../common/colorizer';
import {LONG, NUM} from '../common/query_result';
import {Selection, SelectionKind} from '../common/state';
import {
  TPDuration,
  tpDurationFromNanos,
  TPTime,
  tpTimeFromNanos,
} from '../common/time';

import {checkerboardExcept} from './checkerboard';
import {globals} from './globals';
import {Slice} from './slice';
import {DEFAULT_SLICE_LAYOUT, SliceLayout} from './slice_layout';
import {NewTrackArgs, SliceRect, Track} from './track';
import {BUCKETS_PER_PIXEL, CacheKey, TrackCache} from './track_cache';

// The common class that underpins all tracks drawing slices.

export const SLICE_FLAGS_INCOMPLETE = 1;
export const SLICE_FLAGS_INSTANT = 2;

// Slices smaller than this don't get any text:
const SLICE_MIN_WIDTH_FOR_TEXT_PX = 5;
const SLICE_MIN_WIDTH_PX = 1 / BUCKETS_PER_PIXEL;
const CHEVRON_WIDTH_PX = 10;
const DEFAULT_SLICE_COLOR = UNEXPECTED_PINK_COLOR;

// Exposed and standalone to allow for testing without making this
// visible to subclasses.
function filterVisibleSlices<S extends Slice>(
    slices: S[], start: TPTime, end: TPTime): S[] {
  // Here we aim to reduce the number of slices we have to draw
  // by ignoring those that are not visible. A slice is visible iff:
  //   slice.start + slice.duration >= start && slice.start <= end
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
  // by a special code path.
  // TODO(hjd): Implement special code path.

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
  // For all slice in slices: slice.startS > endS (e.g. all slices are to the
  // right). Since the slices are sorted by startS we can check this easily:
  const maybeFirstSlice: S|undefined = slices[0];
  if (maybeFirstSlice && maybeFirstSlice.start > end) {
    return [];
  }
  // It's not possible to easily check the analogous edge case where all slices
  // are to the left:
  // For all slice in slices: slice.startS + slice.durationS < startS
  // as the slices are not ordered by 'endS'.

  // As described above you could do some clever binary search combined with
  // iteration however that seems quite complicated and error prone so instead
  // the idea of the code below is that we iterate forward though the
  // array incrementing startIdx until we find the first visible slice
  // then backwards through the array decrementing endIdx until we find the
  // last visible slice. In the worst case we end up doing one full pass on
  // the array. This code is robust to slices not being sorted.
  let startIdx = 0;
  let endIdx = slices.length;
  for (; startIdx < endIdx; ++startIdx) {
    const slice = slices[startIdx];
    const sliceEndS = slice.start + slice.duration;
    if (sliceEndS >= start && slice.start <= end) {
      break;
    }
  }
  for (; startIdx < endIdx; --endIdx) {
    const slice = slices[endIdx - 1];
    const sliceEndS = slice.start + slice.duration;
    if (sliceEndS >= start && slice.start <= end) {
      break;
    }
  }
  return slices.slice(startIdx, endIdx);
}

export const filterVisibleSlicesForTesting = filterVisibleSlices;

// The minimal set of columns that any table/view must expose to render tracks.
// Note: this class assumes that, at the SQL level, slices are:
// - Not temporally overlapping (unless they are nested at inner depth).
// - Strictly stacked (i.e. a slice at depth N+1 cannot be larger than any
//   slices at depth 0..N.
// If you need temporally overlapping slices, look at AsyncSliceTrack, which
// merges several tracks into one visual track.
export const BASE_SLICE_ROW = {
  id: NUM,      // The slice ID, for selection / lookups.
  tsq: NUM,     // Quantized |ts|. This class owns the quantization logic.
  tsqEnd: NUM,  // Quantized |ts+dur|. The end bucket.
  ts: NUM,      // Start time in nanoseconds.
  dur: NUM,     // Duration in nanoseconds. -1 = incomplete, 0 = instant.
  depth: NUM,   // Vertical depth.
};

export type BaseSliceRow = typeof BASE_SLICE_ROW;

// These properties change @ 60FPS and shouldn't be touched by the subclass.
// since the Impl doesn't see every frame attempting to reason on them in a
// subclass will run in to issues.
interface SliceInternal {
  x: number;
  w: number;
}

// We use this to avoid exposing subclasses to the properties that live on
// SliceInternal. Within BaseSliceTrack the underlying storage and private
// methods use CastInternal<T['slice']> (i.e. whatever the subclass requests
// plus our implementation fields) but when we call 'virtual' methods that
// the subclass should implement we use just T['slice'] hiding x & w.
type CastInternal<S extends Slice> = S&SliceInternal;

// The meta-type which describes the types used to extend the BaseSliceTrack.
// Derived classes can extend this interface to override these types if needed.
export interface BaseSliceTrackTypes {
  slice: Slice;
  row: BaseSliceRow;
  config: {};
}

export abstract class BaseSliceTrack<T extends BaseSliceTrackTypes =
                                                   BaseSliceTrackTypes> extends
    Track<T['config']> {
  protected sliceLayout: SliceLayout = {...DEFAULT_SLICE_LAYOUT};

  // This is the over-skirted cached bounds:
  private slicesKey: CacheKey = CacheKey.zero();

  // This is the currently 'cached' slices:
  private slices = new Array<CastInternal<T['slice']>>();

  // This is the slices cache:
  private cache: TrackCache<Array<CastInternal<T['slice']>>> =
      new TrackCache(5);

  protected readonly tableName: string;
  private maxDurNs: TPDuration = 0n;
  private sqlState: 'UNINITIALIZED'|'INITIALIZING'|'QUERY_PENDING'|
      'QUERY_DONE' = 'UNINITIALIZED';
  private extraSqlColumns: string[];

  private charWidth = -1;
  private hoverPos?: {x: number, y: number};
  protected hoveredSlice?: T['slice'];
  private hoverTooltip: string[] = [];
  private maxDataDepth = 0;

  // Computed layout.
  private computedTrackHeight = 0;
  private computedSliceHeight = 0;
  private computedRowSpacing = 0;

  // True if this track (and any views tables it might have created) has been
  // destroyed. This is unfortunately error prone (since we must manually check
  // this between each query).
  // TODO(hjd): Replace once we have cancellable query sequences.
  private isDestroyed = false;

  // Extension points.
  // Each extension point should take a dedicated argument type (e.g.,
  // OnSliceOverArgs {slice?: T['slice']}) so it makes future extensions
  // non-API-breaking (e.g. if we want to add the X position).
  abstract initSqlTable(_tableName: string): Promise<void>;
  getRowSpec(): T['row'] {
    return BASE_SLICE_ROW;
  }
  onSliceOver(_args: OnSliceOverArgs<T['slice']>): void {}
  onSliceOut(_args: OnSliceOutArgs<T['slice']>): void {}
  onSliceClick(_args: OnSliceClickArgs<T['slice']>): void {}

  // The API contract of onUpdatedSlices() is:
  //  - I am going to draw these slices in the near future.
  //  - I am not going to draw any slice that I haven't passed here first.
  //  - This is guaranteed to be called at least once on every global
  //    state update.
  //  - This is NOT guaranteed to be called on every frame. For instance you
  //    cannot use this to do some colour-based animation.
  onUpdatedSlices(slices: Array<T['slice']>): void {
    this.highlightHovererdAndSameTitle(slices);
  }

  // TODO(hjd): Remove.
  drawSchedLatencyArrow(
      _: CanvasRenderingContext2D, _selectedSlice?: T['slice']): void {}

  constructor(args: NewTrackArgs) {
    super(args);
    this.frontendOnly = true;  // Disable auto checkerboarding.
    // TODO(hjd): Handle pinned tracks, which current cause a crash
    // since the tableName we generate is the same for both.
    this.tableName = `track_${this.trackId}`.replace(/[^a-zA-Z0-9_]+/g, '_');

    // Work out the extra columns.
    // This is the union of the embedder-defined columns and the base columns
    // we know about (ts, dur, ...).
    const allCols = Object.keys(this.getRowSpec());
    const baseCols = Object.keys(BASE_SLICE_ROW);
    this.extraSqlColumns = allCols.filter((key) => !baseCols.includes(key));
  }

  setSliceLayout(sliceLayout: SliceLayout) {
    if (sliceLayout.minDepth > sliceLayout.maxDepth) {
      const {maxDepth, minDepth} = sliceLayout;
      throw new Error(`minDepth ${minDepth} must be <= maxDepth ${maxDepth}`);
    }
    this.sliceLayout = sliceLayout;
  }

  onFullRedraw(): void {
    // Give a chance to the embedder to change colors and other stuff.
    this.onUpdatedSlices(this.slices);
  }

  protected isSelectionHandled(selection: Selection): boolean {
    // TODO(hjd): Remove when updating selection.
    // We shouldn't know here about CHROME_SLICE. Maybe should be set by
    // whatever deals with that. Dunno the namespace of selection is weird. For
    // most cases in non-ambiguous (because most things are a 'slice'). But some
    // others (e.g. THREAD_SLICE) have their own ID namespace so we need this.
    const supportedSelectionKinds: SelectionKind[] = ['SLICE', 'CHROME_SLICE'];
    return supportedSelectionKinds.includes(selection.kind);
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    // TODO(hjd): fonts and colors should come from the CSS and not hardcoded
    // here.
    const {
      visibleTimeScale: timeScale,
      visibleWindowTime: vizTime,
    } = globals.frontendLocalState;

    {
      const windowSizePx = Math.max(1, timeScale.pxSpan.delta);
      const rawStartNs = vizTime.start.toTPTime();
      const rawEndNs = vizTime.end.toTPTime();
      const rawSlicesKey = CacheKey.create(rawStartNs, rawEndNs, windowSizePx);

      // If the visible time range is outside the cached area, requests
      // asynchronously new data from the SQL engine.
      this.maybeRequestData(rawSlicesKey);
    }

    // In any case, draw whatever we have (which might be stale/incomplete).

    let charWidth = this.charWidth;
    if (charWidth < 0) {
      // TODO(hjd): Centralize font measurement/invalidation.
      ctx.font = '12px Roboto Condensed';
      charWidth = this.charWidth = ctx.measureText('dbpqaouk').width / 8;
    }

    // Filter only the visible slices. |this.slices| will have more slices than
    // needed because maybeRequestData() over-fetches to handle small pan/zooms.
    // We don't want to waste time drawing slices that are off screen.
    const vizSlices = this.getVisibleSlicesInternal(
        vizTime.start.toTPTime('floor'), vizTime.end.toTPTime('ceil'));

    let selection = globals.state.currentSelection;

    if (!selection || !this.isSelectionHandled(selection)) {
      selection = null;
    }

    // Believe it or not, doing 4xO(N) passes is ~2x faster than trying to draw
    // everything in one go. The key is that state changes operations on the
    // canvas (e.g., color, fonts) dominate any number crunching we do in JS.

    this.updateSliceAndTrackHeight();
    const sliceHeight = this.computedSliceHeight;
    const padding = this.sliceLayout.padding;
    const rowSpacing = this.computedRowSpacing;

    // First pass: compute geometry of slices.
    let selSlice: CastInternal<T['slice']>|undefined;

    // pxEnd is the last visible pixel in the visible viewport. Drawing
    // anything < 0 or > pxEnd doesn't produce any visible effect as it goes
    // beyond the visible portion of the canvas.
    const pxEnd = Math.floor(timeScale.hpTimeToPx(vizTime.end));

    for (const slice of vizSlices) {
      // Compute the basic geometry for any visible slice, even if only
      // partially visible. This might end up with a negative x if the
      // slice starts before the visible time or with a width that overflows
      // pxEnd.
      slice.x = timeScale.tpTimeToPx(slice.start);
      slice.w = timeScale.durationToPx(slice.duration);
      if (slice.flags & SLICE_FLAGS_INSTANT) {
        // In the case of an instant slice, set the slice geometry on the
        // bounding box that will contain the chevron.
        slice.x -= CHEVRON_WIDTH_PX / 2;
        slice.w = CHEVRON_WIDTH_PX;
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

      if (selection && (selection as {id: number}).id === slice.id) {
        selSlice = slice;
      }
    }

    // Second pass: fill slices by color.
    // The .slice() turned out to be an unintended pun.
    const vizSlicesByColor = vizSlices.slice();
    vizSlicesByColor.sort((a, b) => colorCompare(a.color, b.color));
    let lastColor = undefined;
    for (const slice of vizSlicesByColor) {
      if (slice.color !== lastColor) {
        lastColor = slice.color;
        ctx.fillStyle = colorToStr(slice.color);
      }
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      if (slice.flags & SLICE_FLAGS_INSTANT) {
        this.drawChevron(ctx, slice.x, y, sliceHeight);
      } else if (slice.flags & SLICE_FLAGS_INCOMPLETE) {
        const w = Math.max(slice.w - 2, 2);
        drawIncompleteSlice(ctx, slice.x, y, w, sliceHeight);
      } else {
        const w = Math.max(slice.w, SLICE_MIN_WIDTH_PX);
        ctx.fillRect(slice.x, y, w, sliceHeight);
      }
    }

    // Third pass, draw the titles (e.g., process name for sched slices).
    ctx.fillStyle = '#fff';
    ctx.textAlign = 'center';
    ctx.font = '12px Roboto Condensed';
    ctx.textBaseline = 'middle';
    for (const slice of vizSlices) {
      if ((slice.flags & SLICE_FLAGS_INSTANT) || !slice.title ||
          slice.w < SLICE_MIN_WIDTH_FOR_TEXT_PX) {
        continue;
      }

      const title = cropText(slice.title, charWidth, slice.w);
      const rectXCenter = slice.x + slice.w / 2;
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      const yDiv = slice.subTitle ? 3 : 2;
      const yMidPoint = Math.floor(y + sliceHeight / yDiv) - 0.5;
      ctx.fillText(title, rectXCenter, yMidPoint);
    }

    // Fourth pass, draw the subtitles (e.g., thread name for sched slices).
    ctx.fillStyle = 'rgba(255, 255, 255, 0.6)';
    ctx.font = '10px Roboto Condensed';
    for (const slice of vizSlices) {
      if (slice.w < SLICE_MIN_WIDTH_FOR_TEXT_PX || !slice.subTitle ||
          (slice.flags & SLICE_FLAGS_INSTANT)) {
        continue;
      }
      const rectXCenter = slice.x + slice.w / 2;
      const subTitle = cropText(slice.subTitle, charWidth, slice.w);
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      const yMidPoint = Math.ceil(y + sliceHeight * 2 / 3) + 1.5;
      ctx.fillText(subTitle, rectXCenter, yMidPoint);
    }

    // Draw a thicker border around the selected slice (or chevron).
    if (selSlice !== undefined) {
      const color = selSlice.color;
      const y = padding + selSlice.depth * (sliceHeight + rowSpacing);
      ctx.strokeStyle = `hsl(${color.h}, ${color.s}%, 30%)`;
      ctx.beginPath();
      const THICKNESS = 3;
      ctx.lineWidth = THICKNESS;
      ctx.strokeRect(
          selSlice.x, y - THICKNESS / 2, selSlice.w, sliceHeight + THICKNESS);
      ctx.closePath();
    }

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        this.getHeight(),
        timeScale.hpTimeToPx(vizTime.start),
        timeScale.hpTimeToPx(vizTime.end),
        timeScale.tpTimeToPx(this.slicesKey.start),
        timeScale.tpTimeToPx(this.slicesKey.end));

    // TODO(hjd): Remove this.
    // The only thing this does is drawing the sched latency arrow. We should
    // have some abstraction for that arrow (ideally the same we'd use for
    // flows).
    this.drawSchedLatencyArrow(ctx, selSlice);

    // If a slice is hovered, draw the tooltip.
    const tooltip = this.hoverTooltip;
    if (this.hoveredSlice !== undefined && tooltip.length > 0 &&
        this.hoverPos !== undefined) {
      if (tooltip.length === 1) {
        this.drawTrackHoverTooltip(ctx, this.hoverPos, tooltip[0]);
      } else {
        this.drawTrackHoverTooltip(ctx, this.hoverPos, tooltip[0], tooltip[1]);
      }
    }  // if (hoveredSlice)
  }

  onDestroy() {
    super.onDestroy();
    this.isDestroyed = true;
    this.engine.query(`DROP VIEW IF EXISTS ${this.tableName}`);
  }

  // This method figures out if the visible window is outside the bounds of
  // the cached data and if so issues new queries (i.e. sorta subsumes the
  // onBoundsChange).
  private async maybeRequestData(rawSlicesKey: CacheKey) {
    // Important: this method is async and is invoked on every frame. Care
    // must be taken to avoid piling up queries on every frame, hence the FSM.
    if (this.sqlState === 'UNINITIALIZED') {
      this.sqlState = 'INITIALIZING';

      if (this.isDestroyed) {
        return;
      }
      await this.initSqlTable(this.tableName);

      if (this.isDestroyed) {
        return;
      }
      const queryRes = await this.engine.query(`select
          ifnull(max(dur), 0) as maxDur, count(1) as rowCount
          from ${this.tableName}`);
      const row = queryRes.firstRow({maxDur: LONG, rowCount: NUM});
      this.maxDurNs = row.maxDur;
      this.sqlState = 'QUERY_DONE';
    } else if (
        this.sqlState === 'INITIALIZING' || this.sqlState === 'QUERY_PENDING') {
      return;
    }

    if (rawSlicesKey.isCoveredBy(this.slicesKey)) {
      return;  // We have the data already, no need to re-query
    }

    // Determine the cache key:
    const slicesKey = rawSlicesKey.normalize();
    if (!rawSlicesKey.isCoveredBy(slicesKey)) {
      throw new Error(`Normalization error ${slicesKey.toString()} ${
          rawSlicesKey.toString()}`);
    }

    const maybeCachedSlices = this.cache.lookup(slicesKey);
    if (maybeCachedSlices) {
      this.slicesKey = slicesKey;
      this.onUpdatedSlices(maybeCachedSlices);
      this.slices = maybeCachedSlices;
      return;
    }

    this.sqlState = 'QUERY_PENDING';
    const bucketNs = slicesKey.bucketSize;
    let queryTsq;
    let queryTsqEnd;
    // When we're zoomed into the level of single ns there is no point
    // doing quantization (indeed it causes bad artifacts) so instead
    // we use ts / ts+dur directly.
    if (bucketNs === 1n) {
      queryTsq = 'ts';
      queryTsqEnd = 'ts + dur';
    } else {
      queryTsq = `(ts + ${bucketNs / 2n}) / ${bucketNs} * ${bucketNs}`;
      queryTsqEnd = `(ts + dur + ${bucketNs / 2n}) / ${bucketNs} * ${bucketNs}`;
    }

    const extraCols = this.extraSqlColumns.join(',');
    let depthCol = 'depth';
    let maybeGroupByDepth = 'depth, ';
    const layout = this.sliceLayout;
    const isFlat = (layout.maxDepth - layout.minDepth) <= 1;
    // maxDepth === minDepth only makes sense if track is empty which on the
    // one hand isn't very useful (and so maybe should be an error) on the
    // other hand I can see it happening if someone does:
    // minDepth = min(slices.depth); maxDepth = max(slices.depth);
    // and slices is empty, so we treat that as flat.
    if (isFlat) {
      depthCol = `${this.sliceLayout.minDepth} as depth`;
      maybeGroupByDepth = '';
    }

    // TODO(hjd): Re-reason and improve this query:
    // - Materialize the unfinished slices one off.
    // - Avoid the union if we know we don't have any -1 slices.
    // - Maybe we don't need the union at all and can deal in TS?
    if (this.isDestroyed) {
      this.sqlState = 'QUERY_DONE';
      return;
    }
    // TODO(hjd): Count and expose the number of slices summarized in
    // each bucket?
    const queryRes = await this.engine.query(`
    with q1 as (
      select
        ${queryTsq} as tsq,
        ${queryTsqEnd} as tsqEnd,
        ts,
        max(dur) as dur,
        id,
        ${depthCol}
        ${extraCols ? ',' + extraCols : ''}
      from ${this.tableName}
      where
        ts >= ${slicesKey.start - this.maxDurNs /* - durNs */} and
        ts <= ${slicesKey.end /* + durNs */}
      group by ${maybeGroupByDepth} tsq
      order by tsq),
    q2 as (
      select
        ${queryTsq} as tsq,
        ${queryTsqEnd} as tsqEnd,
        ts,
        -1 as dur,
        id,
        ${depthCol}
        ${extraCols ? ',' + extraCols : ''}
      from ${this.tableName}
      where dur = -1
      group by ${maybeGroupByDepth} tsq
      )
      select min(dur) as _unused, * from
      (select * from q1 union all select * from q2)
      group by ${maybeGroupByDepth} tsq
      order by tsq
    `);

    // Here convert each row to a Slice. We do what we can do
    // generically in the base class, and delegate the rest to the impl
    // via that rowToSlice() abstract call.
    const slices = new Array<CastInternal<T['slice']>>(queryRes.numRows());
    const it = queryRes.iter(this.getRowSpec());

    let maxDataDepth = this.maxDataDepth;
    this.slicesKey = slicesKey;
    for (let i = 0; it.valid(); it.next(), ++i) {
      maxDataDepth = Math.max(maxDataDepth, it.depth);
      // Construct the base slice. The Impl will construct and return
      // the full derived T["slice"] (e.g. CpuSlice) in the
      // rowToSlice() method.
      slices[i] = this.rowToSliceInternal(it);
    }
    this.maxDataDepth = maxDataDepth;
    this.onUpdatedSlices(slices);
    this.cache.insert(slicesKey, slices);
    this.slices = slices;

    this.sqlState = 'QUERY_DONE';
    globals.rafScheduler.scheduleRedraw();
  }

  private rowToSliceInternal(row: T['row']): CastInternal<T['slice']> {
    const slice = this.rowToSlice(row) as CastInternal<T['slice']>;
    slice.x = -1;
    slice.w = -1;
    return slice;
  }

  rowToSlice(row: T['row']): T['slice'] {
    const startNsQ = row.tsq;
    const endNsQ = row.tsqEnd;
    let flags = 0;
    if (row.dur === -1) {
      flags |= SLICE_FLAGS_INCOMPLETE;
    } else if (row.dur === 0) {
      flags |= SLICE_FLAGS_INSTANT;
    }

    return {
      id: row.id,
      start: tpTimeFromNanos(startNsQ),
      duration: tpDurationFromNanos(endNsQ - startNsQ),
      flags,
      depth: row.depth,
      title: '',
      subTitle: '',

      // The derived class doesn't need to initialize these. They are
      // rewritten on every renderCanvas() call. We just need to initialize
      // them to something.
      baseColor: DEFAULT_SLICE_COLOR,
      color: DEFAULT_SLICE_COLOR,
    };
  }

  private findSlice({x, y}: {x: number, y: number}): undefined|Slice {
    const trackHeight = this.computedTrackHeight;
    const sliceHeight = this.computedSliceHeight;
    const padding = this.sliceLayout.padding;
    const rowSpacing = this.computedRowSpacing;

    // Need at least a draw pass to resolve the slice layout.
    if (sliceHeight === 0) {
      return undefined;
    }

    if (y >= padding && y <= trackHeight - padding) {
      const depth = Math.floor((y - padding) / (sliceHeight + rowSpacing));
      for (const slice of this.slices) {
        if (slice.depth === depth && slice.x <= x && x <= slice.x + slice.w) {
          return slice;
        }
      }
    }

    return undefined;
  }

  onMouseMove(position: {x: number, y: number}): void {
    this.hoverPos = position;
    this.updateHoveredSlice(this.findSlice(position));
  }

  onMouseOut(): void {
    this.updateHoveredSlice(undefined);
  }

  private updateHoveredSlice(slice?: T['slice']): void {
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
      const args: OnSliceOverArgs<T['slice']> = {slice: this.hoveredSlice};
      globals.dispatch(
          Actions.setHighlightedSliceId({sliceId: this.hoveredSlice.id}));
      this.onSliceOver(args);
      this.hoverTooltip = args.tooltip || [];
    }
  }

  onMouseClick(position: {x: number, y: number}): boolean {
    const slice = this.findSlice(position);
    if (slice === undefined) {
      return false;
    }
    const args: OnSliceClickArgs<T['slice']> = {slice};
    this.onSliceClick(args);
    return true;
  }

  private getVisibleSlicesInternal(start: TPTime, end: TPTime):
      Array<CastInternal<T['slice']>> {
    return filterVisibleSlices<CastInternal<T['slice']>>(
        this.slices, start, end);
  }

  private updateSliceAndTrackHeight() {
    const lay = this.sliceLayout;

    const rows =
        Math.min(Math.max(this.maxDataDepth + 1, lay.minDepth), lay.maxDepth);

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
      ctx: CanvasRenderingContext2D, x: number, y: number, h: number) {
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
    ctx.moveTo(midX, y);                              // A.
    ctx.lineTo(x + CHEVRON_WIDTH_PX, y + h);          // B.
    ctx.lineTo(midX, y + h - HALF_CHEVRON_WIDTH_PX);  // C.
    ctx.lineTo(x, y + h);                             // D.
    ctx.lineTo(midX, y);                              // Back to A.
    ctx.closePath();
    ctx.fill();
  }

  // This is a good default implementation for highlighting slices. By default
  // onUpdatedSlices() calls this. However, if the XxxSliceTrack impl overrides
  // onUpdatedSlices() this gives them a chance to call the highlighting without
  // having to reimplement it.
  protected highlightHovererdAndSameTitle(slices: Slice[]) {
    for (const slice of slices) {
      const isHovering = globals.state.highlightedSliceId === slice.id ||
          (this.hoveredSlice && this.hoveredSlice.title === slice.title);
      if (isHovering) {
        slice.color = {
          c: slice.baseColor.c,
          h: slice.baseColor.h,
          s: slice.baseColor.s,
          l: 30,
        };
      } else {
        slice.color = slice.baseColor;
      }
    }
  }

  getHeight(): number {
    this.updateSliceAndTrackHeight();
    return this.computedTrackHeight;
  }

  getSliceRect(_tStart: TPTime, _tEnd: TPTime, _depth: number): SliceRect
      |undefined {
    // TODO(hjd): Implement this as part of updating flow events.
    return undefined;
  }
}

// This is the argument passed to onSliceOver(args).
// This is really a workaround for the fact that TypeScript doesn't allow
// inner types within a class (whether the class is templated or not).
export interface OnSliceOverArgs<S extends Slice> {
  // Input args (BaseSliceTrack -> Impl):
  slice: S;  // The slice being hovered.

  // Output args (Impl -> BaseSliceTrack):
  tooltip?: string[];  // One entry per row, up to a max of 2.
}

export interface OnSliceOutArgs<S extends Slice> {
  // Input args (BaseSliceTrack -> Impl):
  slice: S;  // The slice which is not hovered anymore.
}

export interface OnSliceClickArgs<S extends Slice> {
  // Input args (BaseSliceTrack -> Impl):
  slice: S;  // The slice which is clicked.
}

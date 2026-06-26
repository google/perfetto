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
import {search} from '../../base/binary_search';
import {QuerySlot, SerialTaskQueue} from '../../base/query_slot';
import {Time} from '../../base/time';
import {Icons} from '../../base/semantic_icons';
import type {Trace} from '../../public/trace';
import type {TrackRenderContext, TrackRenderer} from '../../public/track';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';

const TRACK_HEIGHT = 100;
// Height used when the filmstrip content is hidden: the default minimum track
// height, so the track collapses to a thin strip showing nothing.
const HIDDEN_TRACK_HEIGHT = 18;
// Aspect ratio (w/h) used before any image has loaded. Replaced as soon as the
// first thumbnail's natural dimensions are known.
const DEFAULT_ASPECT = 16 / 9;

interface ScreenshotIndex {
  // Sorted by ts.
  ids: Int32Array;
  timestamps: BigInt64Array;
}

interface ImageEntry {
  // 'loading' while the query is in-flight; 'failed' on error.
  state: 'loading' | 'ready' | 'failed';
  image?: HTMLImageElement;
}

// Shared machinery for the screenshot filmstrip tracks: a single slice spanning
// from the first screenshot in the trace to the end of the trace, filled with
// screenshot thumbnails. Subclasses differ only in how the thumbnails are laid
// out within the slice (see render()).
abstract class ScreenshotFilmstripBase implements TrackRenderer {
  private readonly queue = new SerialTaskQueue();
  private readonly indexSlot = new QuerySlot<ScreenshotIndex>(this.queue);

  // Cache of decoded images, keyed by slice id. Lives for the track lifetime.
  private readonly images = new Map<number, ImageEntry>();
  // Aspect ratio of the screenshots in this trace; assumed uniform. Learned
  // from the first decoded image, falls back to DEFAULT_ASPECT until then.
  protected aspect = DEFAULT_ASPECT;

  // When true, the filmstrip content is hidden and the track collapses to the
  // default height. Toggled via the track shell button.
  private contentHidden = false;

  constructor(protected readonly trace: Trace) {}

  getHeight(): number {
    return this.contentHidden ? HIDDEN_TRACK_HEIGHT : TRACK_HEIGHT;
  }

  getTrackShellButtons(): m.Children {
    return m(Button, {
      className: 'pf-visible-on-hover',
      icon: this.contentHidden ? 'visibility' : Icons.Hide,
      tooltip: this.contentHidden ? 'Show screenshots' : 'Hide screenshots',
      compact: true,
      onclick: () => {
        this.contentHidden = !this.contentHidden;
        this.trace.raf.scheduleFullRedraw();
      },
    });
  }

  private async loadIndex(): Promise<ScreenshotIndex> {
    const res = await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE android.screenshots;
      SELECT id, ts FROM android_screenshots ORDER BY ts
    `);
    const ids = new Int32Array(res.numRows());
    const timestamps = new BigInt64Array(res.numRows());
    const it = res.iter({id: NUM, ts: LONG});
    for (let i = 0; it.valid(); it.next(), i++) {
      ids[i] = it.id;
      timestamps[i] = it.ts;
    }
    return {ids, timestamps};
  }

  protected requestImage(id: number): ImageEntry {
    let entry = this.images.get(id);
    if (entry !== undefined) return entry;
    entry = {state: 'loading'};
    this.images.set(id, entry);
    (async () => {
      try {
        const result = await this.trace.engine.query(`
          SELECT extract_arg(arg_set_id, 'screenshot.jpg_image') AS image_data
          FROM slice WHERE id = ${id}
        `);
        const row = result.firstRow({image_data: STR});
        const img = new Image();
        img.onload = () => {
          entry!.state = 'ready';
          entry!.image = img;
          if (img.naturalWidth > 0 && img.naturalHeight > 0) {
            this.aspect = img.naturalWidth / img.naturalHeight;
          }
          this.trace.raf.scheduleCanvasRedraw();
        };
        img.onerror = () => {
          entry!.state = 'failed';
          this.trace.raf.scheduleCanvasRedraw();
        };
        img.src = 'data:image/png;base64,' + row.image_data;
      } catch {
        entry!.state = 'failed';
        this.trace.raf.scheduleCanvasRedraw();
      }
    })();
    return entry;
  }

  // Return the index of the screenshot whose ts is closest to `t`, or -1 if
  // the index is empty.
  protected nearestIndex(idx: ScreenshotIndex, t: bigint): number {
    const n = idx.timestamps.length;
    if (n === 0) return -1;
    const left = search(idx.timestamps, t);
    if (left === -1) return 0;
    if (left + 1 >= n) return n - 1;
    const dLeft = t - idx.timestamps[left];
    const dRight = idx.timestamps[left + 1] - t;
    return dRight < dLeft ? left + 1 : left;
  }

  render(rc: TrackRenderContext): void {
    // When hidden, draw nothing; the track is collapsed to HIDDEN_TRACK_HEIGHT.
    if (this.contentHidden) return;

    const {ctx, size, colors, timescale} = rc;

    const indexResult = this.indexSlot.use({
      key: {kind: 'screenshots-index'},
      queryFn: () => this.loadIndex(),
    });
    const idx = indexResult.data;
    if (idx === undefined || idx.ids.length === 0) return;

    const width = size.width;
    const h = TRACK_HEIGHT;
    if (width <= 0) return;

    // The slice spans from the first screenshot to the end of the trace.
    const startPx = timescale.timeToPx(Time.fromRaw(idx.timestamps[0]));
    const endPx = timescale.timeToPx(this.trace.traceInfo.end);

    ctx.save();
    ctx.beginPath();
    ctx.rect(0, 0, width, h);
    ctx.clip();

    // The slice body, used as a backdrop while thumbnails load.
    const sliceLeft = Math.max(0, startPx);
    const sliceRight = Math.min(width, endPx);
    if (sliceRight > sliceLeft) {
      ctx.fillStyle = colors.COLOR_ACCENT;
      ctx.fillRect(sliceLeft, 0, sliceRight - sliceLeft, h);
    }

    this.drawThumbnails(rc, idx, startPx, endPx);

    ctx.restore();
  }

  // Fills the on-screen portion of the slice [startPx, endPx] with screenshot
  // thumbnails. Implemented by subclasses; the canvas is already clipped to the
  // track and the slice backdrop has been drawn.
  protected abstract drawThumbnails(
    rc: TrackRenderContext,
    idx: ScreenshotIndex,
    startPx: number,
    endPx: number,
  ): void;
}

// Time-anchored filmstrip: thumbnails are tiled edge-to-edge starting at the
// slice start, so each thumb is pinned to a region of *time*. Panning feels
// natural (frames travel with the trace), but zooming makes the strip race
// sideways because the grid origin (the far-off slice start) moves fast in
// pixel terms under zoom.
export class ScreenshotSpanTrack extends ScreenshotFilmstripBase {
  protected drawThumbnails(
    rc: TrackRenderContext,
    idx: ScreenshotIndex,
    startPx: number,
    endPx: number,
  ): void {
    const {ctx, size, colors, timescale} = rc;
    const width = size.width;
    const h = TRACK_HEIGHT;

    // Each thumb keeps the screenshot's natural aspect ratio at the track's full
    // height, so the zoom level (px per thumb) controls how many fit. Each slot
    // picks the screenshot nearest its centre.
    const thumbW = Math.max(1, Math.round(h * this.aspect));
    let lastDrawnId = -1;
    let lastDrawnImage: HTMLImageElement | undefined;
    for (let x = startPx; x < endPx && x < width; x += thumbW) {
      if (x + thumbW <= 0) continue; // Slot fully off the left edge.
      const centerTs = timescale.pxToHpTime(x + thumbW / 2).toTime();
      const i = this.nearestIndex(idx, centerTs);
      if (i < 0) continue;
      const id = idx.ids[i];

      // Use this slot's screenshot if it has decoded; otherwise (still loading,
      // failed, or a duplicate of the previous slot) carry the most recently
      // drawn image forward so the strip stays continuous instead of flashing
      // the blue backdrop.
      if (id !== lastDrawnId) {
        const entry = this.requestImage(id);
        if (entry.state === 'ready' && entry.image) {
          lastDrawnImage = entry.image;
          lastDrawnId = id;
        }
      }
      if (lastDrawnImage) {
        ctx.drawImage(lastDrawnImage, x, 0, thumbW, h);
      }
    }

    // Hairline separators between thumb slots.
    ctx.strokeStyle = colors.COLOR_BACKGROUND;
    for (let x = startPx + thumbW; x < endPx && x < width; x += thumbW) {
      if (x <= 0) continue;
      ctx.beginPath();
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, h);
      ctx.stroke();
    }
  }
}

// Screen-anchored filmstrip (scrubber-style): thumb slots are fixed in screen
// space, tiled from the left edge of the on-screen slice rightward. The slots
// never move under pan/zoom; instead the *content* of each slot updates to the
// screenshot nearest the time currently under that slot. Visually rock-stable,
// at the cost of frames swapping in place rather than travelling with the trace
// (like a video player's hover-scrubber storyboard).
export class ScreenshotScrubberTrack extends ScreenshotFilmstripBase {
  protected drawThumbnails(
    rc: TrackRenderContext,
    idx: ScreenshotIndex,
    startPx: number,
    endPx: number,
  ): void {
    const {ctx, size, colors, timescale} = rc;
    const width = size.width;
    const h = TRACK_HEIGHT;

    const thumbW = Math.max(1, Math.round(h * this.aspect));

    // The grid is anchored to screen space: it starts at the left edge of the
    // on-screen slice (clamped to x=0 so it fills from the viewport's left edge
    // once the slice start has scrolled off), and tiles right to the on-screen
    // slice end. Because slot positions depend only on the clamped slice
    // bounds, they stay still under pan/zoom; only the image inside each slot
    // changes as time scrolls beneath it.
    const left = Math.max(0, startPx);
    const right = Math.min(width, endPx);

    let lastDrawnId = -1;
    let lastDrawnImage: HTMLImageElement | undefined;
    for (let x = left; x < right; x += thumbW) {
      // Time at the centre of this slot, clamped to the slot's on-screen extent
      // so the rightmost partial slot still samples a sensible time.
      const centerX = Math.min(x + thumbW / 2, right);
      const centerTs = timescale.pxToHpTime(centerX).toTime();
      const i = this.nearestIndex(idx, centerTs);
      if (i < 0) continue;
      const id = idx.ids[i];

      if (id !== lastDrawnId) {
        const entry = this.requestImage(id);
        if (entry.state === 'ready' && entry.image) {
          lastDrawnImage = entry.image;
          lastDrawnId = id;
        }
      }
      if (lastDrawnImage) {
        // Clip the rightmost slot to the slice edge so thumbnails don't spill
        // past the (virtual) slice into empty space.
        const slotW = Math.min(thumbW, right - x);
        ctx.save();
        ctx.beginPath();
        ctx.rect(x, 0, slotW, h);
        ctx.clip();
        ctx.drawImage(lastDrawnImage, x, 0, thumbW, h);
        ctx.restore();
      }
    }

    // Hairline separators between thumb slots.
    ctx.strokeStyle = colors.COLOR_BACKGROUND;
    for (let x = left + thumbW; x < right; x += thumbW) {
      ctx.beginPath();
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, h);
      ctx.stroke();
    }
  }
}

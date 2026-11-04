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
import {range} from '../../base/array_utils';
import {ensureIsInstance} from '../../base/assert';
import {search} from '../../base/binary_search';
import {Time, type time} from '../../base/time';
import type {MinimapMode, MinimapRenderContext} from '../../public/minimap';
import type {Trace} from '../../public/trace';
import {
  drawVideoFrame,
  type FrameInfo,
  type VideoFramePlayer,
} from './video_frame_player';

// Upper bound on the number of thumbnails per stream, which bounds decoding
// work and memory for long recordings. Shorter streams keep every frame.
const MAX_THUMBNAILS = 256;
// Thumbnails fit within a square of this size.
const THUMBNAIL_SIZE_PX = 160;
const TILE_PADDING_PX = 2;
const TILE_GAP_PX = 1;

interface Thumbnail {
  readonly frame: FrameInfo;
  // Source rectangle within the stream's `ThumbnailAtlas`.
  readonly sx: number;
  readonly sy: number;
  readonly w: number;
  readonly h: number;
}

// Packs all thumbnails for a stream onto a single offscreen <canvas> sprite
// sheet rather than creating one 2D <canvas> per thumbnail: Chromium backs
// each 2D <canvas> context with its own GPU resource provider, so hundreds of
// separate canvases thrash the GPU cache and trigger ReadPixels stalls.
class ThumbnailAtlas {
  readonly canvas = document.createElement('canvas');
  private ctx?: CanvasRenderingContext2D;
  private cols = 1;
  private slotW = 0;
  private slotH = 0;
  private count = 0;

  constructor(private readonly capacity: number) {}

  async add(
    frame: VideoFrame,
    frameInfo: FrameInfo,
  ): Promise<Thumbnail | undefined> {
    const fw = frame.displayWidth || frame.codedWidth;
    const fh = frame.displayHeight || frame.codedHeight;
    if (fw <= 0 || fh <= 0 || this.count >= this.capacity) {
      return undefined;
    }
    if (this.ctx === undefined) {
      const initScale = Math.min(
        1,
        THUMBNAIL_SIZE_PX / fw,
        THUMBNAIL_SIZE_PX / fh,
      );
      this.slotW = Math.max(1, Math.round(fw * initScale));
      this.slotH = Math.max(1, Math.round(fh * initScale));
      this.cols = Math.max(1, Math.ceil(Math.sqrt(this.capacity)));
      const rows = Math.max(1, Math.ceil(this.capacity / this.cols));
      this.canvas.width = this.cols * this.slotW;
      this.canvas.height = rows * this.slotH;
      const ctx = this.canvas.getContext('2d');
      if (ctx === null) return undefined;
      this.ctx = ctx;
    }
    const scale = Math.min(1, this.slotW / fw, this.slotH / fh);
    const w = Math.max(1, Math.round(fw * scale));
    const h = Math.max(1, Math.round(fh * scale));
    const sx = (this.count % this.cols) * this.slotW;
    const sy = Math.floor(this.count / this.cols) * this.slotH;
    this.count++;
    await drawVideoFrame(this.ctx, frame, sx, sy, w, h);
    return {frame: frameInfo, sx, sy, w, h};
  }

  dispose() {
    this.canvas.width = 0;
    this.canvas.height = 0;
    this.ctx = undefined;
  }
}

// A minimap mode showing a filmstrip of the frames of one video stream, like
// the screenshots strip in Chrome DevTools' performance panel.
export class VideoMinimapMode implements MinimapMode {
  readonly heightPx = 100;
  private atlas?: ThumbnailAtlas;
  // Sorted by frame timestamp, with the timestamps kept in a parallel array
  // for binary search.
  private readonly thumbnails: Thumbnail[] = [];
  private readonly thumbnailTs: time[] = [];
  // Timestamps of all frames of the stream, for binary search.
  private frameTs: time[] = [];
  // Set once all thumbnails are decoded.
  private lastFrameTs?: time;
  // Last frame drawn onto the hover tooltip <canvas>, avoiding redundant 2D
  // blits while the pointer moves within the same frame's span.
  private tooltipFrameId?: number;

  constructor(
    private readonly trace: Trace,
    private readonly player: VideoFramePlayer,
    readonly id: string,
    readonly name: string,
    readonly priority: number,
  ) {}

  // Decodes thumbnails in the background and registers this mode with the
  // minimap once there is something to show, so streams that can't be
  // decoded (e.g. no WebCodecs, unsupported codec) leave the activity
  // overview in place. Stops when the trace is closed.
  async load(): Promise<void> {
    const abort = new AbortController();
    this.trace.trash.defer(() => {
      abort.abort();
      this.thumbnails.length = 0;
      this.thumbnailTs.length = 0;
      this.atlas?.dispose();
      this.atlas = undefined;
    });
    try {
      await this.decodeThumbnails(abort.signal);
    } catch (e) {
      // Queries fail once the trace is closed; that's expected.
      if (!abort.signal.aborted) throw e;
    }
  }

  private async decodeThumbnails(signal: AbortSignal) {
    await this.player.ensureFramesLoaded();
    if (signal.aborted) return;
    const frames = this.player.frames;
    this.frameTs = frames.map((f) => Time.fromRaw(f.ts));
    const indices = thumbnailIndices(frames);
    if (indices.length === 0) return;
    const atlas = new ThumbnailAtlas(indices.length);
    this.atlas = atlas;
    await this.player.decodeFrames(
      indices,
      async (idx, frame) => {
        try {
          const frameInfo = frames[idx];
          if (frameInfo === undefined) return;
          const thumb = await atlas.add(frame, frameInfo);
          if (thumb !== undefined && !signal.aborted) this.addThumbnail(thumb);
        } finally {
          frame.close();
        }
      },
      signal,
    );
    if (!signal.aborted && this.thumbnails.length > 0) {
      this.lastFrameTs = this.frameTs[this.frameTs.length - 1];
      this.trace.raf.scheduleCanvasRedraw();
    }
  }

  private addThumbnail(thumb: Thumbnail) {
    this.thumbnails.push(thumb);
    this.thumbnailTs.push(Time.fromRaw(thumb.frame.ts));
    // Two thumbnails span a time range, so there is a filmstrip to draw.
    if (this.thumbnails.length === 2) {
      this.trace.minimap.registerMode(this);
      m.redraw();
    }
    this.trace.raf.scheduleCanvasRedraw();
  }

  render({ctx, timescale, rect}: MinimapRenderContext) {
    const atlas = this.atlas;
    const first = this.thumbnails[0];
    const firstTs = this.thumbnailTs[0];
    const last = this.thumbnails[this.thumbnails.length - 1];
    const endTs = this.endTs;
    const tileH = rect.height - 2 * TILE_PADDING_PX;
    if (
      atlas === undefined ||
      first === undefined ||
      firstTs === undefined ||
      last === undefined ||
      endTs === undefined ||
      tileH <= 0
    ) {
      return;
    }

    const tileW = Math.max(1, Math.round((tileH * first.w) / first.h));
    const tiles = tileLayout(
      timescale.timeToPx(firstTs),
      timescale.timeToPx(endTs),
      rect.right,
      tileW,
    );
    const y = rect.top + TILE_PADDING_PX;
    // Each tile shows the frame on screen at its left edge, except the last
    // tile which shows the last frame.
    tiles.forEach((x, i) => {
      const thumb =
        i === tiles.length - 1
          ? last
          : (this.thumbnailAt(timescale.pxToHpTime(x).toTime()) ?? first);
      ctx.drawImage(
        atlas.canvas,
        thumb.sx,
        thumb.sy,
        thumb.w,
        thumb.h,
        x,
        y,
        tileW,
        tileH,
      );
    });
  }

  renderTooltip(ts: time): m.Children {
    const atlas = this.atlas;
    const thumb = this.thumbnailAt(ts);
    if (atlas === undefined || thumb === undefined) return undefined;
    const draw = ({dom}: m.VnodeDOM, forceRedraw: boolean) => {
      if (!forceRedraw && this.tooltipFrameId === thumb.frame.id) return;
      const canvas = ensureIsInstance(dom, HTMLCanvasElement);
      if (canvas.width !== thumb.w) canvas.width = thumb.w;
      if (canvas.height !== thumb.h) canvas.height = thumb.h;
      canvas
        .getContext('2d')
        ?.drawImage(
          atlas.canvas,
          thumb.sx,
          thumb.sy,
          thumb.w,
          thumb.h,
          0,
          0,
          thumb.w,
          thumb.h,
        );
      this.tooltipFrameId = thumb.frame.id;
    };
    return [
      m('canvas.pf-video-frame-tooltip__img', {
        oncreate: (vnode) => draw(vnode, true),
        onupdate: (vnode) => draw(vnode, false),
        onremove: ({dom}) => {
          const canvas = ensureIsInstance(dom, HTMLCanvasElement);
          canvas.width = 0;
          canvas.height = 0;
          this.tooltipFrameId = undefined;
        },
      }),
      `Frame ${thumb.frame.frameNumber}`,
    ];
  }

  // Selects the frame on screen at `ts`, which on long streams may not have
  // a thumbnail.
  onClick(ts: time) {
    if (this.thumbnailAt(ts) === undefined) return;
    const frame = this.player.frames[search(this.frameTs, ts)];
    if (frame === undefined) return;
    this.trace.selection.selectTrackEvent(this.player.trackUri, frame.id);
  }

  // The frame on screen at `ts`. The last frame stays on screen until the end
  // of the trace, but while decoding nothing is known past the latest
  // thumbnail.
  private thumbnailAt(ts: time): Thumbnail | undefined {
    const endTs = this.endTs;
    if (endTs === undefined) return undefined;
    if (this.lastFrameTs === undefined && ts > endTs) return undefined;
    return this.thumbnails[search(this.thumbnailTs, ts)];
  }

  // Where the filmstrip ends: the last frame, or the latest decoded thumbnail
  // while decoding.
  private get endTs(): time | undefined {
    return this.lastFrameTs ?? this.thumbnailTs[this.thumbnailTs.length - 1];
  }
}

// Returns the left edges of filmstrip tiles spread evenly from `startPx` (the
// first frame) to one tile past `endPx` (the last frame), so the last frame is
// shown whole. The strip is shifted left where needed to stay within `maxPx`.
export function tileLayout(
  startPx: number,
  endPx: number,
  maxPx: number,
  tileW: number,
): number[] {
  const right = Math.min(endPx + tileW, maxPx);
  const left = Math.min(startPx, right - tileW);
  const count = Math.max(
    1,
    Math.floor((right - left + TILE_GAP_PX) / (tileW + TILE_GAP_PX)),
  );
  const step = count > 1 ? (right - left - tileW) / (count - 1) : 0;
  return range(count).map((i) => Math.round(left + i * step));
}

// Picks up to MAX_THUMBNAILS frames spread evenly over the decodable part of
// the stream (from the first key frame), ending with the last frame. Long
// streams are split into equal slots, using a key frame from each slot where
// possible: key frames decode on their own, whereas other frames need their
// whole GOP decoded first.
export function thumbnailIndices(
  frames: ReadonlyArray<Pick<FrameInfo, 'isKey'>>,
): number[] {
  const keys = range(frames.length).filter((i) => frames[i]?.isKey);
  const first = keys[0];
  if (first === undefined) return [];
  const available = frames.length - first;
  const count = Math.min(MAX_THUMBNAILS, available);
  const indices = range(count).map((i) => {
    const slotStart = first + Math.floor((i * available) / count);
    const slotEnd = first + Math.floor(((i + 1) * available) / count);
    // The first key frame at or after the start of the slot.
    const key = keys[search(keys, slotStart - 1) + 1];
    return key !== undefined && key < slotEnd ? key : slotStart;
  });
  indices[count - 1] = frames.length - 1;
  return indices;
}

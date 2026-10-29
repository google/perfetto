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
  type FrameInfo,
  frameToCanvas,
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
  readonly image: HTMLCanvasElement;
}

// A minimap mode showing a filmstrip of the frames of one video stream, like
// the screenshots strip in Chrome DevTools' performance panel.
export class VideoMinimapMode implements MinimapMode {
  readonly heightPx = 100;
  // Sorted by frame timestamp, with the timestamps kept in a parallel array
  // for binary search.
  private readonly thumbnails: Thumbnail[] = [];
  private readonly thumbnailTs: time[] = [];
  // Timestamps of all frames of the stream, for binary search.
  private frameTs: time[] = [];
  // Set once all thumbnails are decoded.
  private lastFrameTs?: time;

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
    this.trace.trash.defer(() => abort.abort());
    try {
      await this.decodeThumbnails(abort.signal);
    } catch (e) {
      // Queries fail once the trace is closed; that's expected.
      if (!abort.signal.aborted) throw e;
    }
  }

  private async decodeThumbnails(signal: AbortSignal) {
    await this.player.ensureFramesLoaded();
    const frames = this.player.frames;
    this.frameTs = frames.map((f) => Time.fromRaw(f.ts));
    await this.player.decodeFrames(
      thumbnailIndices(frames),
      (idx, frame) => {
        const image = frameToCanvas(frame, THUMBNAIL_SIZE_PX);
        frame.close();
        if (image === undefined) return;
        this.addThumbnail({frame: frames[idx]!, image});
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
    const first = this.thumbnails[0];
    const last = this.thumbnails[this.thumbnails.length - 1];
    const tileH = rect.height - 2 * TILE_PADDING_PX;
    if (first === undefined || last === undefined || tileH <= 0) return;

    const {width: imgW, height: imgH} = first.image;
    const tileW = Math.max(1, Math.round((tileH * imgW) / imgH));
    const tiles = tileLayout(
      timescale.timeToPx(this.thumbnailTs[0]!),
      timescale.timeToPx(this.endTs),
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
      ctx.drawImage(thumb.image, x, y, tileW, tileH);
    });
  }

  renderTooltip(ts: time): m.Children {
    const thumb = this.thumbnailAt(ts);
    if (thumb === undefined) return undefined;
    const draw = ({dom}: m.VnodeDOM) => {
      const canvas = ensureIsInstance(dom, HTMLCanvasElement);
      canvas.getContext('2d')?.drawImage(thumb.image, 0, 0);
    };
    return [
      m('canvas.pf-video-frame-tooltip__img', {
        width: thumb.image.width,
        height: thumb.image.height,
        oncreate: draw,
        onupdate: draw,
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
    if (this.lastFrameTs === undefined && ts > this.endTs) return undefined;
    return this.thumbnails[search(this.thumbnailTs, ts)];
  }

  // Where the filmstrip ends: the last frame, or the latest decoded thumbnail
  // while decoding.
  private get endTs(): time {
    return this.lastFrameTs ?? this.thumbnailTs[this.thumbnailTs.length - 1]!;
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

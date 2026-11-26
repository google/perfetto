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

import {assertUnreachable} from '../../base/assert';
import {HSLColor} from '../../base/color';
import {Duration, Time, type duration, type time} from '../../base/time';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import {LONG, NUM} from '../../trace_processor/query_result';
import type {Size, VideoFramePlayer} from './video_frame_player';

// How long a touch stays on screen during playback, fading out with age.
export const PLAYBACK_TRAIL_DUR: duration = Duration.fromMillis(300);

// Smallest touch mark radius on screen, so marks stay visible in a small
// preview.
const MIN_RADIUS_CSS_PX = 6;

// MotionEvent.ACTION_* values, after masking off the pointer index.
const ACTION_DOWN = 0;
const ACTION_UP = 1;
const ACTION_MOVE = 2;
const ACTION_CANCEL = 3;
const ACTION_POINTER_DOWN = 5;
const ACTION_POINTER_UP = 6;

export type TouchPhase = 'down' | 'move' | 'up' | 'cancel';

// One pointer of one motion event, in logical display coordinates.
export interface TouchSample {
  readonly ts: time; // When the touch happened (kernel event time).
  readonly pointerId: number;
  readonly phase: TouchPhase;
  readonly x: number;
  readonly y: number;
}

// Maps logical display coordinates onto the decoded picture.
export interface PictureTransform {
  readonly scale: number;
  readonly offsetX: number;
  readonly offsetY: number;
}

// Half-open time range [start, end) of samples to draw.
export interface TimeWindow {
  readonly start: time;
  readonly end: time;
}

// One pointer's samples within a window, in time order.
export interface TouchStroke {
  readonly pointerId: number;
  // The samples in the window, preceded by the pointer's last sample before
  // the window (if it was still down) so the trail joins up across windows.
  readonly points: ReadonlyArray<TouchSample>;
}

// The picture shows the source display scaled to fit, preserving aspect
// ratio and centred, so after a rotation the display is letterboxed.
export function pictureTransform(
  source: Size,
  picture: Size,
): PictureTransform | undefined {
  if (source.width <= 0 || source.height <= 0) return undefined;
  if (picture.width <= 0 || picture.height <= 0) return undefined;
  const scale = Math.min(
    picture.width / source.width,
    picture.height / source.height,
  );
  return {
    scale,
    offsetX: (picture.width - source.width * scale) / 2,
    offsetY: (picture.height - source.height * scale) / 2,
  };
}

// Radius of a touch mark, in picture pixels. A fingertip is a fixed size on
// the display, so it's a fraction of the source display mapped onto the
// picture (smaller when letterboxed after a rotation), but at least
// `MIN_RADIUS_CSS_PX` on screen (`cssPx` picture pixels per CSS pixel).
export function touchRadius(
  source: Size,
  transform: PictureTransform,
  cssPx: number,
): number {
  const display = 0.012 * Math.min(source.width, source.height);
  return Math.max(MIN_RADIUS_CSS_PX * cssPx, display * transform.scale);
}

// What a pointer does in a motion event. POINTER_DOWN/UP only apply to the
// pointer at the action index; the event's other pointers just report their
// position. Returns undefined for non-touch actions (hover, scroll, ...).
export function touchPhase(
  actionMasked: number,
  actionPointerIndex: number,
  pointerIndex: number,
): TouchPhase | undefined {
  const isActionPointer = pointerIndex === actionPointerIndex;
  switch (actionMasked) {
    case ACTION_DOWN:
      return 'down';
    case ACTION_UP:
      return 'up';
    case ACTION_MOVE:
      return 'move';
    case ACTION_CANCEL:
      return 'cancel';
    case ACTION_POINTER_DOWN:
      return isActionPointer ? 'down' : 'move';
    case ACTION_POINTER_UP:
      return isActionPointer ? 'up' : 'move';
    default:
      return undefined;
  }
}

// Index of the first sample with ts >= t. `samples` must be sorted by ts.
export function lowerBound(
  samples: ReadonlyArray<{readonly ts: time}>,
  t: time,
): number {
  let lo = 0;
  let hi = samples.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    // mid < hi <= samples.length, so the index is in range.
    if (samples[mid]!.ts < t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// Paused on a frame: the touches made while it was on screen, i.e. until
// the next frame. The last frame stays on screen until the trace ends.
export function frameWindow(
  frameTs: time,
  nextFrameTs: time | undefined,
): TimeWindow {
  return {start: frameTs, end: nextFrameTs ?? Time.MAX};
}

// Playing at t: the touches in (t - PLAYBACK_TRAIL_DUR, t].
export function playbackWindow(t: time): TimeWindow {
  return {
    start: Time.add(Time.sub(t, PLAYBACK_TRAIL_DUR), 1n),
    end: Time.add(t, 1n),
  };
}

// Groups the samples in `window` into one stroke per gesture per pointer.
export function strokesInWindow(
  samples: ReadonlyArray<TouchSample>,
  window: TimeWindow,
): TouchStroke[] {
  const lo = lowerBound(samples, window.start);
  const hi = lowerBound(samples, window.end);
  const strokes: TouchStroke[] = [];
  // The stroke each pointer is currently drawing, until it lifts.
  const open = new Map<number, TouchSample[]>();
  for (const s of samples.slice(lo, hi)) {
    let points = open.get(s.pointerId);
    if (points === undefined || s.phase === 'down') {
      points = [];
      if (s.phase !== 'down') {
        const prev = previousSample(samples, lo, s.pointerId, window.start);
        if (prev !== undefined) points.push(prev);
      }
      strokes.push({pointerId: s.pointerId, points});
      open.set(s.pointerId, points);
    }
    points.push(s);
    if (s.phase === 'up' || s.phase === 'cancel') open.delete(s.pointerId);
  }
  return strokes;
}

// The pointer's last sample before index `before`, if the pointer was still
// down then. Looks back at most one playback trail, so it stays cheap.
function previousSample(
  samples: ReadonlyArray<TouchSample>,
  before: number,
  pointerId: number,
  windowStart: time,
): TouchSample | undefined {
  const limit = Time.sub(windowStart, PLAYBACK_TRAIL_DUR);
  for (let i = before - 1; i >= 0; i--) {
    const s = samples[i]!; // 0 <= i < before <= samples.length.
    if (s.ts < limit) return undefined;
    if (s.pointerId !== pointerId) continue;
    return s.phase === 'up' || s.phase === 'cancel' ? undefined : s;
  }
  return undefined;
}

// The displays with touch coordinates. Reads the intrinsic tables directly,
// so traces without input pay no stdlib module includes at load.
export async function queryTouchDisplays(engine: Engine): Promise<Set<number>> {
  const res = await engine.query(`
    SELECT DISTINCT e.display_id AS displayId
    FROM __intrinsic_android_motion_events e
    WHERE e.display_id IS NOT NULL
      AND e.id IN (
        SELECT motion_event_id
        FROM __intrinsic_android_motion_event_pointer
        WHERE x IS NOT NULL
      )
  `);
  const displays = new Set<number>();
  const it = res.iter({displayId: NUM});
  for (; it.valid(); it.next()) displays.add(it.displayId);
  return displays;
}

// Draws the touches on one display over its video preview: a second canvas
// stacked on the preview canvas, with the same buffer size, so it rescales
// with it. Repainted from the player's playback clock on every playback
// tick, and on every seek/pause, without touching the decoded picture.
export class TouchOverlay {
  // Whether to draw at all ("Show touches").
  enabled = true;

  private readonly trace: Trace;
  private readonly player: VideoFramePlayer;
  private readonly displayId: number;
  private samples: TouchSample[] = [];
  // Logical source display size per video frame id.
  private readonly sourceSizes = new Map<number, Size>();
  private loaded = false;
  private loading?: Promise<void>;
  private canvas?: HTMLCanvasElement;
  // Repaints when the preview is resized, as mark sizes have an on-screen
  // minimum that depends on the displayed size.
  private resizeObserver?: ResizeObserver;

  constructor(trace: Trace, player: VideoFramePlayer, displayId: number) {
    this.trace = trace;
    this.player = player;
    this.displayId = displayId;
    player.onRepaint = () => this.draw();
  }

  get isLoaded(): boolean {
    return this.loaded;
  }

  // The logical size of the source display when this frame was captured;
  // undefined for traces recorded before VideoFrame carried it.
  sourceSize(frameId: number): Size | undefined {
    return this.sourceSizes.get(frameId);
  }

  // Loads once; concurrent callers share the in-flight load.
  ensureLoaded(): Promise<void> {
    this.loading ??= this.load();
    return this.loading;
  }

  private async load(): Promise<void> {
    const engine = this.trace.engine;
    const sizes = await engine.query(`
      INCLUDE PERFETTO MODULE android.video_frames;
      SELECT id, source_width AS width, source_height AS height
      FROM android_video_frames
      WHERE display_id = ${this.displayId}
        AND source_width > 0 AND source_height > 0
    `);
    const sizeIt = sizes.iter({id: NUM, width: NUM, height: NUM});
    for (; sizeIt.valid(); sizeIt.next()) {
      this.sourceSizes.set(sizeIt.id, {
        width: sizeIt.width,
        height: sizeIt.height,
      });
    }
    // Synthesized events (InputDispatcher splits, cancels, ...) re-describe
    // the device's touches for some window; draw only what the device saw.
    const res = await engine.query(`
      INCLUDE PERFETTO MODULE android.input;
      SELECT
        COALESCE(event_ts, ts) AS ts,
        pointer_id AS pointerId,
        pointer_index AS pointerIndex,
        action_masked AS actionMasked,
        action_pointer_index AS actionPointerIndex,
        x,
        y
      FROM android_motion_event_pointers
      WHERE display_id = ${this.displayId}
        AND x IS NOT NULL AND y IS NOT NULL
        AND NOT COALESCE(is_synthesized, 0)
      ORDER BY ts, motion_event_id, pointer_index
    `);
    const it = res.iter({
      ts: LONG,
      pointerId: NUM,
      pointerIndex: NUM,
      actionMasked: NUM,
      actionPointerIndex: NUM,
      x: NUM,
      y: NUM,
    });
    const samples: TouchSample[] = [];
    for (; it.valid(); it.next()) {
      const phase = touchPhase(
        it.actionMasked,
        it.actionPointerIndex,
        it.pointerIndex,
      );
      if (phase === undefined) continue;
      samples.push({
        ts: Time.fromRaw(it.ts),
        pointerId: it.pointerId,
        phase,
        x: it.x,
        y: it.y,
      });
    }
    this.samples = samples;
    this.loaded = true;
    this.draw();
  }

  attachCanvas(c: HTMLCanvasElement): void {
    this.canvas = c;
    this.resizeObserver = new ResizeObserver(() => this.draw());
    this.resizeObserver.observe(c);
    this.draw();
  }

  detachCanvas(): void {
    this.resizeObserver?.disconnect();
    this.resizeObserver = undefined;
    this.canvas = undefined;
  }

  toggle(): void {
    this.enabled = !this.enabled;
    this.draw();
  }

  draw(): void {
    const c = this.canvas;
    if (c === undefined) return;
    const picture = this.player.frameSize;
    if (picture === undefined) return;
    if (c.width !== picture.width) c.width = picture.width;
    if (c.height !== picture.height) c.height = picture.height;
    const ctx = c.getContext('2d');
    if (ctx === null) return;
    ctx.clearRect(0, 0, c.width, c.height);

    const frame = this.player.currentFrame;
    if (!this.enabled || frame === undefined) return;
    const source = this.sourceSizes.get(frame.id);
    if (source === undefined) return;
    const transform = pictureTransform(source, picture);
    if (transform === undefined) return;

    const next = this.player.frames[this.player.currentIdx + 1];
    const nextTs = next && Time.fromRaw(next.ts);
    // While playing, follow the playback clock, but never past what the
    // video shows: if decoding lags, the clock runs ahead of the painted
    // frame, and touches would appear before the frame they belong to.
    const clock = this.player.playbackTs;
    const now =
      clock === undefined || nextTs === undefined
        ? clock
        : Time.min(clock, Time.sub(nextTs, 1n));
    const window =
      now === undefined
        ? frameWindow(Time.fromRaw(frame.ts), nextTs)
        : playbackWindow(now);
    // Picture pixels per CSS pixel, as the canvas is scaled to fit its box,
    // so marks stay visible however small the preview is shown.
    const fit = Math.min(c.clientWidth / c.width, c.clientHeight / c.height);
    const cssPx = fit > 0 ? 1 / fit : 1;
    const strokes = strokesInWindow(this.samples, window);
    const radius = touchRadius(source, transform, cssPx);
    drawStrokes(ctx, strokes, transform, radius, cssPx, now);
  }
}

// Dark outline around touch-down dots, to keep them visible on white.
const OUTLINE = new HSLColor([0, 0, 0]);

// Backdrop of legend glyphs: the marks are drawn for a video picture.
const LEGEND_BACKDROP = new HSLColor([220, 10, 12]);

// The kinds of touch mark, as named in the legend.
export type TouchMark = 'down' | 'move' | 'still' | 'up' | 'second';

// The legend of the touch overlay, in display order.
export const TOUCH_LEGEND: ReadonlyArray<{
  readonly mark: TouchMark;
  readonly label: string;
}> = [
  {mark: 'down', label: 'finger down'},
  {mark: 'move', label: 'moved'},
  {mark: 'still', label: 'still down'},
  {mark: 'up', label: 'lifted'},
  {mark: 'second', label: '2nd finger'},
];

// How the overlay picks the touches to draw, for the legend.
export const TOUCH_TIME_RULE =
  'Paused: touches while this frame was on screen. ' +
  `Playing: last ${Duration.toMilliseconds(PLAYBACK_TRAIL_DUR)} ms.`;

// The size of the marks: dots have `radius`, lines `lineWidth`.
interface MarkSize {
  readonly radius: number;
  readonly lineWidth: number;
}

function css(color: HSLColor, alpha: number): string {
  return color.setAlpha(alpha).cssString;
}

// A trail segment of a moving pointer.
function drawSegment(
  ctx: CanvasRenderingContext2D,
  from: readonly [number, number],
  to: readonly [number, number],
  size: MarkSize,
  color: HSLColor,
  alpha: number,
): void {
  ctx.lineCap = 'round';
  ctx.lineWidth = size.lineWidth;
  ctx.strokeStyle = css(color, 0.6 * alpha);
  ctx.beginPath();
  ctx.moveTo(...from);
  ctx.lineTo(...to);
  ctx.stroke();
}

// Where a pointer went down: a filled dot with a thin dark outline.
function drawDown(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  size: MarkSize,
  color: HSLColor,
  alpha: number,
): void {
  ctx.beginPath();
  ctx.arc(x, y, size.radius, 0, 2 * Math.PI);
  ctx.fillStyle = css(color, 0.85 * alpha);
  ctx.fill();
  ctx.lineWidth = size.lineWidth / 2;
  ctx.strokeStyle = css(OUTLINE, 0.5 * alpha);
  ctx.stroke();
}

// Where a pointer lifted: a ring, `grow` (0-1) of the way through expanding.
function drawUp(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  size: MarkSize,
  color: HSLColor,
  alpha: number,
  grow: number,
): void {
  ctx.beginPath();
  ctx.arc(x, y, size.radius * (1 + 1.5 * grow), 0, 2 * Math.PI);
  // A slightly wider dark stroke under the ring, like the dot's outline,
  // keeps it readable on light backgrounds.
  ctx.lineWidth = size.lineWidth * 2;
  ctx.strokeStyle = css(OUTLINE, 0.4 * alpha);
  ctx.stroke();
  ctx.lineWidth = size.lineWidth;
  ctx.strokeStyle = css(color, 0.85 * alpha);
  ctx.stroke();
}

// Where a pointer that is still down is now: a small dot.
function drawStill(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  size: MarkSize,
  color: HSLColor,
  alpha: number,
): void {
  ctx.beginPath();
  ctx.arc(x, y, size.radius / 2, 0, 2 * Math.PI);
  ctx.fillStyle = css(color, 0.85 * alpha);
  ctx.fill();
}

// Paints the strokes, subtly: a trail per pointer, a dot where it went down,
// a ring where it lifted, and a small dot where it is while still down.
// While playing (`now` set), each mark fades out with age and lift rings
// expand; while paused everything is fully drawn.
function drawStrokes(
  ctx: CanvasRenderingContext2D,
  strokes: ReadonlyArray<TouchStroke>,
  transform: PictureTransform,
  radius: number,
  cssPx: number,
  now: time | undefined,
): void {
  const size = {radius, lineWidth: Math.max(cssPx, radius / 4)};
  const at = (s: TouchSample): [number, number] => [
    transform.offsetX + s.x * transform.scale,
    transform.offsetY + s.y * transform.scale,
  ];
  // 1 for a fresh (or paused) mark, down to 0 at the end of the trail.
  const freshness = (s: TouchSample) =>
    now === undefined
      ? 1
      : Math.max(0, 1 - Number(now - s.ts) / Number(PLAYBACK_TRAIL_DUR));

  for (const stroke of strokes) {
    const color = pointerColor(stroke.pointerId);
    let prev: TouchSample | undefined;
    for (const s of stroke.points) {
      if (prev !== undefined) {
        drawSegment(ctx, at(prev), at(s), size, color, freshness(s));
      }
      prev = s;
    }

    for (const s of stroke.points) {
      const alpha = freshness(s);
      if (alpha <= 0) continue;
      if (s.phase === 'down') {
        drawDown(ctx, ...at(s), size, color, alpha);
      } else if (s.phase === 'up') {
        const grow = now === undefined ? 0.5 : 1 - alpha;
        drawUp(ctx, ...at(s), size, color, alpha, grow);
      }
    }

    // Where a pointer that's still down is now, so a finger held still (or
    // just moving) stays visible on a paused frame.
    const head = stroke.points[stroke.points.length - 1];
    if (head?.phase === 'move') {
      drawStill(ctx, ...at(head), size, color, freshness(head));
    }
  }
}

// Draws one legend glyph, filling a `px` square, with the same code and
// colours as the overlay (as drawn on a paused frame).
export function drawLegendGlyph(
  ctx: CanvasRenderingContext2D,
  mark: TouchMark,
  px: number,
): void {
  ctx.fillStyle = LEGEND_BACKDROP.cssString;
  ctx.fillRect(0, 0, px, px);
  const size = {radius: px * 0.22, lineWidth: Math.max(1, px * 0.055)};
  const mid = px / 2;
  const first = pointerColor(0);
  switch (mark) {
    case 'down':
      drawDown(ctx, mid, mid, size, first, 1);
      break;
    case 'move':
      drawSegment(
        ctx,
        [px * 0.2, px * 0.8],
        [px * 0.8, px * 0.2],
        size,
        first,
        1,
      );
      break;
    case 'still':
      drawSegment(ctx, [px * 0.2, px * 0.8], [mid, mid], size, first, 1);
      drawStill(ctx, mid, mid, size, first, 1);
      break;
    case 'up':
      // The ring as drawn while paused, a bit smaller to fit.
      drawUp(
        ctx,
        mid,
        mid,
        {...size, radius: size.radius * 0.8},
        first,
        1,
        0.5,
      );
      break;
    case 'second':
      drawDown(ctx, mid, mid, size, pointerColor(1), 1);
      break;
    default:
      assertUnreachable(mark);
  }
}

// The first pointer is white; further pointers of a multi-touch gesture get
// distinct light hues, golden-angle steps apart so neighbouring ids differ.
function pointerColor(pointerId: number): HSLColor {
  if (pointerId === 0) return new HSLColor([0, 0, 100]);
  return new HSLColor([(pointerId * 137.5) % 360, 80, 70]);
}

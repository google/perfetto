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

import {Color} from './color';
import {Transform2D} from './geom';

// Flag bits for drawRect options
export const RECT_PATTERN_HATCHED = 1; // Draw diagonal crosshatch pattern
export const RECT_PATTERN_FADE_RIGHT = 2; // Fade alpha from full left to 0 right across width

// Describes a marker render function to customize the marker.
export type MarkerRenderFunc = (
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
) => void;

// Interface for a general renderer with 2D primitive drawing capabilities which
// can be implemented with different backends (e.g., WebGL, Canvas2D).
export interface Renderer {
  // Push a transform onto the stack. Offsets add, scales multiply.
  // Returns a disposable that restores the previous transform when disposed.
  // Use with `using`:
  //   using _ = renderer.pushTransform({offsetX: 10, offsetY: 20, scaleX: 1, scaleY: 1});
  pushTransform(transform: Partial<Transform2D>): Disposable;

  // Reset the transform to identity (no offset, scale=1).
  resetTransform(): void;

  // Clear the entire canvas.
  clear(): void;

  // Set a clipping rectangle in pixels. All subsequent draws will be clipped
  // to this region. Returns a disposable that restores the previous clip.
  // Use with `using`:
  //   using _ = renderer.clip(x, y, w, h);
  clip(x: number, y: number, w: number, h: number): Disposable;

  // Draw a single marker centered horizontally at the given position. A marker
  // is a sprite/glyph with fixed size in pixels regardless of the current
  // transform.
  drawMarker(
    x: number,
    y: number,
    w: number,
    h: number,
    color: Color,
    render: MarkerRenderFunc,
  ): void;

  // Draw a single rectangle.
  // left/right are in time units, top/bottom are in pixels.
  drawRect(
    left: number,
    top: number,
    right: number,
    bottom: number,
    color: Color,
    pattern?: number,
  ): void;

  // Draw a step-area chart (filled area under a step function).
  //
  // For each segment i, draws:
  // - A filled rectangle from ys[i] to baselineY with alpha fills[i]
  // - A 1px stroke line at ys[i] extending horizontally to xs[i+1]
  // - A vertical "wiggle" at xs[i] connecting the previous segment's y to
  //   minYs[i], maxYs[i], then ys[i] (visualizes min/max range at transitions)
  //
  // xs: x positions for each data point
  // ys: y positions for fill top and horizontal stroke line
  // minYs: minimum Y of the wiggle at each transition
  // maxYs: maximum Y of the wiggle at each transition
  // fills: fill alpha per segment (0 = transparent, 1 = filled)
  // count: number of data points
  // trackTop: Y coordinate of top of track (for WebGL quad bounds)
  // trackBottom: Y coordinate of bottom of track (for WebGL quad bounds)
  // baselineY: Y coordinate of baseline (bottom of fill region)
  // color: fill and stroke color
  drawStepArea(
    xs: ArrayLike<number>,
    ys: ArrayLike<number>,
    minYs: ArrayLike<number>,
    maxYs: ArrayLike<number>,
    fills: ArrayLike<number>,
    count: number,
    trackTop: number,
    trackBottom: number,
    baselineY: number,
    color: Color,
  ): void;

  // Flush all pending draw/marker calls to the underlying backend and
  // invalidate caches. Users should ensure that they call flush before
  // accessing the canvas2d context directly in order to synchronize draws and
  // avoid visual glitches. However, excessive flushing can degrade performance,
  // so it should be used judiciously. If possible, try to batch as many draw
  // calls together as possible inbetween flushes.
  flush(): void;
}

// Copyright (C) 2025 The Android Open Source Project
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

// Common interfaces for canvas rendering, shared between WebGL and Canvas2D
// implementations.

// 2D transformation (offset + scale). Transforms compound when pushed:
// - Offsets add: newOffset = currentOffset + transform.offset
// - Scales multiply: newScale = currentScale * transform.scale
// For time-to-pixel conversion, use scaleX as pixels-per-time-unit.
export interface Transform2D {
  offsetX: number; // Pixel offset in X
  offsetY: number; // Pixel offset in Y
  scaleX: number; // Scale factor for X (use as pxPerTime for time conversion)
  scaleY: number; // Scale factor for Y
}

export interface RGBA {
  r: number; // 0-255
  g: number; // 0-255
  b: number; // 0-255
  a: number; // 0-255
}

// Flag bits for drawRect options
export const RECT_FLAG_HATCHED = 1; // Draw diagonal crosshatch pattern
export const RECT_FLAG_FADEOUT = 2; // Fade alpha from full to 0 across width

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
    color: RGBA,
    render: MarkerRenderFunc,
  ): void;

  // Bulk draw markers centered horizontally at given positions.
  // A billboard is a sprite with fixed pixel dimensions regardless of scale.
  // x values are in time units, y values are in pixels.
  // render: Canvas2D fallback function (ignored by WebGL which uses SDF).
  drawMarkers(
    positions: Float32Array,
    sizes: Float32Array,
    colors: Uint8Array,
    count: number,
    render: MarkerRenderFunc,
  ): void;

  // Draw a single rectangle.
  // left/right are in time units, top/bottom are in pixels.
  drawRect(
    left: number,
    top: number,
    right: number,
    bottom: number,
    color: RGBA,
    flags?: number,
  ): void;

  // Bulk draw rectangles.
  // x values are in time units, y values are in pixels.
  drawRects(
    topLeft: Float32Array,
    bottomRight: Float32Array,
    colors: Uint8Array,
    count: number,
    flags?: Uint8Array,
  ): void;

  // Escape hatch to raw CanvasRenderingContext2D for custom drawing. Using this
  // instead of drawing manually to the canvas makes sure than any pipelining is
  // correctly flushed.
  raw(fn: (ctx: CanvasRenderingContext2D) => void): void;

  // Flush all pending draw calls to the GPU.
  flush(): void;
}

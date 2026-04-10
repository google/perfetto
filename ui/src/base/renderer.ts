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
import {Transform1D, Transform2D} from './geom';

// Flag bits for drawRect options
export const RECT_PATTERN_HATCHED = 1; // Draw diagonal crosshatch pattern
export const RECT_PATTERN_FADE_RIGHT = 2; // Fade alpha from full left to 0 right across width

// Describes the vertical layout of rows in a slice track using a two-tier
// formula. Row 0 can have a different height from the rest (e.g. for collapsed
// mode where row 0 stays full-size).
//
// depth == 0: top = paddingTop, height = firstRowHeight
// depth > 0:  top = paddingTop + firstRowHeight + rowGap
//                  + (depth - 1) * (rowHeight + rowGap)
//             height = rowHeight
export interface RowLayout {
  // Height of rows in CSS pixels.
  readonly rowHeight: number;
  // Offset from the top of the track to the first row, in CSS pixels. Defaults
  // to 0.
  readonly paddingTop?: number;
  // Height of the first row (row 0) in CSS pixels. Defaults to rowHeight.
  readonly firstRowHeight?: number;
  // Vertical gap between rows, in CSS pixels. Defaults to 0.
  readonly rowGap?: number;
}

// Compute the top Y position for a row at the given depth.
export function rowTopFromLayout(
  {
    paddingTop = 0,
    rowHeight,
    rowGap = 0,
    firstRowHeight = rowHeight,
  }: RowLayout,
  depth: number,
): number {
  if (depth === 0) return paddingTop;
  const stride = rowHeight + rowGap;
  return paddingTop + firstRowHeight + rowGap + (depth - 1) * stride;
}

// Compute the height for a row at the given depth.
export function rowHeightFromLayout(
  {rowHeight, firstRowHeight = rowHeight}: RowLayout,
  depth: number,
): number {
  return depth === 0 ? firstRowHeight : rowHeight;
}

// Buffers for batch slice rendering.
// Per-slice data: left, right, depth, color, pattern.
// Row layout defines how depth maps to vertical position via a formula.
// Start/end coordinates are in data space (e.g., nanoseconds) and transformed
// by the dataTransform passed to drawSlices.
export interface SliceBuffers {
  // Start (left edge) positions in data space (transformed by dataTransform)
  readonly starts: Float32Array;
  // End (right edge) positions in data space (transformed by dataTransform)
  readonly ends: Float32Array;
  // Depth (row index) per slice
  readonly depths: Uint16Array;
  // Packed RGBA colors (0xRRGGBBAA)
  readonly colors: Uint32Array;
  // Pattern flags per slice (0 = none, RECT_PATTERN_HATCHED, etc.)
  readonly patterns: Uint8Array;
  // Number of valid slices
  readonly count: number;
}

// Buffers for step-area chart data.
// Each array contains one element per data point.
// Values are in data space and transformed to screen coordinates by drawStepArea.
export interface StepAreaBuffers {
  // X positions for each data point
  readonly xs: Float32Array;
  // X positions for the next data point
  readonly xnext: Float32Array;
  // Y positions for fill top and horizontal stroke line
  readonly ys: Float32Array;
  // Minimum Y of the range indicator at each transition
  readonly minYs: Float32Array;
  // Maximum Y of the range indicator at each transition
  readonly maxYs: Float32Array;
  // Fill alpha per segment (0 = transparent, 1 = filled)
  readonly fillAlpha: Float32Array;
  // Number of valid data points in the arrays
  readonly count: number;
}

// Buffers for batch marker rendering.
// Markers are sprites/glyphs with fixed size in pixels (e.g., chevrons for instant events).
// X coordinates are in data space and transformed by dataTransform.
// Y coordinates, width, and height are in screen pixels.
export interface MarkerBuffers {
  // Center X coordinates (data space, transformed by dataTransform.scaleX/offsetX)
  readonly xs: Float32Array;
  // Depth (row index) per marker
  readonly depths: Uint16Array;
  // Packed RGBA colors (0xRRGGBBAA)
  readonly colors: Uint32Array;
  // Number of valid markers
  readonly count: number;
}

// Describes a marker render function to customize the marker (Canvas2D fallback).
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

  // Draw multiple markers from columnar buffers.
  // Markers are sprites/glyphs with fixed size in pixels (e.g., chevrons).
  // X coordinates are centered and in data space, transformed by xTransform.
  // render is a Canvas2D fallback function for when WebGL is unavailable.
  drawMarkers(
    buffers: MarkerBuffers,
    rowLayout: RowLayout,
    markerWidth: number,
    xTransform: Transform1D,
    render: MarkerRenderFunc,
  ): void;

  // Draw multiple slices from columnar buffers.
  // Each slice has a left, right, depth, color, and pattern.
  // The row layout maps depth to vertical position (top/bottom in CSS pixels).
  // xTransform converts X coordinates from data space to screen space.
  drawSlices(
    buffers: SliceBuffers,
    rowLayout: RowLayout,
    xTransform: Transform1D,
  ): void;

  // Draw a step-area chart (filled area under a step function).
  //
  // For each segment i, draws:
  // - A filled rectangle from ys[i] to baseline (y=0 in data space) with alpha fillAlpha[i]
  // - A 1px horizontal stroke line at ys[i] extending to xs[i+1]
  // - A vertical range indicator at xs[i] showing the min/max range at each transition
  //
  // buffers: the data arrays (xs, ys, minYs, maxYs, fillAlpha, count)
  // transform: coordinate transform - baseline is where y=0 maps to (transform.offsetY)
  // color: fill and stroke color
  // top: Y coordinate of top of rendering area (for WebGL quad bounds)
  // bottom: Y coordinate of bottom of rendering area (for WebGL quad bounds)
  drawStepArea(
    buffers: StepAreaBuffers,
    transform: Transform2D,
    color: Color,
    top: number,
    bottom: number,
  ): void;
}

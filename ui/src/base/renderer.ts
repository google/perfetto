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

// Buffers for batch rectangle rendering.
// All arrays must have the same length (count).
// Colors are packed as 0xRRGGBBAA (big-endian RGBA).
// X coordinates and widths are in data space (e.g., nanoseconds) and transformed
// by the dataTransform passed to drawRects. Y coordinates are in screen pixels.
export interface RectBuffers {
  // Left edge X coordinates (data space, transformed by dataTransform.scaleX/offsetX)
  readonly xs: Float32Array;
  // Top edge Y coordinates (screen pixels)
  readonly ys: Float32Array;
  // Widths (data space, transformed by dataTransform.scaleX)
  // Use -1 for incomplete rects that extend to screenEnd
  readonly ws: Float32Array;
  // Height in screen pixels (uniform for all rects)
  readonly h: number;
  // Packed RGBA colors (0xRRGGBBAA)
  readonly colors: Uint32Array;
  // Pattern flags per rect (0 = none, RECT_PATTERN_HATCHED, etc.)
  readonly patterns: Uint8Array;
  // Number of valid rects
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
  // Top edge Y coordinates (screen pixels)
  readonly ys: Float32Array;
  // Width in screen pixels (uniform for all markers)
  readonly w: number;
  // Height in screen pixels (uniform for all markers)
  readonly h: number;
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
  // X coordinates are centered and in data space, transformed by dataTransform.
  // render is a Canvas2D fallback function for when WebGL is unavailable.
  drawMarkers(
    buffers: MarkerBuffers,
    dataTransform: Transform2D,
    render: MarkerRenderFunc,
  ): void;

  // Draw multiple rectangles from columnar buffers.
  // This is more efficient than calling drawRect() in a loop.
  // Colors are packed as 0xRRGGBBAA (big-endian RGBA).
  // dataTransform converts buffer coordinates to screen coordinates.
  drawRects(buffers: RectBuffers, dataTransform: Transform2D): void;

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

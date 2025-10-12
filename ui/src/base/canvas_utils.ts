// Copyright (C) 2019 The Android Open Source Project
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
import {Size2D, Point2D} from './geom';

export function drawDoubleHeadedArrow(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  length: number,
  showArrowHeads: boolean,
  color: string,
  width = 2,
) {
  ctx.beginPath();
  ctx.lineWidth = width;
  ctx.lineCap = 'round';
  ctx.strokeStyle = color;
  ctx.moveTo(x, y);
  ctx.lineTo(x + length, y);
  ctx.stroke();
  ctx.closePath();
  // Arrowheads on the each end of the line.
  if (showArrowHeads) {
    ctx.beginPath();
    ctx.moveTo(x + length - 8, y - 4);
    ctx.lineTo(x + length, y);
    ctx.lineTo(x + length - 8, y + 4);
    ctx.stroke();
    ctx.closePath();
    ctx.beginPath();
    ctx.moveTo(x + 8, y - 4);
    ctx.lineTo(x, y);
    ctx.lineTo(x + 8, y + 4);
    ctx.stroke();
    ctx.closePath();
  }
}

export function drawIncompleteSlice(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  width: number,
  height: number,
  color: Color,
  showGradient: boolean = true,
) {
  if (width <= 0 || height <= 0) {
    return;
  }
  ctx.beginPath();
  const triangleSize = height / 4;
  ctx.moveTo(x, y);
  ctx.lineTo(x + width, y);
  ctx.lineTo(x + width - 3, y + triangleSize * 0.5);
  ctx.lineTo(x + width, y + triangleSize);
  ctx.lineTo(x + width - 3, y + triangleSize * 1.5);
  ctx.lineTo(x + width, y + 2 * triangleSize);
  ctx.lineTo(x + width - 3, y + triangleSize * 2.5);
  ctx.lineTo(x + width, y + 3 * triangleSize);
  ctx.lineTo(x + width - 3, y + triangleSize * 3.5);
  ctx.lineTo(x + width, y + 4 * triangleSize);
  ctx.lineTo(x, y + height);

  const originalFillStyle = ctx.fillStyle;

  if (showGradient) {
    const gradient = ctx.createLinearGradient(x, y, x + width, y + height);
    gradient.addColorStop(0.66, color.cssString);
    gradient.addColorStop(1, color.setAlpha(0).cssString);
    ctx.fillStyle = gradient;
  }

  ctx.fill();
  ctx.fillStyle = originalFillStyle;
}

/**
 * Clip a canvas using a rect-like object.
 *
 * @param ctx - The canvas context to clip.
 * @param rect - The position and dimensions of the rect to clip.
 */
export function canvasClip(
  ctx: CanvasRenderingContext2D,
  rect: Point2D & Size2D,
): void;

/**
 * Clip a canvas using a separate x, y, width, height values.
 *
 * @param ctx - The canvas context to clip.
 */
export function canvasClip(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
): void;

// This function can either take individual x, y, w, h parameters to define the
// rect, or x can be a rect-like object.
export function canvasClip(
  ctx: CanvasRenderingContext2D,
  x: number | (Point2D & Size2D),
  y?: number,
  w?: number,
  h?: number,
): void {
  ctx.beginPath();
  if (typeof x === 'number') {
    // TypeScript ensures y, w, and h are defined here
    ctx.rect(x, y!, w!, h!);
  } else {
    ctx.rect(x.x, x.y, x.width, x.height);
  }
  ctx.clip();
}

/**
 * Save the state of the canvas, returning a disposable which restores the state
 * when disposed.
 *
 * Allows using the |using| keyword to automatically restore the canvas state.
 * @param ctx - The canvas context to save the state of.
 * @returns A disposable.
 *
 * @example
 * {
 *   using const _ = canvasSave(ctx);
 *   ctx.translate(123, 456); // Manipulate the canvas state
 * } // ctx.restore() is automatically called when the _ falls out of scope
 */
export function canvasSave(ctx: CanvasRenderingContext2D): Disposable {
  ctx.save();
  return {
    [Symbol.dispose](): void {
      ctx.restore();
    },
  };
}

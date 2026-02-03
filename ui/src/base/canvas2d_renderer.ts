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

// Canvas 2D fallback implementation of Renderer for when WebGL is unavailable.
// All transforms are applied via the canvas context's transform matrix
// (translate/scale), so draw methods use coordinates directly.

import {Color} from './color';
import {Transform2D} from './geom';
import {
  Renderer,
  RECT_PATTERN_HATCHED,
  MarkerRenderFunc,
  StepAreaBuffers,
} from './renderer';

// Clip bounds stored in physical screen coordinates (post-transform).
// This allows correct culling regardless of what transforms are active.
interface PhysicalClipBounds {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

export class Canvas2DRenderer implements Renderer {
  private readonly ctx: CanvasRenderingContext2D;
  private previousFillStyle?: string;
  // Track transform ourselves for CPU-side culling calculations.
  private transform = Transform2D.Identity;
  private physicalClipBounds?: PhysicalClipBounds;

  constructor(ctx: CanvasRenderingContext2D) {
    this.ctx = ctx;
  }

  pushTransform(t: Partial<Transform2D>): Disposable {
    const {offsetX = 0, offsetY = 0, scaleX = 1, scaleY = 1} = t;
    const ctx = this.ctx;

    const previousTransform = this.transform;
    this.transform = Transform2D.compose(this.transform, t);

    ctx.save();
    ctx.translate(offsetX, offsetY);
    ctx.scale(scaleX, scaleY);

    return {
      [Symbol.dispose]: () => {
        ctx.restore();
        this.transform = previousTransform;
      },
    };
  }

  drawMarker(
    x: number,
    y: number,
    w: number,
    h: number,
    color: Color,
    render: MarkerRenderFunc,
  ): void {
    // CPU-side culling: transform marker bounds to physical space and compare
    if (this.physicalClipBounds !== undefined) {
      const t = this.transform;
      const physLeft = t.offsetX + (x - w / 2) * t.scaleX;
      const physRight = t.offsetX + (x + w / 2) * t.scaleX;
      const physTop = t.offsetY + y * t.scaleY;
      const physBottom = t.offsetY + (y + h) * t.scaleY;
      const clip = this.physicalClipBounds;
      if (
        physRight < clip.left ||
        physLeft > clip.right ||
        physBottom < clip.top ||
        physTop > clip.bottom
      ) {
        return;
      }
    }

    const ctx = this.ctx;
    if (this.previousFillStyle !== color.cssString) {
      ctx.fillStyle = color.cssString;
      this.previousFillStyle = color.cssString;
    }
    render(ctx, x - w / 2, y, w, h);
  }

  drawRect(
    left: number,
    top: number,
    right: number,
    bottom: number,
    color: Color,
    flags = 0,
  ): void {
    // CPU-side culling: transform rect bounds to physical space and compare
    if (this.physicalClipBounds !== undefined) {
      const t = this.transform;
      const physLeft = t.offsetX + left * t.scaleX;
      const physRight = t.offsetX + right * t.scaleX;
      const physTop = t.offsetY + top * t.scaleY;
      const physBottom = t.offsetY + bottom * t.scaleY;
      const clip = this.physicalClipBounds;
      if (
        physRight < clip.left ||
        physLeft > clip.right ||
        physBottom < clip.top ||
        physTop > clip.bottom
      ) {
        return;
      }
    }

    const ctx = this.ctx;
    const w = right - left;
    const h = bottom - top;

    if (this.previousFillStyle !== color.cssString) {
      ctx.fillStyle = color.cssString;
      this.previousFillStyle = color.cssString;
    }
    ctx.fillRect(left, top, w, h);

    if (flags & RECT_PATTERN_HATCHED && w >= 5) {
      ctx.fillStyle = getHatchedPattern(ctx);
      ctx.fillRect(left, top, w, h);
      this.previousFillStyle = undefined;
    }
  }

  drawStepArea(
    buffers: StepAreaBuffers,
    transform: Transform2D,
    color: Color,
  ): void {
    const {xs, ys, minYs, maxYs, fillAlpha, xnext, count} = buffers;
    if (count < 1) return;

    const ctx = this.ctx;
    const canvasWidth = ctx.canvas.width;

    // Transform functions: screenCoord = raw * scale + offset
    const tx = (x: number) => x * transform.scaleX + transform.offsetX;
    const ty = (y: number) => y * transform.scaleY + transform.offsetY;
    // Baseline is where y=0 maps to
    const baselineY = transform.offsetY;

    // Fill the area under the step line
    ctx.fillStyle = color.cssString;

    // Draw the stroke line on top
    const strokeColor = color.setAlpha(1.0);
    ctx.strokeStyle = strokeColor.cssString;
    ctx.beginPath();
    let strokeStarted = false;

    for (let i = 0; i < count; i++) {
      // Compute segment bounds
      const x = Math.round(tx(xs[i]));
      const nextX = Math.round(tx(xnext[i]));

      // Skip segments entirely off the left edge
      if (nextX <= 0) continue;
      // Stop once we're past the right edge
      if (x >= canvasWidth) break;

      const y = ty(ys[i]);
      const minY = ty(minYs[i]);
      const maxY = ty(maxYs[i]);
      const fill = fillAlpha[i];

      // If fillAlpha is close to zero, don't draw anything at all
      if (fill >= 0.01) {
        const width = nextX - x;
        const height = baselineY - y;
        ctx.globalAlpha = fill;
        ctx.fillRect(x, y, width, height);
      }

      // Stroke
      if (!strokeStarted) {
        ctx.moveTo(x, baselineY);
        strokeStarted = true;
      }
      ctx.lineTo(x, maxY);
      ctx.lineTo(x, minY);
      ctx.lineTo(x, y);
      ctx.lineTo(nextX, y);
    }

    ctx.globalAlpha = 1.0;
    ctx.stroke();
  }

  flush(): void {
    // Draw calls are immediate in Canvas2D, so nothing to do here. Reset the
    // previous color cache as the ctx might be used and the fillStyle changed
    // externally.
    this.previousFillStyle = undefined;
  }

  clip(x: number, y: number, w: number, h: number): Disposable {
    const ctx = this.ctx;

    // Store clip bounds in physical coordinates for CPU-side culling
    const t = this.transform;
    const physX = t.offsetX + x * t.scaleX;
    const physY = t.offsetY + y * t.scaleY;
    const physW = w * t.scaleX;
    const physH = h * t.scaleY;

    const previousClipBounds = this.physicalClipBounds;
    this.physicalClipBounds = {
      left: physX,
      top: physY,
      right: physX + physW,
      bottom: physY + physH,
    };

    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();

    return {
      [Symbol.dispose]: () => {
        ctx.restore();
        this.physicalClipBounds = previousClipBounds;
      },
    };
  }

  resetTransform(): void {
    this.ctx.resetTransform();
    this.transform = Transform2D.Identity;
  }

  clear(): void {
    const ctx = this.ctx;
    const canvas = ctx.canvas;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
  }
}

// Creates a diagonal hatched pattern for distinguishing slices with real-time
// priorities. The pattern is created once as an offscreen canvas and cached
// on the main canvas context.
function getHatchedPattern(ctx: CanvasRenderingContext2D): CanvasPattern {
  const mctx = ctx as CanvasRenderingContext2D & {
    sliceHatchedPattern?: CanvasPattern;
  };
  if (mctx.sliceHatchedPattern !== undefined) return mctx.sliceHatchedPattern;

  const canvas = document.createElement('canvas');
  const SIZE = 8;
  canvas.width = canvas.height = SIZE;
  const patternCtx = canvas.getContext('2d')!;
  patternCtx.strokeStyle = 'rgba(255,255,255,0.3)';
  patternCtx.beginPath();
  patternCtx.lineWidth = 1;
  patternCtx.moveTo(0, SIZE);
  patternCtx.lineTo(SIZE, 0);
  patternCtx.stroke();
  mctx.sliceHatchedPattern = mctx.createPattern(canvas, 'repeat')!;
  return mctx.sliceHatchedPattern;
}

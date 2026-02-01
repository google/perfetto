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
import {
  Renderer,
  Transform2D,
  RECT_PATTERN_HATCHED,
  MarkerRenderFunc,
} from './renderer';

export class Canvas2DRenderer implements Renderer {
  private readonly ctx: CanvasRenderingContext2D;
  private previousFillStyle?: string;

  constructor(ctx: CanvasRenderingContext2D) {
    this.ctx = ctx;
  }

  pushTransform({
    offsetX = 0,
    offsetY = 0,
    scaleX = 1,
    scaleY = 1,
  }: Partial<Transform2D>): Disposable {
    const ctx = this.ctx;

    ctx.save();
    ctx.translate(offsetX, offsetY);
    ctx.scale(scaleX, scaleY);

    return {
      [Symbol.dispose]: () => {
        ctx.restore();
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

  flush(): void {
    // Draw calls are immediate in Canvas2D, so nothing to do here. Reset the
    // previous color cache as the ctx might be used and the fillStyle changed
    // externally.
    this.previousFillStyle = undefined;
  }

  clip(x: number, y: number, w: number, h: number): Disposable {
    const ctx = this.ctx;
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();
    return {
      [Symbol.dispose]: () => {
        ctx.restore();
      },
    };
  }

  resetTransform(): void {
    this.ctx.resetTransform();
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

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

import {
  Renderer,
  Transform2D,
  RECT_PATTERN_HATCHED,
  MarkerRenderFunc,
} from './renderer';

export class Canvas2DRenderer implements Renderer {
  private readonly ctx: CanvasRenderingContext2D;
  private readonly colorCache: Record<number, string> = {};

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
    rgba: number,
    render: MarkerRenderFunc,
  ): void {
    const ctx = this.ctx;
    if (this.previousColor !== rgba) {
      ctx.fillStyle = this.rgbaToString(rgba);
      this.previousColor = rgba;
    }
    render(ctx, x - w / 2, y, w, h);
  }

  private previousColor = -1;

  drawRect(
    left: number,
    top: number,
    right: number,
    bottom: number,
    rgba: number,
    flags = 0,
  ): void {
    const ctx = this.ctx;
    const w = right - left;
    const h = bottom - top;

    if (this.previousColor !== rgba) {
      ctx.fillStyle = this.rgbaToString(rgba);
      this.previousColor = rgba;
    }
    ctx.fillRect(left, top, w, h);

    if (flags & RECT_PATTERN_HATCHED && w >= 5) {
      ctx.fillStyle = getHatchedPattern(ctx);
      ctx.fillRect(left, top, w, h);
      this.previousColor = -1;
    }
  }

  flush(): void {
    this.previousColor = -1;
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

  rawCanvas(fn: (ctx: CanvasRenderingContext2D) => void): void {
    this.previousColor = -1;
    fn(this.ctx);
  }

  resetTransform(): void {
    this.ctx.resetTransform();
  }

  clear(): void {
    const ctx = this.ctx;
    const canvas = ctx.canvas;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
  }

  // Converts RGBA packed integer to css string
  private rgbaToString(rgba: number): string {
    const cached = this.colorCache[rgba];
    if (cached !== undefined) {
      return cached;
    }
    const r = (rgba >> 24) & 0xff;
    const g = (rgba >> 16) & 0xff;
    const b = (rgba >> 8) & 0xff;
    const a = rgba & 0xff;
    const cssString = `rgba(${r},${g},${b},${a})`;
    this.colorCache[rgba] = cssString;
    return cssString;
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

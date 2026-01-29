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

// Canvas 2D fallback implementation of Renderer for when WebGL is unavailable.
// All transforms are applied via the canvas context's transform matrix
// (translate/scale), so draw methods use coordinates directly.

import {
  Renderer,
  RGBA,
  Transform2D,
  RECT_FLAG_HATCHED,
  RECT_FLAG_FADEOUT,
  MarkerRenderFunc,
} from './renderer';

export class Canvas2DRenderer implements Renderer {
  private readonly ctx: CanvasRenderingContext2D;

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
    color: RGBA,
    render: MarkerRenderFunc,
  ): void {
    const ctx = this.ctx;
    ctx.fillStyle = rgbaToString(color);
    render(ctx, x - w / 2, y, w, h);
  }

  drawMarkers(
    positions: Float32Array,
    sizes: Float32Array,
    colors: Uint8Array,
    count: number,
    render: MarkerRenderFunc,
  ): void {
    const ctx = this.ctx;

    for (let i = 0; i < count; i++) {
      const x = positions[i * 2];
      const y = positions[i * 2 + 1];
      const w = sizes[i * 2];
      const h = sizes[i * 2 + 1];

      const r = colors[i * 4];
      const g = colors[i * 4 + 1];
      const b = colors[i * 4 + 2];
      const a = colors[i * 4 + 3] / 255;

      ctx.fillStyle = `rgba(${r},${g},${b},${a})`;
      render(ctx, x - w / 2, y, w, h);
    }
  }

  drawRect(
    left: number,
    top: number,
    right: number,
    bottom: number,
    color: RGBA,
    flags = 0,
  ): void {
    const ctx = this.ctx;
    const w = right - left;
    const h = bottom - top;

    if (flags & RECT_FLAG_FADEOUT) {
      const gradient = ctx.createLinearGradient(left, 0, right, 0);
      gradient.addColorStop(0, rgbaToString(color));
      gradient.addColorStop(1, rgbaToString({...color, a: 0}));
      ctx.fillStyle = gradient;
    } else {
      ctx.fillStyle = rgbaToString(color);
    }
    ctx.fillRect(left, top, w, h);

    if (flags & RECT_FLAG_HATCHED && w >= 5) {
      ctx.fillStyle = getHatchedPattern(ctx);
      ctx.fillRect(left, top, w, h);
    }
  }

  drawRects(
    topLeft: Float32Array,
    bottomRight: Float32Array,
    colors: Uint8Array,
    count: number,
    flags?: Uint8Array,
  ): void {
    const ctx = this.ctx;

    for (let i = 0; i < count; i++) {
      const rectFlags = flags?.[i] ?? 0;

      const left = topLeft[i * 2];
      const top = topLeft[i * 2 + 1];
      const right = bottomRight[i * 2];
      const bottom = bottomRight[i * 2 + 1];
      const w = right - left;
      const h = bottom - top;

      const r = colors[i * 4];
      const g = colors[i * 4 + 1];
      const b = colors[i * 4 + 2];
      const a = colors[i * 4 + 3] / 255;

      if (rectFlags & RECT_FLAG_FADEOUT) {
        const gradient = ctx.createLinearGradient(left, 0, right, 0);
        gradient.addColorStop(0, `rgba(${r},${g},${b},${a})`);
        gradient.addColorStop(1, `rgba(${r},${g},${b},0)`);
        ctx.fillStyle = gradient;
      } else {
        ctx.fillStyle = `rgba(${r},${g},${b},${a})`;
      }
      ctx.fillRect(left, top, w, h);

      if (rectFlags & RECT_FLAG_HATCHED && w >= 5) {
        ctx.fillStyle = getHatchedPattern(ctx);
        ctx.fillRect(left, top, w, h);
      }
    }
  }

  raw(fn: (ctx: CanvasRenderingContext2D) => void): void {
    fn(this.ctx);
  }

  flush(): void {
    // No-op for Canvas 2D - drawing is immediate
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
}

function rgbaToString(color: RGBA): string {
  return `rgba(${color.r},${color.g},${color.b},${color.a / 255})`;
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

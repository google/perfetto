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

// Canvas 2D fallback implementation of CanvasRenderer for when WebGL is
// unavailable.

import {
  TimelineRenderer,
  RGBA,
  Transform2D,
  RECT_FLAG_HATCHED,
  RECT_FLAG_FADEOUT,
} from './timeline_renderer';

// 1D time transform (x-axis only): pixelX = offset + time * scale
interface TimeTransform {
  offset: number;
  scale: number;
}

// Canvas 2D fallback renderer.
// - Pixel transforms (offset only) are applied to canvas context immediately
// - Time transforms (offset + scale) are stored manually and applied to rects
export class Canvas2DRenderer implements TimelineRenderer {
  private readonly ctx: CanvasRenderingContext2D;

  // Time transform stack - 1D (x-axis only)
  private timeTransformStack: TimeTransform[] = [];
  private timeTransform: TimeTransform = {offset: 0, scale: 1};

  constructor(ctx: CanvasRenderingContext2D) {
    this.ctx = ctx;
  }

  pushTransform(transform: Transform2D): Disposable {
    const isPixelTransform = transform.scaleX === 1 && transform.scaleY === 1;
    const ctx = this.ctx;

    if (isPixelTransform) {
      // Pixel transform: apply to canvas context immediately
      ctx.save();
      ctx.translate(transform.offsetX, transform.offsetY);

      return {
        [Symbol.dispose]: () => {
          ctx.restore();
        },
      };
    } else {
      // Time transform: store manually (1D, x-axis only)
      this.timeTransformStack.push({...this.timeTransform});
      this.timeTransform = {
        offset: this.timeTransform.offset + transform.offsetX,
        scale: this.timeTransform.scale * transform.scaleX,
      };

      return {
        [Symbol.dispose]: () => {
          const prev = this.timeTransformStack.pop();
          if (prev) {
            this.timeTransform = prev;
          }
        },
      };
    }
  }

  drawBillboard(
    x: number,
    y: number,
    w: number,
    h: number,
    color: RGBA,
    render: (
      ctx: CanvasRenderingContext2D,
      x: number,
      y: number,
      w: number,
      h: number,
    ) => void,
  ): void {
    const t = this.timeTransform;

    // Transform x from time to pixels (y is already in pixels)
    const pixelX = t.offset + x * t.scale;

    // Center horizontally
    const centeredX = pixelX - w / 2;

    // Draw - canvas context already has pixel transform applied
    const ctx = this.ctx;
    ctx.fillStyle = rgbaToString(color);
    render(ctx, centeredX, y, w, h);
  }

  drawBillboards(
    positions: Float32Array,
    sizes: Float32Array,
    colors: Uint8Array,
    count: number,
    render: (
      ctx: CanvasRenderingContext2D,
      x: number,
      y: number,
      w: number,
      h: number,
    ) => void,
  ): void {
    const t = this.timeTransform;
    const ctx = this.ctx;

    for (let i = 0; i < count; i++) {
      const x = positions[i * 2];
      const y = positions[i * 2 + 1];
      const w = sizes[i * 2];
      const h = sizes[i * 2 + 1];

      // Transform x from time to pixels (y is already in pixels)
      const pixelX = t.offset + x * t.scale;

      // Center horizontally
      const centeredX = pixelX - w / 2;

      const r = colors[i * 4];
      const g = colors[i * 4 + 1];
      const b = colors[i * 4 + 2];
      const a = colors[i * 4 + 3] / 255;

      ctx.fillStyle = `rgba(${r},${g},${b},${a})`;
      render(ctx, centeredX, y, w, h);
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
    const t = this.timeTransform;
    const ctx = this.ctx;

    // Transform x from time to pixels (y is already in pixels)
    const pxLeft = t.offset + left * t.scale;
    // Handle +Infinity (incomplete slices) - extend to canvas edge
    const pxRight = isFinite(right)
      ? t.offset + right * t.scale
      : ctx.canvas.width;

    const w = Math.max(1, pxRight - pxLeft); // 1px minimum
    const h = bottom - top;

    // Handle fadeout with a horizontal gradient
    if (flags & RECT_FLAG_FADEOUT) {
      const gradient = ctx.createLinearGradient(pxLeft, 0, pxRight, 0);
      gradient.addColorStop(0, rgbaToString(color));
      gradient.addColorStop(1, rgbaToString({...color, a: 0}));
      ctx.fillStyle = gradient;
    } else {
      ctx.fillStyle = rgbaToString(color);
    }
    ctx.fillRect(pxLeft, top, w, h);

    // Draw hatched pattern overlay if flag is set (skip if too small)
    if ((flags & RECT_FLAG_HATCHED) && w >= 5) {
      ctx.fillStyle = getHatchedPattern(ctx);
      ctx.fillRect(pxLeft, top, w, h);
    }
  }

  drawRects(
    topLeft: Float32Array,
    bottomRight: Float32Array,
    colors: Uint8Array,
    count: number,
    flags?: Uint8Array,
  ): void {
    const t = this.timeTransform;
    const ctx = this.ctx;

    for (let i = 0; i < count; i++) {
      const rectFlags = flags?.[i] ?? 0;

      const left = topLeft[i * 2];
      const top = topLeft[i * 2 + 1];
      const right = bottomRight[i * 2];
      const bottom = bottomRight[i * 2 + 1];

      // Transform x from time to pixels (y is already in pixels)
      const pxLeft = t.offset + left * t.scale;
      // Handle +Infinity (incomplete slices) - extend to canvas edge
      const pxRight = isFinite(right)
        ? t.offset + right * t.scale
        : ctx.canvas.width;

      const w = Math.max(1, pxRight - pxLeft); // 1px minimum
      const h = bottom - top;

      const r = colors[i * 4];
      const g = colors[i * 4 + 1];
      const b = colors[i * 4 + 2];
      const a = colors[i * 4 + 3] / 255;

      // Handle fadeout with a horizontal gradient
      if (rectFlags & RECT_FLAG_FADEOUT) {
        const gradient = ctx.createLinearGradient(pxLeft, 0, pxRight, 0);
        gradient.addColorStop(0, `rgba(${r},${g},${b},${a})`);
        gradient.addColorStop(1, `rgba(${r},${g},${b},0)`);
        ctx.fillStyle = gradient;
      } else {
        ctx.fillStyle = `rgba(${r},${g},${b},${a})`;
      }
      ctx.fillRect(pxLeft, top, w, h);

      // Draw hatched pattern overlay if flag is set (skip if too small)
      if ((rectFlags & RECT_FLAG_HATCHED) && w >= 5) {
        ctx.fillStyle = getHatchedPattern(ctx);
        ctx.fillRect(pxLeft, top, w, h);
      }
    }
  }

  flush(): void {
    // No-op for Canvas 2D - drawing is immediate
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

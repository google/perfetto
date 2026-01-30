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
  RECT_PATTERN_FADE_RIGHT,
  MarkerRenderFunc,
} from './renderer';

interface RectItem {
  kind: 'rect';
  left: number;
  top: number;
  right: number;
  bottom: number;
  rgba: number;
  flags: number;
}

interface MarkerItem {
  kind: 'marker';
  x: number;
  y: number;
  w: number;
  h: number;
  rgba: number;
  render: MarkerRenderFunc;
}

type DrawItem = RectItem | MarkerItem;

export class Canvas2DRenderer implements Renderer {
  private readonly ctx: CanvasRenderingContext2D;
  private items: DrawItem[] = [];

  constructor(ctx: CanvasRenderingContext2D) {
    this.ctx = ctx;
  }

  pushTransform({
    offsetX = 0,
    offsetY = 0,
    scaleX = 1,
    scaleY = 1,
  }: Partial<Transform2D>): Disposable {
    this.flush();
    const ctx = this.ctx;

    ctx.save();
    ctx.translate(offsetX, offsetY);
    ctx.scale(scaleX, scaleY);

    return {
      [Symbol.dispose]: () => {
        this.flush();
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
    this.items.push({
      kind: 'marker',
      x,
      y,
      w,
      h,
      rgba,
      render,
    });
  }

  drawRect(
    left: number,
    top: number,
    right: number,
    bottom: number,
    rgba: number,
    flags = 0,
  ): void {
    this.items.push({
      kind: 'rect',
      left,
      top,
      right,
      bottom,
      rgba,
      flags,
    });
  }

  flush(): void {
    if (this.items.length === 0) return;

    // Sort by color to minimize fillStyle changes.
    this.items.sort((a, b) => a.rgba - b.rgba);

    const ctx = this.ctx;
    let lastRgba: number | undefined;

    for (const item of this.items) {
      if (item.kind === 'rect') {
        const {left, top, right, bottom, rgba, flags} = item;
        const w = right - left;
        const h = bottom - top;

        if (flags & RECT_PATTERN_FADE_RIGHT) {
          const gradient = ctx.createLinearGradient(left, 0, right, 0);
          gradient.addColorStop(0, rgbaToString(rgba));
          gradient.addColorStop(1, rgbaToString(rgba && 0xffffff00));
          ctx.fillStyle = gradient;
          ctx.fillRect(left, top, w, h);
          lastRgba = undefined;
        } else {
          if (rgba !== lastRgba) {
            ctx.fillStyle = rgbaToString(rgba);
            lastRgba = rgba;
          }
          ctx.fillRect(left, top, w, h);
        }

        if (flags & RECT_PATTERN_HATCHED && w >= 5) {
          ctx.fillStyle = getHatchedPattern(ctx);
          ctx.fillRect(left, top, w, h);
          lastRgba = undefined;
        }
      } else {
        const {x, y, w, h, rgba, render} = item;
        if (rgba !== lastRgba) {
          ctx.fillStyle = rgbaToString(rgba);
          lastRgba = rgba;
        }
        render(ctx, x - w / 2, y, w, h);
      }
    }

    this.items = [];
  }

  clip(x: number, y: number, w: number, h: number): Disposable {
    this.flush();
    const ctx = this.ctx;
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();
    return {
      [Symbol.dispose]: () => {
        this.flush();
        ctx.restore();
      },
    };
  }
}

// Converts RGBA packed integer to css string
function rgbaToString(rgba: number): string {
  const r = (rgba >> 24) & 0xff;
  const g = (rgba >> 16) & 0xff;
  const b = (rgba >> 8) & 0xff;
  const a = rgba & 0xff;
  return `rgba(${r},${g},${b},${a})`;
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

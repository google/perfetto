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
  RECT_PATTERN_FADE_RIGHT,
  MarkerRenderFunc,
  MarkerBuffers,
  StepAreaBuffers,
  RectBuffers,
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

  drawMarkers(
    buffers: MarkerBuffers,
    dataTransform: Transform2D,
    render: MarkerRenderFunc,
  ): void {
    const {xs, ys, w, h, colors, count} = buffers;
    const ctx = this.ctx;
    const clip = this.physicalClipBounds;
    const t = this.transform;
    const {offsetX, scaleX, offsetY, scaleY} = dataTransform;
    let previousColor: number | undefined = undefined;

    for (let i = 0; i < count; i++) {
      // Transform X from data space to screen space (centered)
      const screenX = xs[i] * scaleX + offsetX;
      const y = ys[i] * scaleY + offsetY;

      // CPU-side culling
      if (clip !== undefined) {
        const physLeft = t.offsetX + (screenX - w / 2) * t.scaleX;
        const physRight = t.offsetX + (screenX + w / 2) * t.scaleX;
        const physTop = t.offsetY + y * t.scaleY;
        const physBottom = t.offsetY + (y + h) * t.scaleY;
        if (
          physRight < clip.left ||
          physLeft > clip.right ||
          physBottom < clip.top ||
          physTop > clip.bottom
        ) {
          continue;
        }
      }

      // Convert packed RGBA (0xRRGGBBAA) to CSS string
      const rgba = colors[i];
      if (previousColor !== rgba) {
        const r = (rgba >> 24) & 0xff;
        const g = (rgba >> 16) & 0xff;
        const b = (rgba >> 8) & 0xff;
        const a = (rgba & 0xff) / 255;
        const cssColor = `rgba(${r},${g},${b},${a})`;
        ctx.fillStyle = cssColor;
        previousColor = rgba;
      }

      render(ctx, screenX - w / 2, y, w, h);
    }
  }

  drawRects(buffers: RectBuffers, dataTransform: Transform2D): void {
    const {xs, ys, ws, h, colors, patterns, count} = buffers;
    const ctx = this.ctx;
    const clip = this.physicalClipBounds;
    const t = this.transform;
    const {offsetX, scaleX, offsetY, scaleY} = dataTransform;
    let previousColor: number | undefined = undefined;

    for (let i = 0; i < count; i++) {
      // Transform X and Y from data coordinates to screen coordinates
      const x = xs[i] * scaleX + offsetX;
      const y = ys[i] * scaleY + offsetY;
      const w = Math.max(ws[i] * scaleX, 1);

      // CPU-side culling and clamping to clip bounds
      let drawX = x;
      let drawY = y;
      let drawW = w;
      let drawH = h;

      if (clip !== undefined) {
        const physLeft = t.offsetX + x * t.scaleX;
        const physRight = t.offsetX + (x + w) * t.scaleX;
        const physTop = t.offsetY + y * t.scaleY;
        const physBottom = t.offsetY + (y + h) * t.scaleY;

        // Cull if completely outside
        if (
          physRight < clip.left ||
          physLeft > clip.right ||
          physBottom < clip.top ||
          physTop > clip.bottom
        ) {
          continue;
        }

        // Clamp to clip bounds (in physical space, then convert back to screen space)
        const cPhysLeft = Math.max(physLeft, clip.left);
        const cPhysRight = Math.min(physRight, clip.right);
        const cPhysTop = Math.max(physTop, clip.top);
        const cPhysBottom = Math.min(physBottom, clip.bottom);

        // Convert clamped physical coords back to screen space
        drawX = (cPhysLeft - t.offsetX) / t.scaleX;
        drawY = (cPhysTop - t.offsetY) / t.scaleY;
        drawW = (cPhysRight - cPhysLeft) / t.scaleX;
        drawH = (cPhysBottom - cPhysTop) / t.scaleY;
      }

      // Convert packed RGBA (0xRRGGBBAA) to CSS string
      const rgba = colors[i];
      if (previousColor !== rgba) {
        const r = (rgba >> 24) & 0xff;
        const g = (rgba >> 16) & 0xff;
        const b = (rgba >> 8) & 0xff;
        const a = (rgba & 0xff) / 255;
        const cssColor = `rgba(${r},${g},${b},${a})`;
        ctx.fillStyle = cssColor;
        previousColor = rgba;
      }
      ctx.fillRect(drawX, drawY, drawW, drawH);

      const flags = patterns[i];
      if (flags & RECT_PATTERN_HATCHED && w >= 5) {
        ctx.fillStyle = getHatchedPattern(ctx);
        ctx.fillRect(drawX, drawY, drawW, drawH);
        previousColor = undefined;
      }

      if (flags & RECT_PATTERN_FADE_RIGHT && w >= 5) {
        ctx.save();
        ctx.globalCompositeOperation = 'destination-out';
        // Fade ends at the clamped right edge
        const gradient = ctx.createLinearGradient(
          drawX,
          drawY,
          drawX + drawW,
          drawY,
        );
        gradient.addColorStop(0.66, 'rgba(0, 0, 0, 0)');
        gradient.addColorStop(1.0, 'rgba(0, 0, 0, 1)');
        ctx.fillStyle = gradient;
        ctx.fillRect(drawX, drawY, drawW, drawH);
        ctx.restore();
        previousColor = undefined;
      }
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
    const clip = this.physicalClipBounds;
    const baselineY = transform.offsetY;
    const strokeColor = color.setAlpha(1.0);

    // Transform functions: screenCoord = raw * scale + offset
    const tx = (x: number) => x * transform.scaleX + transform.offsetX;
    const ty = (y: number) => y * transform.scaleY + transform.offsetY;

    ctx.fillStyle = color.cssString;
    ctx.strokeStyle = strokeColor.cssString;
    ctx.beginPath();

    for (let i = 0; i < count; i++) {
      // Compute segment bounds
      const x = Math.round(tx(xs[i]));
      const nextX = Math.round(tx(xnext[i]));

      // Don't render segments that are fully outside the clip region
      if (clip) {
        const physX = this.transform.offsetX + x * this.transform.scaleX;
        const physNextX =
          this.transform.offsetX + nextX * this.transform.scaleX;
        // Skip segments entirely off the left edge
        if (physNextX < clip.left) continue;
        // Stop once we're past the right edge
        if (physX >= clip.right) break;
      }

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

      // Draws a sideways T (range indicator) at the transition x:
      //
      //  maxY +  (Top of range)
      //       |
      //     y +-------+ (nextX, y)
      //       |
      //  minY +  (Bottom of range)
      ctx.moveTo(x, maxY);
      ctx.lineTo(x, minY);
      ctx.moveTo(x, y);
      ctx.lineTo(nextX, y);
    }

    ctx.globalAlpha = 1.0;
    ctx.stroke();
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

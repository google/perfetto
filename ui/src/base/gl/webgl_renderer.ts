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

// WebGL renderer with an immediate-mode style API using instanced rendering.
// Uses two separate pipelines:
// 1. Rects pipeline - plain/hatched rectangles
// 2. Markers pipeline - SDF-based shapes like chevrons

import {Renderer, MarkerRenderFunc} from './../renderer';
import {DisposableStack} from './../disposable_stack';
import {RectBatch} from './rects';
import {ChevronBatch} from './chevrons';
import {StepAreaBatch} from './step_area';
import {Color} from './../color';
import {Transform2D} from '../geom';

export class WebGLRenderer implements Renderer {
  private readonly c2d: CanvasRenderingContext2D;
  readonly gl: WebGL2RenderingContext;
  private readonly rects: RectBatch;
  private readonly markers: ChevronBatch;
  private readonly stepArea: StepAreaBatch;
  private transform = Transform2D.Identity;

  constructor(c2d: CanvasRenderingContext2D, gl: WebGL2RenderingContext) {
    this.c2d = c2d;
    this.gl = gl;
    this.rects = new RectBatch(gl);
    this.markers = new ChevronBatch(gl);
    this.stepArea = new StepAreaBatch(gl);
  }

  pushTransform(transform: Partial<Transform2D>): Disposable {
    const trash = new DisposableStack();
    trash.use(this.pushWebGLTransform(transform));
    trash.use(this.pushCanvas2DTransform(transform));
    return trash;
  }

  pushWebGLTransform(transform: Partial<Transform2D>): Disposable {
    const previousTransform = this.transform;
    this.transform = Transform2D.compose(this.transform, transform);
    return {
      [Symbol.dispose]: () => {
        this.transform = previousTransform;
      },
    };
  }

  pushCanvas2DTransform({
    offsetX = 0,
    offsetY = 0,
    scaleX = 1,
    scaleY = 1,
  }: Partial<Transform2D>): Disposable {
    const ctx = this.c2d;
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
    _render: MarkerRenderFunc,
  ): void {
    if (this.markers.isFull) {
      this.markers.flush(this.transform);
    }
    this.markers.add(x, y, w, h, color.rgba);
  }

  drawRect(
    left: number,
    top: number,
    right: number,
    bottom: number,
    color: Color,
    flags = 0,
  ): void {
    if (this.rects.isFull) {
      this.rects.flush(this.transform);
    }
    this.rects.add(left, top, right, bottom, color.rgba, flags);
  }

  drawStepArea(
    xs: ArrayLike<number>,
    ys: ArrayLike<number>,
    minYs: ArrayLike<number>,
    maxYs: ArrayLike<number>,
    fills: ArrayLike<number>,
    count: number,
    trackTop: number,
    trackBottom: number,
    baselineY: number,
    color: Color,
  ): void {
    if (count < 1) return;

    // Canvas width in CSS pixels (before DPR scaling)
    const canvasWidth = this.gl.canvas.width / this.transform.scaleX;

    // Find the range of visible points (cull offscreen points)
    let startIdx = 0;
    let endIdx = count;

    // Find first point whose segment is visible (next point > 0, or last point)
    while (startIdx < count - 1 && xs[startIdx + 1] <= 0) {
      startIdx++;
    }

    // Find last point that starts before canvasWidth
    while (endIdx > startIdx + 1 && xs[endIdx - 1] >= canvasWidth) {
      endIdx--;
    }

    // No visible points
    if (startIdx >= endIdx) return;

    this.stepArea.begin(trackTop, trackBottom, baselineY, color.rgba);
    for (let i = startIdx; i < endIdx; i++) {
      if (this.stepArea.isFull) {
        this.stepArea.flush(this.transform);
        this.stepArea.begin(trackTop, trackBottom, baselineY, color.rgba);
      }
      // For the first visible segment, connect from baseline (like Canvas2D moveTo)
      const prevY = i === startIdx ? baselineY : ys[i - 1];
      // Clamp x values to visible area; last point extends to canvasWidth
      const x0 = Math.max(0, xs[i]);
      const x1 = i + 1 < count ? Math.min(canvasWidth, xs[i + 1]) : canvasWidth;
      this.stepArea.addSegment(
        x0,
        x1,
        ys[i],
        minYs[i],
        maxYs[i],
        prevY,
        fills[i],
      );
    }
    this.stepArea.flush(this.transform);
  }

  flush(): void {
    this.rects.flush(this.transform);
    this.markers.flush(this.transform);
    this.stepArea.flush(this.transform);
  }

  resetTransform(): void {
    this.flush();
    this.transform = Transform2D.Identity;
    this.c2d.resetTransform();
  }

  clear(): void {
    const gl = this.gl;
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    const ctx = this.c2d;
    const canvas = ctx.canvas;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
  }

  clip(x: number, y: number, w: number, h: number): Disposable {
    const gl = this.gl;
    const ctx = this.c2d;

    this.flush();

    // Apply transform: physPos = offset + pos * scale
    const physX = this.transform.offsetX + x * this.transform.scaleX;
    const physY = this.transform.offsetY + y * this.transform.scaleY;
    const physW = w * this.transform.scaleX;
    const physH = h * this.transform.scaleY;

    gl.enable(gl.SCISSOR_TEST);
    const canvasHeight = gl.canvas.height;
    gl.scissor(
      Math.round(physX),
      Math.round(canvasHeight - (physY + physH)),
      Math.round(physW),
      Math.round(physH),
    );

    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();

    return {
      [Symbol.dispose]: () => {
        this.flush();
        ctx.restore();
        gl.disable(gl.SCISSOR_TEST);
      },
    };
  }
}

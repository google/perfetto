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

import {
  Renderer,
  MarkerRenderFunc,
  MarkerBuffers,
  StepAreaBuffers,
  RectBuffers,
} from './../renderer';
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
  private clipRect:
    | {left: number; top: number; right: number; bottom: number}
    | undefined;

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

  drawMarkers(
    buffers: MarkerBuffers,
    dataTransform: Transform2D,
    _render: MarkerRenderFunc,
  ): void {
    this.markers.draw(buffers, dataTransform, this.transform);
  }

  drawRects(buffers: RectBuffers, dataTransform: Transform2D): void {
    // Use current clip rect, or full canvas if no clip is active
    const clipRect = this.clipRect ?? {
      left: 0,
      top: 0,
      right: this.gl.canvas.width,
      bottom: this.gl.canvas.height,
    };
    this.rects.draw(buffers, dataTransform, this.transform, clipRect);
  }

  drawStepArea(
    buffers: StepAreaBuffers,
    dataTransform: Transform2D,
    color: Color,
    top: number,
    bottom: number,
  ): void {
    this.stepArea.draw(
      buffers,
      dataTransform,
      this.transform,
      top,
      bottom,
      color.rgba,
    );
  }

  resetTransform(): void {
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

    // Store clip rect in screen space for shader-based vertex clamping
    const previousClipRect = this.clipRect;
    this.clipRect = {
      left: physX,
      top: physY,
      right: physX + physW,
      bottom: physY + physH,
    };

    return {
      [Symbol.dispose]: () => {
        ctx.restore();
        gl.disable(gl.SCISSOR_TEST);
        this.clipRect = previousClipRect;
      },
    };
  }
}

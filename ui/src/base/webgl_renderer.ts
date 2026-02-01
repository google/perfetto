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

import {Renderer, Transform2D, MarkerRenderFunc} from './renderer';
import {DisposableStack} from './disposable_stack';
import {RectBatch} from './rects';
import {MarkerBatch} from './markers';
import {Color} from './color';

function composeTransforms(
  a: Transform2D,
  b: Partial<Transform2D>,
): Transform2D {
  const {offsetX = 0, offsetY = 0, scaleX = 1, scaleY = 1} = b;
  return {
    offsetX: a.offsetX + offsetX * a.scaleX,
    offsetY: a.offsetY + offsetY * a.scaleY,
    scaleX: a.scaleX * scaleX,
    scaleY: a.scaleY * scaleY,
  };
}

const Identity: Transform2D = {
  offsetX: 0,
  offsetY: 0,
  scaleX: 1,
  scaleY: 1,
};

export class WebGLRenderer implements Renderer {
  private readonly c2d: CanvasRenderingContext2D;
  readonly gl: WebGL2RenderingContext;
  private readonly rects: RectBatch;
  private readonly markers: MarkerBatch;
  private transform: Transform2D = Identity;

  constructor(c2d: CanvasRenderingContext2D, gl: WebGL2RenderingContext) {
    this.c2d = c2d;
    this.gl = gl;
    this.rects = new RectBatch(gl);
    this.markers = new MarkerBatch(gl);
  }

  pushTransform(transform: Partial<Transform2D>): Disposable {
    const trash = new DisposableStack();
    trash.use(this.pushWebGLTransform(transform));
    trash.use(this.pushCanvas2DTransform(transform));
    return trash;
  }

  pushWebGLTransform(transform: Partial<Transform2D>): Disposable {
    const previousTransform = this.transform;
    this.transform = composeTransforms(this.transform, transform);
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

  flush(): void {
    this.rects.flush(this.transform);
    this.markers.flush(this.transform);
  }

  resetTransform(): void {
    this.flush();
    this.transform = Identity;
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

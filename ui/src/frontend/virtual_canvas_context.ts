// Copyright (C) 2018 The Android Open Source Project
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

/**
 * VirtualCanvasContext is similar to a CanvasRenderingContext2D object, with
 * knowledge of where it is positioned relative to the parent rendering context.
 * The parent rendering context is either another VirtualRenderingContext, or a
 * real CanvasRenderingContext2D.
 *
 * It implements a subset of the CanvasRenderingContext2D API, but it translates
 * all the coordinates to the parent context's coordinate space by applying
 * appropriate offsets. The user of this context can thus assume a local
 * coordinate space of (0, 0, width, height). In addition, VirtualCanvasContexts
 * performs strict bounds checking on some drawing, and it allows the user to
 * to query if the virtual context is on the (eventual) backing real canvas, so
 * the user can avoid executing unnecessary drawing logic.
 */
export abstract class VirtualCanvasContext {
  stroke: () => void;
  beginPath: () => void;
  closePath: () => void;
  measureText: () => TextMetrics;

  constructor(protected ctx: CanvasRenderingContext2D|VirtualCanvasContext) {
    this.stroke = this.ctx.stroke.bind(this.ctx);
    this.beginPath = this.ctx.beginPath.bind(this.ctx);
    this.closePath = this.ctx.closePath.bind(this.ctx);
    this.measureText = this.ctx.measureText.bind(this.ctx);
  }

  abstract isOnCanvas(): boolean;
  abstract checkRectOnCanvas(boundingRect: BoundingRect): boolean;
  abstract getBoundingRect(): BoundingRect;

  fillRect(x: number, y: number, width: number, height: number) {
    if (x < 0 || x + width > this.getBoundingRect().width || y < 0 ||
        y + height > this.getBoundingRect().height) {
      throw new OutOfBoundsDrawingError(
          'draw a rect', {x, y, width, height}, this.getBoundingRect());
    }
    if (!this.isOnCanvas()) {
      throw new ContextNotOnCanvasError();
    }

    this.ctx.fillRect(
        x + this.getBoundingRect().x,
        y + this.getBoundingRect().y,
        width,
        height);
  }

  moveTo(x: number, y: number) {
    if (x < 0 || x > this.getBoundingRect().width || y < 0 ||
        y > this.getBoundingRect().height) {
      throw new OutOfBoundsDrawingError(
          'moveto', {x, y}, this.getBoundingRect());
    }
    if (!this.isOnCanvas()) {
      throw new ContextNotOnCanvasError();
    }
    this.ctx.moveTo(x + this.getBoundingRect().x, y + this.getBoundingRect().y);
  }

  lineTo(x: number, y: number) {
    if (x < 0 || x > this.getBoundingRect().width || y < 0 ||
        y > this.getBoundingRect().height) {
      throw new OutOfBoundsDrawingError(
          'lineto', {x, y}, this.getBoundingRect());
    }
    if (!this.isOnCanvas()) {
      throw new ContextNotOnCanvasError();
    }
    this.ctx.lineTo(x + this.getBoundingRect().x, y + this.getBoundingRect().y);
  }

  fillText(text: string, x: number, y: number) {
    if (x < 0 || x > this.getBoundingRect().width || y < 0 ||
        y > this.getBoundingRect().height) {
      throw new OutOfBoundsDrawingError(
          'draw text', {x, y}, this.getBoundingRect());
    }
    if (!this.isOnCanvas()) {
      throw new ContextNotOnCanvasError();
    }
    this.ctx.fillText(
        text, x + this.getBoundingRect().x, y + this.getBoundingRect().y);
  }

  set strokeStyle(v: string) {
    this.ctx.strokeStyle = v;
  }

  set fillStyle(v: string) {
    this.ctx.fillStyle = v;
  }

  set lineWidth(width: number) {
    this.ctx.lineWidth = width;
  }

  set font(fontString: string) {
    this.ctx.font = fontString;
  }
}

export class OutOfBoundsDrawingError extends Error {
  constructor(action: string, drawing: {}, boundingRect: BoundingRect) {
    super(
        `Attempted to ${action} (${JSON.stringify(drawing)})` +
        `in bounds ${JSON.stringify(boundingRect)}`);
  }
}

export class ContextNotOnCanvasError extends Error {
  constructor() {
    super(
        `Attempted to draw on a virtual context that is not on the canvas. ` +
        `Did you check virtualContext.isOnCanvas()?`);
  }
}

export interface BoundingRect {
  x: number;
  y: number;
  width: number;
  height: number;
}
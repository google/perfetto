/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

export class TrackCanvasContext {
  stroke: () => void;
  beginPath: () => void;
  closePath: () => void;
  measureText: () => TextMetrics;

  constructor(
      private ctx: CanvasRenderingContext2D|TrackCanvasContext,
      private rect:
          {left: number, top: number, width: number, height: number}) {
    this.stroke = this.ctx.stroke.bind(this.ctx);
    this.beginPath = this.ctx.beginPath.bind(this.ctx);
    this.closePath = this.ctx.closePath.bind(this.ctx);
    this.measureText = this.ctx.measureText.bind(this.ctx);
  }

  fillRect(x: number, y: number, width: number, height: number) {
    if (x < 0 || x + width > this.rect.width || y < 0 ||
        y + height > this.rect.height) {
      throw new OutOfBoundsDrawingError(
          'draw a rect', {x, y, width, height}.toString(), this.rect);
    }

    this.ctx.fillRect(x + this.rect.left, y + this.rect.top, width, height);
  }

  setDimensions(width: number, height: number) {
    this.rect.width = width;
    this.rect.height = height;
  }

  setYOffset(offset: number) {
    this.rect.top = offset;
  }

  moveTo(x: number, y: number) {
    if (x < 0 || x > this.rect.width || y < 0 || y > this.rect.height) {
      throw new OutOfBoundsDrawingError('moveto', {x, y}.toString(), this.rect);
    }

    this.ctx.moveTo(x + this.rect.left, y + this.rect.top);
  }

  lineTo(x: number, y: number) {
    if (x < 0 || x > this.rect.width || y < 0 || y > this.rect.height) {
      throw new OutOfBoundsDrawingError('lineto', {x, y}.toString(), this.rect);
    }

    this.ctx.lineTo(x + this.rect.left, y + this.rect.top);
  }

  fillText(text: string, x: number, y: number) {
    if (x < 0 || x > this.rect.width || y < 0 || y > this.rect.height) {
      throw new OutOfBoundsDrawingError(
          'draw text', {x, y}.toString(), this.rect);
    }
    this.ctx.fillText(text, x + this.rect.left, y + this.rect.top);
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
  constructor(
      action: string, drawing: string,
      bounds: {left: number, top: number, width: number, height: number}) {
    super(
        'Attempted to ' + action + ' (' + drawing + ') in bounds ' +
        bounds.toString());
  }
}
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

const CANVAS_OVERDRAW_FACTOR = 2;

// TODO: The word 'Controller' makes it sound like it lives on the controller
// thread. Should find a better name.
/**
 * Creates a canvas with a context that is set up for compositor scrolling. The
 * canvas has a fixed height of visibleHeight * CANVAS_OVERDRAW_FACTOR. This
 * class is in charge of accepting new scrollTop value for the container element
 * of the canvas, so it can compute the top offset required to recenter the
 * canvas.
 */
export class CanvasController {
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;

  private scrollOffset = 0;

  // Number of additional pixels above/below for compositor scrolling.
  private extraHeightPerSide = 0;

  private canvasHeight = 0;
  private canvasWidth = 0;

  constructor() {
    this.canvas = document.createElement('canvas');

    const ctx = this.canvas.getContext('2d');

    if (!ctx) {
      throw new Error('Could not create canvas context');
    }

    this.ctx = ctx;
  }

  setDimensions(width: number, visibleCanvasHeight: number) {
    this.canvasWidth = width;
    this.canvasHeight = visibleCanvasHeight * CANVAS_OVERDRAW_FACTOR;
    this.extraHeightPerSide =
        Math.round((this.canvasHeight - visibleCanvasHeight) / 2);

    const dpr = window.devicePixelRatio;
    this.canvas.style.width = this.canvasWidth.toString() + 'px';
    this.canvas.style.height = this.canvasHeight.toString() + 'px';
    this.canvas.width = this.canvasWidth * dpr;
    this.canvas.height = this.canvasHeight * dpr;
    this.ctx.scale(dpr, dpr);
  }

  clear(): void {
    this.ctx.fillStyle = 'white';
    this.ctx.fillRect(0, 0, this.canvasWidth, this.canvasHeight);
  }

  getCanvasElement(): HTMLCanvasElement {
    return this.canvas;
  }

  /**
   * Returns the canvas 2D rendering context so it doesn't have to be recreated
   * from the canvas element.
   */
  get2DContext(): CanvasRenderingContext2D {
    return this.ctx;
  }

  /**
   * Places the canvas and its contents at the correct position.
   * Re-centers the canvas element in the current viewport, and sets the context
   * offsets such that the contents move up as we scroll, while rendering the
   * first track within the viewport.
   */
  updateScrollOffset(scrollOffset: number): void {
    this.scrollOffset = scrollOffset;
  }

  /**
   * Returns the desired y position of canvas relative to the
   * ScrollingTrackDisplay that owns this so the canvas is centered in the
   * visible area. Since we overdraw the canvas on top, this value can be
   * negative.
   */
  getCanvasYStart(): number {
    return this.scrollOffset - this.extraHeightPerSide;
  }

  // TODO(dproy): Need to write tests for this.
  isYBoundsOnCanvas(bounds: {yStart: number, yEnd: number}) {
    const canvasYStart = this.getCanvasYStart();
    const canvasYEnd = canvasYStart + this.canvasHeight;
    return (bounds.yEnd >= canvasYStart && bounds.yStart <= canvasYEnd);
  }
}

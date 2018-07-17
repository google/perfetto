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

import {RootVirtualContext} from './root_virtual_context';

const CANVAS_OVERDRAW_FACTOR = 2;

/**
 * Creates a canvas with a context that is set up for compositor scrolling.
 * Creates a canvas and a virtual context and handles their size and position
 * for smooth scrolling. The canvas is (width, height * CANVAS_OVERDRAW_FACTOR),
 * and through the virtual context behaves like (width, Inf).
 */
export class CanvasController {
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private rootVirtualContext: RootVirtualContext;

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
    this.rootVirtualContext = new RootVirtualContext(this.ctx);
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

    this.rootVirtualContext.setCanvasTopOffset(this.getCanvasTopOffset());
    this.rootVirtualContext.setCanvasSize(this.canvasWidth, this.canvasHeight);
  }

  clear(): void {
    this.ctx.fillStyle = 'white';
    this.ctx.fillRect(0, 0, this.canvasWidth, this.canvasHeight);
  }

  getContext(): RootVirtualContext {
    return this.rootVirtualContext;
  }

  getCanvasElement(): HTMLCanvasElement {
    return this.canvas;
  }

  /**
   * Places the canvas and its contents at the correct position.
   * Re-centers the canvas element in the current viewport, and sets the context
   * offsets such that the contents move up as we scroll, while rendering the
   * first track within the viewport.
   */
  updateScrollOffset(scrollOffset: number): void {
    this.scrollOffset = scrollOffset;
    this.rootVirtualContext.setCanvasTopOffset(this.getCanvasTopOffset());
  }

  getCanvasTopOffset(): number {
    return this.scrollOffset - this.extraHeightPerSide;
  }
}

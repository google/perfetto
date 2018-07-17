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

import {BoundingRect, VirtualCanvasContext} from './virtual_canvas_context';

/**
 * RootVirtualContext is a VirtualCanvasContext that has knowledge of the
 * actual canvas element and the scroll position, which it can use to determine
 * whether any rect is within the canvas. ChildVirtualContexts can use this to
 * determine whether they should execute their draw calls.
 */
export class RootVirtualContext extends VirtualCanvasContext {
  private boundingRect: BoundingRect = {x: 0, y: 0, width: 0, height: 0};
  private canvasWidth = 0;
  private canvasHeight = 0;
  private canvasTopOffset = 0;

  constructor(context: CanvasRenderingContext2D) {
    super(context);

    this.updateBoundingRect();
  }

  isOnCanvas(): boolean {
    return this.checkRectOnCanvas(this.getBoundingRect());
  }

  checkRectOnCanvas(boundingRect: BoundingRect): boolean {
    const canvasBottom = this.canvasTopOffset + this.canvasHeight;
    const rectBottom = boundingRect.y + boundingRect.height;
    const rectRight = boundingRect.x + boundingRect.width;

    const heightIntersects =
        boundingRect.y <= canvasBottom && rectBottom >= this.canvasTopOffset;
    const widthIntersects =
        boundingRect.x <= this.canvasWidth && rectRight >= 0;

    return heightIntersects && widthIntersects;
  }

  /**
   * This defines a BoundingRect that causes correct positioning of the context
   * contents due to the scroll position, without causing bounds checking.
   */
  private updateBoundingRect(): void {
    this.boundingRect = {
      // As the user scrolls down, the contents have to move up.
      y: this.canvasTopOffset * -1,
      x: 0,
      width: Infinity,
      height: Infinity
    };
  }

  setCanvasTopOffset(topOffset: number): void {
    this.canvasTopOffset = topOffset;
    this.updateBoundingRect();
  }

  setCanvasSize(canvasWidth: number, canvasHeight: number): void {
    this.canvasWidth = canvasWidth;
    this.canvasHeight = canvasHeight;
  }

  getBoundingRect(): BoundingRect {
    return this.boundingRect;
  }
}

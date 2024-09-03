// Copyright (C) 2024 The Android Open Source Project
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
 * Canvases have limits on their maximum size (which is determined by the
 * system). Usually, this limit is fairly large, but can be as small as
 * 4096x4096px on some machines.
 *
 * If we need a super large canvas, we need to use a different approach.
 *
 * Unless the user has a huge monitor, most of the time any sufficiently large
 * canvas will overflow it's container, so we assume this container is set to
 * scroll so that the user can actually see all of the canvas. We can take
 * advantage of the fact that users may only see a small portion of the canvas
 * at a time. So, if we position a small floating canvas element over the
 * viewport of the scrolling container, we can approximate a huge canvas using a
 * much smaller one.
 *
 * Given a target element and it's scrolling container, VirtualCanvas turns an
 * empty HTML element into a "virtual" canvas with virtually unlimited size
 * using the "floating" canvas technique described above.
 */

import {DisposableStack} from '../base/disposable_stack';
import {Bounds2D, Rect2D, Size2D} from '../base/geom';

export type LayoutShiftListener = (
  canvas: HTMLCanvasElement,
  rect: Rect2D,
) => void;

export type CanvasResizeListener = (
  canvas: HTMLCanvasElement,
  width: number,
  height: number,
) => void;

export interface VirtualCanvasOpts {
  // How much buffer to add above and below the visible window.
  overdrawPx: number;

  // If true, the canvas will remain within the bounds on the target element at
  // all times.
  //
  // If false, the canvas is allowed to overflow the bounds of the target
  // element to avoid resizing unnecessarily.
  avoidOverflowingContainer: boolean;
}

export class VirtualCanvas implements Disposable {
  private readonly _trash = new DisposableStack();
  private readonly _canvasElement: HTMLCanvasElement;
  private readonly _targetElement: HTMLElement;

  // Describes the offset of the canvas w.r.t. the "target" container
  private _canvasRect: Rect2D;
  private _layoutShiftListener?: LayoutShiftListener;
  private _canvasResizeListener?: CanvasResizeListener;

  /**
   * @param targetElement The element to turn into a virtual canvas. The
   * dimensions of this element are used to size the canvas, so ensure this
   * element is sized appropriately.
   * @param containerElement The scrolling container to be used for determining
   * the size and position of the canvas. The targetElement should be a child of
   * this element.
   * @param opts Setup options for the VirtualCanvas.
   */
  constructor(
    targetElement: HTMLElement,
    containerElement: Element,
    opts?: Partial<VirtualCanvasOpts>,
  ) {
    const {overdrawPx = 100, avoidOverflowingContainer} = opts ?? {};

    // Returns what the canvas rect should look like
    const getCanvasRect = () => {
      const containerRect = new Rect2D(
        containerElement.getBoundingClientRect(),
      );
      const targetElementRect = targetElement.getBoundingClientRect();

      // Calculate the intersection of the container's viewport and the target
      const intersection = containerRect.intersect(targetElementRect);

      // Pad the intersection by the overdraw amount
      const intersectionExpanded = intersection.expand(overdrawPx);

      // Intersect with the original target rect unless we want to avoid resizes
      const canvasTargetRect = avoidOverflowingContainer
        ? intersectionExpanded.intersect(targetElementRect)
        : intersectionExpanded;

      return canvasTargetRect.reframe(targetElementRect);
    };

    const updateCanvas = () => {
      let repaintRequired = false;

      const canvasRect = getCanvasRect();
      const canvasRectPrev = this._canvasRect;
      this._canvasRect = canvasRect;

      if (
        canvasRectPrev.width !== canvasRect.width ||
        canvasRectPrev.height !== canvasRect.height
      ) {
        // Canvas needs to change size, update its size
        canvas.style.width = `${canvasRect.width}px`;
        canvas.style.height = `${canvasRect.height}px`;
        this._canvasResizeListener?.(
          canvas,
          canvasRect.width,
          canvasRect.height,
        );
        repaintRequired = true;
      }

      if (
        canvasRectPrev.left !== canvasRect.left ||
        canvasRectPrev.top !== canvasRect.top
      ) {
        // Canvas needs to move, update the transform
        canvas.style.transform = `translate(${canvasRect.left}px, ${canvasRect.top}px)`;
        repaintRequired = true;
      }

      repaintRequired && this._layoutShiftListener?.(canvas, canvasRect);
    };

    containerElement.addEventListener('scroll', updateCanvas, {
      passive: true,
    });
    this._trash.defer(() =>
      containerElement.removeEventListener('scroll', updateCanvas),
    );

    // Resize observer callbacks are called once immediately
    const resizeObserver = new ResizeObserver(() => {
      updateCanvas();
    });

    resizeObserver.observe(containerElement);
    resizeObserver.observe(targetElement);
    this._trash.defer(() => {
      resizeObserver.disconnect();
    });

    // Ensures the canvas doesn't change the size of the target element
    targetElement.style.overflow = 'hidden';

    const canvas = document.createElement('canvas');
    canvas.style.position = 'absolute';
    targetElement.appendChild(canvas);
    this._trash.defer(() => {
      targetElement.removeChild(canvas);
    });

    this._canvasElement = canvas;
    this._targetElement = targetElement;
    this._canvasRect = new Rect2D({
      left: 0,
      top: 0,
      bottom: 0,
      right: 0,
    });
  }

  /**
   * Set the callback that gets called when the canvas element is moved or
   * resized, thus, invalidating the contents, and should be re-painted.
   *
   * @param cb The new callback.
   */
  setLayoutShiftListener(cb: LayoutShiftListener) {
    this._layoutShiftListener = cb;
  }

  /**
   * Set the callback that gets called when the canvas element is resized. This
   * might be a good opportunity to update the size of the canvas' draw buffer.
   *
   * @param cb The new callback.
   */
  setCanvasResizeListener(cb: CanvasResizeListener) {
    this._canvasResizeListener = cb;
  }

  /**
   * The floating canvas element.
   */
  get canvasElement(): HTMLCanvasElement {
    return this._canvasElement;
  }

  /**
   * The target element, i.e. the one passed to our constructor.
   */
  get targetElement(): HTMLElement {
    return this._targetElement;
  }

  /**
   * The size of the target element, aka the size of the virtual canvas.
   */
  get size(): Size2D {
    return {
      width: this._targetElement.clientWidth,
      height: this._targetElement.clientHeight,
    };
  }

  /**
   * Returns the rect of the floating canvas with respect to the target element.
   * This will need to be subtracted from any drawing operations to get the
   * right alignment within the virtual canvas.
   */
  get canvasRect(): Rect2D {
    return this._canvasRect;
  }

  /**
   * Stop listening to DOM events.
   */
  [Symbol.dispose]() {
    this._trash.dispose();
  }

  /**
   * Return true if a rect overlaps the floating canvas.
   * @param rect The rect to test.
   * @returns true if rect overlaps, false otherwise.
   */
  overlapsCanvas(rect: Bounds2D): boolean {
    const c = this._canvasRect;
    const y = rect.top < c.bottom && rect.bottom > c.top;
    const x = rect.left < c.right && rect.right > c.left;
    return x && y;
  }
}

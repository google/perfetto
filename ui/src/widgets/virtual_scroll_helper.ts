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

import {DisposableStack} from '../base/disposable_stack';
import {Bounds2D, Rect2D} from '../base/geom';

export interface VirtualScrollHelperOpts {
  overdrawPx: number;

  // How close we can get to undrawn regions before updating
  tolerancePx: number;

  callback: (r: Rect2D) => void;
}

export interface Data {
  opts: VirtualScrollHelperOpts;
  rect?: Bounds2D;
}

export class VirtualScrollHelper {
  private readonly _trash = new DisposableStack();
  private readonly _data: Data[] = [];

  constructor(
    sliderElement: HTMLElement,
    containerElement: Element,
    opts: VirtualScrollHelperOpts[] = [],
  ) {
    this._data = opts.map((opts) => {
      return {opts};
    });

    const recalculateRects = () => {
      this._data.forEach((data) =>
        recalculatePuckRect(sliderElement, containerElement, data),
      );
    };

    containerElement.addEventListener('scroll', recalculateRects, {
      passive: true,
    });
    this._trash.defer(() =>
      containerElement.removeEventListener('scroll', recalculateRects),
    );

    // Resize observer callbacks are called once immediately
    const resizeObserver = new ResizeObserver(() => {
      recalculateRects();
    });

    resizeObserver.observe(containerElement);
    resizeObserver.observe(sliderElement);
    this._trash.defer(() => {
      resizeObserver.disconnect();
    });
  }

  [Symbol.dispose]() {
    this._trash.dispose();
  }
}

function recalculatePuckRect(
  sliderElement: HTMLElement,
  containerElement: Element,
  data: Data,
): void {
  const {tolerancePx, overdrawPx, callback} = data.opts;
  if (!data.rect) {
    const targetPuckRect = getTargetPuckRect(
      sliderElement,
      containerElement,
      overdrawPx,
    );
    callback(targetPuckRect);
    data.rect = targetPuckRect;
  } else {
    const viewportRect = new Rect2D(containerElement.getBoundingClientRect());

    // Expand the viewportRect by the tolerance
    const viewportExpandedRect = viewportRect.expand(tolerancePx);

    const sliderClientRect = sliderElement.getBoundingClientRect();
    const viewportClamped = viewportExpandedRect.intersect(sliderClientRect);

    // Translate the puck rect into client space (currently in slider space)
    const puckClientRect = viewportClamped.translate({
      x: sliderClientRect.x,
      y: sliderClientRect.y,
    });

    // Check if the tolerance rect entirely contains the expanded viewport rect
    // If not, request an update
    if (!puckClientRect.contains(viewportClamped)) {
      const targetPuckRect = getTargetPuckRect(
        sliderElement,
        containerElement,
        overdrawPx,
      );
      callback(targetPuckRect);
      data.rect = targetPuckRect;
    }
  }
}

// Returns what the puck rect should look like
function getTargetPuckRect(
  sliderElement: HTMLElement,
  containerElement: Element,
  overdrawPx: number,
) {
  const sliderElementRect = sliderElement.getBoundingClientRect();
  const containerRect = new Rect2D(containerElement.getBoundingClientRect());

  // Calculate the intersection of the container's viewport and the target
  const intersection = containerRect.intersect(sliderElementRect);

  // Pad the intersection by the overdraw amount
  const intersectionExpanded = intersection.expand(overdrawPx);

  // Intersect with the original target rect unless we want to avoid resizes
  const targetRect = intersectionExpanded.intersect(sliderElementRect);

  return targetRect.reframe(sliderElementRect);
}

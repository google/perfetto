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
import {Bounds2D, Rect2D, Vector2D} from '../base/geom';

export interface VirtualScrollHelperZoneConfig {
  overdrawPx: number;

  // How close we can get to undrawn regions before updating
  tolerancePx: number;

  callback: (r: Rect2D) => void;
}

interface ZoneCache {
  readonly config: VirtualScrollHelperZoneConfig;
  rect?: Bounds2D;
}

// Constants for predictive scrolling
const VELOCITY_MULTIPLIER = 0.8;
const MAX_OFFSET_RATIO = 0.75;
const VELOCITY_THRESHOLD = 0.1; // px/ms
const SMOOTHING_FACTOR = 0.4;

/**
 * Calculate predictive offset based on scroll velocity.
 * Returns offset in pixels to shift the viewport in the scroll direction.
 */
function calculatePredictiveOffset(
  velocity: number,
  overdrawPx: number,
): number {
  if (Math.abs(velocity) < VELOCITY_THRESHOLD) {
    return 0;
  }

  const offset = velocity * VELOCITY_MULTIPLIER * overdrawPx;
  const maxOffset = overdrawPx * MAX_OFFSET_RATIO;
  return Math.max(-maxOffset, Math.min(maxOffset, offset));
}

export class VirtualScrollHelper {
  private readonly _trash = new DisposableStack();
  private readonly _data: ReadonlyArray<ZoneCache>;

  constructor(
    sliderElement: HTMLElement,
    containerElement: Element,
    zones: ReadonlyArray<VirtualScrollHelperZoneConfig>,
    useScrollVelocityCompensation: boolean = false,
  ) {
    this._data = zones.map((zone) => {
      return {config: zone};
    });

    let previousScrollOffset = 0;
    let previousScrollEventTimestamp: number | undefined;
    let previousScrollVelocity = 0;

    const recalculateRects = (scrollVelocity: number) => {
      this._data.forEach((data) => {
        recalculatePuckRect(
          sliderElement,
          containerElement,
          data,
          new Vector2D({x: 0, y: scrollVelocity}),
        );
      });
    };

    const handleScroll = (e: Event) => {
      if (!useScrollVelocityCompensation) {
        recalculateRects(0);
        return;
      }

      const target = e.target as Element;
      const delta = target.scrollTop - previousScrollOffset;
      const timeDelta =
        e.timeStamp - (previousScrollEventTimestamp ?? e.timeStamp);
      const scrollVelocity = timeDelta > 0 ? delta / timeDelta : 0;
      previousScrollOffset = target.scrollTop;
      previousScrollEventTimestamp = e.timeStamp;

      // Filter the scroll velocity to avoid spikes
      const filteredScrollVelocity =
        SMOOTHING_FACTOR * scrollVelocity +
        (1 - SMOOTHING_FACTOR) * previousScrollVelocity;
      previousScrollVelocity = filteredScrollVelocity;

      recalculateRects(filteredScrollVelocity);
    };

    containerElement.addEventListener('scroll', handleScroll, {
      passive: true,
    });
    this._trash.defer(() =>
      containerElement.removeEventListener('scroll', handleScroll),
    );

    // Resize observer callbacks are called once immediately
    const resizeObserver = new ResizeObserver(() => {
      recalculateRects(0);
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
  data: ZoneCache,
  velocity: Vector2D,
): void {
  const {tolerancePx, overdrawPx, callback} = data.config;

  if (!data.rect) {
    const targetPuckRect = getTargetPuckRect(
      sliderElement,
      containerElement,
      overdrawPx,
      velocity,
    );
    callback(targetPuckRect);
    data.rect = targetPuckRect;
  } else {
    const oldRect = data.rect;
    const viewportRect = new Rect2D(containerElement.getBoundingClientRect());

    // Expand the viewportRect by the tolerance
    const viewportExpandedRect = viewportRect.expand(tolerancePx);

    const sliderClientRect = sliderElement.getBoundingClientRect();
    const viewportClamped = viewportExpandedRect.intersect(sliderClientRect);

    const viewportInSliderCoods = viewportClamped.reframe(sliderClientRect);

    // Check if the old rect contains the current viewport with the expanded
    // tolerance, then we're all good, otherwise request an update.
    if (!new Rect2D(oldRect).contains(viewportInSliderCoods)) {
      const targetPuckRect = getTargetPuckRect(
        sliderElement,
        containerElement,
        overdrawPx,
        velocity,
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
  velocity: Vector2D,
) {
  const sliderElementRect = sliderElement.getBoundingClientRect();
  const containerRect = new Rect2D(containerElement.getBoundingClientRect());

  // Calculate the intersection of the container's viewport and the target
  const intersection = containerRect.intersect(sliderElementRect);

  // Apply predictive offset based on scroll velocity
  const offsetX = calculatePredictiveOffset(velocity.x, overdrawPx);
  const offsetY = calculatePredictiveOffset(velocity.y, overdrawPx);
  const shiftedIntersection = intersection.translate({x: offsetX, y: offsetY});

  // Pad the intersection by the overdraw amount
  const intersectionExpanded = shiftedIntersection.expand(overdrawPx);

  // Intersect with the original target rect unless we want to avoid resizes
  const targetRect = intersectionExpanded.intersect(sliderElementRect);

  return targetRect.reframe(sliderElementRect);
}

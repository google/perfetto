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
 * These interactions may be added to a ZonedInteractionHandler. They define
 * some common operations that are used by several parts of the timeline such as
 * shift+drag to pan and mouse wheel navigation.
 */

import {Rect2D} from '../../base/geom';
import {TimeScale} from '../../base/time_scale';
import {Zone} from '../../base/zoned_interaction_handler';
import {TraceImpl} from '../../core/trace_impl';

const WHEEL_ZOOM_SPEED = -0.02;

export function shiftDragPanInteraction(
  trace: TraceImpl,
  rect: Rect2D,
  timescale: TimeScale,
): Zone {
  return {
    id: 'drag-pan',
    area: rect,
    cursor: 'grab',
    keyModifier: 'shift',
    drag: {
      cursorWhileDragging: 'grabbing',
      onDrag: (e) => {
        trace.timeline.pan(timescale.pxToDuration(-e.deltaSinceLastEvent.x));
      },
    },
  };
}

export function wheelNavigationInteraction(
  trace: TraceImpl,
  rect: Rect2D,
  timescale: TimeScale,
): Zone {
  return {
    id: 'mouse-wheel-navigation',
    area: rect,
    onWheel: (e) => {
      if (Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
        const tDelta = timescale.pxToDuration(e.deltaX);
        trace.timeline.pan(tDelta);
      } else {
        if (e.ctrlKey) {
          const sign = e.deltaY < 0 ? -1 : 1;
          const deltaY = sign * Math.log2(1 + Math.abs(e.deltaY));
          const zoomPx = e.position.x - rect.left;
          const centerPoint = zoomPx / rect.width;
          trace.timeline.zoom(1 - deltaY * WHEEL_ZOOM_SPEED, centerPoint);
        }
      }
    },
  };
}

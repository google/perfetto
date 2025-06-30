// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {TimeScale} from '../../base/time_scale';
import {Time} from '../../base/time';
import {lynxPerfGlobals} from '../lynx_perf_globals';
import {DROP_FRAME_THRESHOLD} from '../constants';
import {UNEXPECTED_PINK} from '../../components/colorizer';

/**
 * Renders frame performance markers on the timeline canvas
 */
export function renderDoFrameTag(
  ctx: CanvasRenderingContext2D,
  timescale: TimeScale,
  y: number,
  radius: number,
  uri: string,
) {
  const trackMap = lynxPerfGlobals.state.trackUriToThreadMap.get(uri);
  if (!trackMap) {
    return;
  }
  const targetTrackId = trackMap.trackId;
  const frameDurationMap = lynxPerfGlobals.state.frameDurationMap;
  // Set color based on frame duration performance:
  // - Red: Very slow frames (≥2x threshold)
  // - Orange: Slow frames (≥threshold but <2x threshold)
  // - Green: Good frames (<threshold)
  for (const [key, value] of frameDurationMap) {
    if (value.trackId !== targetTrackId) {
      continue;
    }

    const x = timescale.timeToPx(Time.fromRaw(BigInt(key)));

    ctx.beginPath();
    ctx.arc(x, y, radius, 0, 2 * Math.PI);
    ctx.closePath();

    if (lynxPerfGlobals.state.filteredTraceSet.has(value.id)) {
      ctx.fillStyle = UNEXPECTED_PINK.disabled.cssString;
    } else {
      ctx.fillStyle =
        value.dur >= DROP_FRAME_THRESHOLD * 2
          ? 'rgb(180, 0, 0)'
          : value.dur >= DROP_FRAME_THRESHOLD
            ? 'rgb(180, 125, 0)'
            : 'rgb(0, 125, 0)';
    }
    ctx.fill();

    ctx.font = '10px Roboto Condensed';
    ctx.fillStyle = 'white';
    ctx.textBaseline = 'middle';
    ctx.fillText('F', x - radius / 3, y + 0.4);
  }
}

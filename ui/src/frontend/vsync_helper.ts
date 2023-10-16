// Copyright (C) 2023 The Android Open Source Project
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

import {getCssStr} from './css_constants';
import {globals, VsyncData} from './globals';

// Get the vsync data that the user has opted to highlight under the tracks.
export function getActiveVsyncData(): VsyncData|undefined {
  return globals.frontendLocalState.vsyncHighlight ?
    globals.vsyncData :
    undefined;
}

// Render columns where the SurfaceFlinger VSYNC-app counter is on
// in the background of a track or track group of the given |height|,
// as indicated by the |vsync| data.
//
// Preconditions:
//  - the rendering |ctx| is saved, if necessary, because
//    this function reconfigures it
//  - the rendering |ctx| is translated to put the origin at the beginning
//    edge of the track or track group
export function renderVsyncColumns(ctx: CanvasRenderingContext2D,
    height: number, vsync: VsyncData) {
  const vsyncBackground = getCssStr('--track-vsync-background') ?? '#e7e7e7';
  const {visibleWindowTime, visibleTimeScale} = globals.frontendLocalState;
  const startPx =
    Math.floor(visibleTimeScale.hpTimeToPx(visibleWindowTime.start));
  const endPx =
    Math.floor(visibleTimeScale.hpTimeToPx(visibleWindowTime.end));
  const startTs = visibleWindowTime.start.toTPTime();
  const endTs = visibleWindowTime.end.toTPTime();

  ctx.fillStyle = vsyncBackground;

  let fill = vsync.initiallyOn;
  let lastX = startPx;
  for (const ts of vsync.toggleTs) {
    if (ts < startTs) {
      fill = !fill;
      continue;
    }
    const x = visibleTimeScale.tpTimeToPx(ts);
    if (fill) {
      ctx.fillRect(lastX, 0, x - lastX, height);
    }
    if (ts > endTs) {
      break;
    }
    lastX = x;
    fill = !fill;
  }

  // Do we need to fill out to the end?
  if (fill && lastX > startPx && lastX < endPx) {
    ctx.fillRect(lastX, 0, endPx - lastX, height);
  }
}


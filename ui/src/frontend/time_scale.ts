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

import {TimeSpan} from '../common/time';

/**
 * Defines a mapping between number and Milliseconds for the entire application.
 * Linearly scales time values from boundsMs to pixel values in boundsPx and
 * back.
 */
export class TimeScale {
  private timeBounds: TimeSpan;
  private startPx: number;
  private endPx: number;
  private secPerPx = 0;

  constructor(timeBounds: TimeSpan, boundsPx: [number, number]) {
    this.timeBounds = timeBounds;
    this.startPx = boundsPx[0];
    this.endPx = boundsPx[1];
    this.updateSlope();
  }

  private updateSlope() {
    this.secPerPx = this.timeBounds.duration / (this.endPx - this.startPx);
  }

  deltaTimeToPx(time: number): number {
    return Math.round(time / this.secPerPx);
  }

  timeToPx(time: number): number {
    return this.startPx + (time - this.timeBounds.start) / this.secPerPx;
  }

  pxToTime(px: number): number {
    return this.timeBounds.start + (px - this.startPx) * this.secPerPx;
  }

  deltaPxToDuration(px: number): number {
    return px * this.secPerPx;
  }

  setTimeBounds(timeBounds: TimeSpan) {
    this.timeBounds = timeBounds;
    this.updateSlope();
  }

  setLimitsPx(pxStart: number, pxEnd: number) {
    this.startPx = pxStart;
    this.endPx = pxEnd;
    this.updateSlope();
  }
}

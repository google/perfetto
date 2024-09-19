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

import {duration, time} from './time';
import {HighPrecisionTime} from './high_precision_time';
import {HighPrecisionTimeSpan} from './high_precision_time_span';
import {HorizontalBounds} from './geom';

export class TimeScale {
  readonly timeSpan: HighPrecisionTimeSpan;
  readonly pxBounds: HorizontalBounds;
  private readonly timePerPx: number;

  constructor(timespan: HighPrecisionTimeSpan, pxBounds: HorizontalBounds) {
    this.pxBounds = pxBounds;
    this.timeSpan = timespan;
    const delta = pxBounds.right - pxBounds.left;
    if (timespan.duration <= 0 || delta <= 0) {
      this.timePerPx = 1;
    } else {
      this.timePerPx = timespan.duration / delta;
    }
  }

  timeToPx(ts: time): number {
    const timeOffset =
      Number(ts - this.timeSpan.start.integral) -
      this.timeSpan.start.fractional;
    return this.pxBounds.left + timeOffset / this.timePerPx;
  }

  hpTimeToPx(time: HighPrecisionTime): number {
    const timeOffset = time.sub(this.timeSpan.start).toNumber();
    return this.pxBounds.left + timeOffset / this.timePerPx;
  }

  // Convert pixels to a high precision time object, which can be further
  // converted to other time formats.
  pxToHpTime(px: number): HighPrecisionTime {
    const timeOffset = (px - this.pxBounds.left) * this.timePerPx;
    return this.timeSpan.start.addNumber(timeOffset);
  }

  durationToPx(dur: duration): number {
    return Number(dur) / this.timePerPx;
  }

  pxToDuration(pxDelta: number): number {
    return pxDelta * this.timePerPx;
  }
}

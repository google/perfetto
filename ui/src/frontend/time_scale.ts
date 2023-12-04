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

import {assertTrue} from '../base/logging';
import {duration, Span, time} from '../base/time';
import {
  HighPrecisionTime,
  HighPrecisionTimeSpan,
} from '../common/high_precision_time';

export class TimeScale {
  private _start: HighPrecisionTime;
  private _durationNanos: number;
  readonly pxSpan: PxSpan;
  private _nanosPerPx = 0;

  static fromHPTimeSpan(span: Span<HighPrecisionTime>, pxSpan: PxSpan) {
    return new TimeScale(span.start, span.duration.nanos, pxSpan);
  }

  constructor(start: HighPrecisionTime, durationNanos: number, pxSpan: PxSpan) {
    this.pxSpan = pxSpan;
    this._start = start;
    this._durationNanos = durationNanos;
    if (durationNanos <= 0 || pxSpan.delta <= 0) {
      this._nanosPerPx = 1;
    } else {
      this._nanosPerPx = durationNanos / (pxSpan.delta);
    }
  }

  get timeSpan(): Span<HighPrecisionTime> {
    const end = this._start.addNanos(this._durationNanos);
    return new HighPrecisionTimeSpan(this._start, end);
  }

  timeToPx(ts: time): number {
    // WARNING: Number(bigint) can be surprisingly slow. Avoid in hotpath.
    const timeOffsetNanos = Number(ts - this._start.base) - this._start.offset;
    return this.pxSpan.start + timeOffsetNanos / this._nanosPerPx;
  }

  hpTimeToPx(time: HighPrecisionTime): number {
    const timeOffsetNanos = time.sub(this._start).nanos;
    return this.pxSpan.start + timeOffsetNanos / this._nanosPerPx;
  }

  // Convert pixels to a high precision time object, which can be futher
  // converted to other time formats.
  pxToHpTime(px: number): HighPrecisionTime {
    const offsetNanos = (px - this.pxSpan.start) * this._nanosPerPx;
    return this._start.addNanos(offsetNanos);
  }

  durationToPx(dur: duration): number {
    // WARNING: Number(bigint) can be surprisingly slow. Avoid in hotpath.
    return Number(dur) / this._nanosPerPx;
  }

  pxDeltaToDuration(pxDelta: number): HighPrecisionTime {
    const time = pxDelta * this._nanosPerPx;
    return HighPrecisionTime.fromNanos(time);
  }
}

export class PxSpan {
  static readonly ZERO = new PxSpan(0, 0);

  constructor(private _start: number, private _end: number) {
    assertTrue(_start <= _end, 'PxSpan start > end');
  }

  get start(): number {
    return this._start;
  }

  get end(): number {
    return this._end;
  }

  get delta(): number {
    return this._end - this._start;
  }
}

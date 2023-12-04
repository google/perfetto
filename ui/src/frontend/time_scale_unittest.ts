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

import {Time} from '../base/time';
import {HighPrecisionTime} from '../common/high_precision_time';

import {PxSpan, TimeScale} from './time_scale';

describe('TimeScale', () => {
  const ts =
      new TimeScale(new HighPrecisionTime(40n), 100, new PxSpan(200, 1000));

  it('converts timescales to pixels', () => {
    expect(ts.timeToPx(Time.fromRaw(40n))).toEqual(200);
    expect(ts.timeToPx(Time.fromRaw(140n))).toEqual(1000);
    expect(ts.timeToPx(Time.fromRaw(90n))).toEqual(600);

    expect(ts.timeToPx(Time.fromRaw(240n))).toEqual(1800);
    expect(ts.timeToPx(Time.fromRaw(-60n))).toEqual(-600);
  });

  it('converts pixels to HPTime objects', () => {
    let result = ts.pxToHpTime(200);
    expect(result.base).toEqual(40n);
    expect(result.offset).toBeCloseTo(0);

    result = ts.pxToHpTime(1000);
    expect(result.base).toEqual(140n);
    expect(result.offset).toBeCloseTo(0);

    result = ts.pxToHpTime(600);
    expect(result.base).toEqual(90n);
    expect(result.offset).toBeCloseTo(0);

    result = ts.pxToHpTime(1800);
    expect(result.base).toEqual(240n);
    expect(result.offset).toBeCloseTo(0);

    result = ts.pxToHpTime(-600);
    expect(result.base).toEqual(-60n);
    expect(result.offset).toBeCloseTo(0);
  });

  it('converts durations to pixels', () => {
    expect(ts.durationToPx(0n)).toEqual(0);
    expect(ts.durationToPx(1n)).toEqual(8);
    expect(ts.durationToPx(1000n)).toEqual(8000);
  });

  it('converts pxDeltaToDurations to HPTime durations', () => {
    let result = ts.pxDeltaToDuration(0);
    expect(result.base).toEqual(0n);
    expect(result.offset).toBeCloseTo(0);

    result = ts.pxDeltaToDuration(1);
    expect(result.base).toEqual(0n);
    expect(result.offset).toBeCloseTo(0.125);

    result = ts.pxDeltaToDuration(100);
    expect(result.base).toEqual(12n);
    expect(result.offset).toBeCloseTo(0.5);
  });
});

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
import {Span, tpDurationToSeconds} from '../common/time';
import {TPDuration, TPTime, TPTimeSpan} from '../common/time';

import {TRACK_BORDER_COLOR, TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {TimeScale} from './time_scale';

const micros = 1000n;
const millis = 1000n * micros;
const seconds = 1000n * millis;
const minutes = 60n * seconds;
const hours = 60n * minutes;
const days = 24n * hours;

// These patterns cover the entire range of 0 - 2^63-1 nanoseconds
const patterns: [bigint, string][] = [
  [1n, '|'],
  [2n, '|:'],
  [5n, '|....'],
  [10n, '|....:....'],
  [20n, '|.:.'],
  [50n, '|....'],
  [100n, '|....:....'],
  [200n, '|.:.'],
  [500n, '|....'],
  [1n * micros, '|....:....'],
  [2n * micros, '|.:.'],
  [5n * micros, '|....'],
  [10n * micros, '|....:....'],
  [20n * micros, '|.:.'],
  [50n * micros, '|....'],
  [100n * micros, '|....:....'],
  [200n * micros, '|.:.'],
  [500n * micros, '|....'],
  [1n * millis, '|....:....'],
  [2n * millis, '|.:.'],
  [5n * millis, '|....'],
  [10n * millis, '|....:....'],
  [20n * millis, '|.:.'],
  [50n * millis, '|....'],
  [100n * millis, '|....:....'],
  [200n * millis, '|.:.'],
  [500n * millis, '|....'],
  [1n * seconds, '|....:....'],
  [2n * seconds, '|.:.'],
  [5n * seconds, '|....'],
  [10n * seconds, '|....:....'],
  [30n * seconds, '|.:.:.'],
  [1n * minutes, '|.....'],
  [2n * minutes, '|.:.'],
  [5n * minutes, '|.....'],
  [10n * minutes, '|....:....'],
  [30n * minutes, '|.:.:.'],
  [1n * hours, '|.....'],
  [2n * hours, '|.:.'],
  [6n * hours, '|.....'],
  [12n * hours, '|.....:.....'],
  [1n * days, '|.:.'],
  [2n * days, '|.:.'],
  [5n * days, '|....'],
  [10n * days, '|....:....'],
  [20n * days, '|.:.'],
  [50n * days, '|....'],
  [100n * days, '|....:....'],
  [200n * days, '|.:.'],
  [500n * days, '|....'],
  [1000n * days, '|....:....'],
  [2000n * days, '|.:.'],
  [5000n * days, '|....'],
  [10000n * days, '|....:....'],
  [20000n * days, '|.:.'],
  [50000n * days, '|....'],
  [100000n * days, '|....:....'],
  [200000n * days, '|.:.'],
];

// Returns the optimal step size and pattern of ticks within the step.
export function getPattern(minPatternSize: bigint): [TPDuration, string] {
  for (const [size, pattern] of patterns) {
    if (size >= minPatternSize) {
      return [size, pattern];
    }
  }

  throw new Error('Pattern not defined for this minsize');
}

function tickPatternToArray(pattern: string): TickType[] {
  const array = Array.from(pattern);
  return array.map((char) => {
    switch (char) {
      case '|':
        return TickType.MAJOR;
      case ':':
        return TickType.MEDIUM;
      case '.':
        return TickType.MINOR;
      default:
        // This is almost certainly a developer/fat-finger error
        throw Error(`Invalid char "${char}" in pattern "${pattern}"`);
    }
  });
}

// Get the number of decimal places we would have to print a time to for a given
// min step size. For example, if we know the min step size is 0.1 and all
// values are going to be aligned to integral multiples of 0.1, there's no
// point printing these values with more than 1 decimal place.
// Note: It's assumed that stepSize only has one significant figure.
// E.g. 0.3 and 0.00002 are fine, but 0.123 will be treated as if it were 0.1.
// Some examples: (seconds -> decimal places)
//  1.0 -> 0
//  0.5 -> 1
//  0.009 -> 3
//  0.00007 -> 5
//  30000 -> 0
//  0.30000000000000004 -> 1
export function guessDecimalPlaces(stepSize: TPDuration): number {
  const stepSizeSeconds = tpDurationToSeconds(stepSize);
  const decimalPlaces = -Math.floor(Math.log10(stepSizeSeconds));
  return Math.max(0, decimalPlaces);
}

export enum TickType {
  MAJOR,
  MEDIUM,
  MINOR
}

export interface Tick {
  type: TickType;
  time: TPTime;
}

export const MIN_PX_PER_STEP = 120;
export function getMaxMajorTicks(width: number) {
  return Math.max(1, Math.floor(width / MIN_PX_PER_STEP));
}

function roundDownNearest(time: TPTime, stepSize: TPDuration): TPTime {
  return stepSize * (time / stepSize);
}

// An iterable which generates a series of ticks for a given timescale.
export class TickGenerator implements Iterable<Tick> {
  private _tickPattern: TickType[];
  private _patternSize: TPDuration;
  private _timeSpan: Span<TPTime>;
  private _offset: TPTime;

  constructor(
      timeSpan: Span<TPTime>, maxMajorTicks: number, offset: TPTime = 0n) {
    assertTrue(timeSpan.duration > 0n, 'timeSpan.duration cannot be lte 0');
    assertTrue(maxMajorTicks > 0, 'maxMajorTicks cannot be lte 0');

    this._timeSpan = timeSpan.add(-offset);
    this._offset = offset;
    const minStepSize =
        BigInt(Math.floor(Number(timeSpan.duration) / maxMajorTicks));
    const [size, pattern] = getPattern(minStepSize);
    this._patternSize = size;
    this._tickPattern = tickPatternToArray(pattern);
  }

  // Returns an iterable, so this object can be iterated over directly using the
  // `for x of y` notation. The use of a generator here is just to make things
  // more elegant compared to creating an array of ticks and building an
  // iterator for it.
  * [Symbol.iterator](): Generator<Tick> {
    const stepSize = this._patternSize / BigInt(this._tickPattern.length);
    const start = roundDownNearest(this._timeSpan.start, this._patternSize);
    const end = this._timeSpan.end;
    let patternIndex = 0;

    for (let time = start; time < end; time += stepSize, patternIndex++) {
      if (time >= this._timeSpan.start) {
        patternIndex = patternIndex % this._tickPattern.length;
        const type = this._tickPattern[patternIndex];
        yield {type, time: time + this._offset};
      }
    }
  }

  get digits(): number {
    return guessDecimalPlaces(this._patternSize);
  }
}

// Gets the timescale associated with the current visible window.
export function timeScaleForVisibleWindow(
    startPx: number, endPx: number): TimeScale {
  return globals.frontendLocalState.getTimeScale(startPx, endPx);
}

export function drawGridLines(
    ctx: CanvasRenderingContext2D,
    width: number,
    height: number): void {
  ctx.strokeStyle = TRACK_BORDER_COLOR;
  ctx.lineWidth = 1;

  const {earliest, latest} = globals.frontendLocalState.visibleWindow;
  const span = new TPTimeSpan(earliest, latest);
  if (width > TRACK_SHELL_WIDTH && span.duration > 0n) {
    const maxMajorTicks = getMaxMajorTicks(width - TRACK_SHELL_WIDTH);
    const map = timeScaleForVisibleWindow(TRACK_SHELL_WIDTH, width);
    for (const {type, time} of new TickGenerator(
             span, maxMajorTicks, globals.state.traceTime.start)) {
      const px = Math.floor(map.tpTimeToPx(time));
      if (type === TickType.MAJOR) {
        ctx.beginPath();
        ctx.moveTo(px + 0.5, 0);
        ctx.lineTo(px + 0.5, height);
        ctx.stroke();
      }
    }
  }
}

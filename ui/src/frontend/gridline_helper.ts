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
import {roundDownNearest} from '../base/math_utils';
import {TRACK_BORDER_COLOR, TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {TimeScale} from './time_scale';

// Returns the optimal step size (in seconds) and tick pattern of ticks within
// the step. The returned step size has two properties: (1) It is 1, 2, or 5,
// multiplied by some integer power of 10. (2) It is maximised given the
// constraint: |range| / stepSize <= |maxNumberOfSteps|.
export function getStepSize(
    range: number, maxNumberOfSteps: number): [number, string] {
  // First, get the largest possible power of 10 that is smaller than the
  // desired step size, and use it as our initial step size.
  // For example, if the range is 2345ms and the desired steps is 10, then the
  // minimum step size is 234.5ms so the step size will initialise to 100.
  const minStepSize = range / maxNumberOfSteps;
  const zeros = Math.floor(Math.log10(minStepSize));
  const initialStepSize = Math.pow(10, zeros);

  // We know that |initialStepSize| is a power of 10, and
  // initialStepSize <= desiredStepSize <= 10 * initialStepSize. There are four
  // possible candidates for final step size: 1, 2, 5 or 10 * initialStepSize.
  // For our example above, this would result in a step size of 500ms, as both
  // 100ms and 200ms are smaller than the minimum step size of 234.5ms.
  // We pick the candidate that minimizes the step size without letting the
  // number of steps exceed |maxNumberOfSteps|. The factor we pick to also
  // determines the pattern of ticks. This pattern is represented using a string
  // where:
  //  | = Major tick
  //  : = Medium tick
  //  . = Minor tick
  const stepSizeMultipliers: [number, string][] =
      [[1, '|....:....'], [2, '|.:.'], [5, '|....'], [10, '|....:....']];

  for (const [multiplier, pattern] of stepSizeMultipliers) {
    const newStepSize = multiplier * initialStepSize;
    const numberOfNewSteps = range / newStepSize;
    if (numberOfNewSteps <= maxNumberOfSteps) {
      return [newStepSize, pattern];
    }
  }

  throw new Error('Something has gone horribly wrong with maths');
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

// Assuming a number only has one non-zero decimal digit, find the number of
// decimal places required to accurately print that number. I.e. the parameter
// we should pass to number.toFixed(x). To account for floating point
// innaccuracies when representing numbers in base-10, we only take the first
// nonzero fractional digit into account. E.g.
//  1.0 -> 0
//  0.5 -> 1
//  0.009 -> 3
//  0.00007 -> 5
//  30000 -> 0
//  0.30000000000000004 -> 1
export function guessDecimalPlaces(val: number): number {
  const neglog10 = -Math.floor(Math.log10(val));
  const clamped = Math.max(0, neglog10);
  return clamped;
}

export enum TickType {
  MAJOR,
  MEDIUM,
  MINOR
}

export interface Tick {
  type: TickType;
  time: number;
  position: number;
}

const MIN_PX_PER_STEP = 80;

// An iterable which generates a series of ticks for a given timescale.
export class TickGenerator implements Iterable<Tick> {
  private _tickPattern: TickType[];
  private _patternSize: number;

  constructor(private scale: TimeScale, {minLabelPx = MIN_PX_PER_STEP} = {}) {
    assertTrue(minLabelPx > 0, 'minLabelPx cannot be lte 0');
    assertTrue(scale.widthPx > 0, 'widthPx cannot be lte 0');
    assertTrue(
        scale.timeSpan.duration > 0, 'timeSpan.duration cannot be lte 0');

    const desiredSteps = scale.widthPx / minLabelPx;
    const [size, pattern] = getStepSize(scale.timeSpan.duration, desiredSteps);
    this._patternSize = size;
    this._tickPattern = tickPatternToArray(pattern);
  }

  // Returns an iterable, so this object can be iterated over directly using the
  // `for x of y` notation. The use of a generator here is just to make things
  // more elegant than creating an array of ticks and building an iterator for
  // it.
  * [Symbol.iterator](): Generator<Tick> {
    const span = this.scale.timeSpan;
    const stepSize = this._patternSize / this._tickPattern.length;
    const start = roundDownNearest(span.start, this._patternSize);
    const timeAtStep = (i: number) => start + (i * stepSize);

    // Iterating using steps instead of
    // for (let s = start; s < span.end; s += stepSize) because if start is much
    // larger than stepSize we can enter an infinite loop due to floating
    // point precision errors.
    for (let i = 0; timeAtStep(i) < span.end; i++) {
      const time = timeAtStep(i);
      if (time >= span.start) {
        const position = Math.floor(this.scale.timeToPx(time));
        const type = this._tickPattern[i % this._tickPattern.length];
        yield {type, time, position};
      }
    }
  }

  // The number of decimal places labels should be printed with, assuming labels
  // are only printed on major ticks.
  get digits(): number {
    return guessDecimalPlaces(this._patternSize);
  }
}

// Gets the timescale associated with the current visible window.
export function timeScaleForVisibleWindow(
    startPx: number, endPx: number): TimeScale {
  const span = globals.frontendLocalState.visibleWindowTime;
  const spanRelative = span.add(-globals.state.traceTime.startSec);
  return new TimeScale(spanRelative, [startPx, endPx]);
}

export function drawGridLines(
    ctx: CanvasRenderingContext2D,
    width: number,
    height: number): void {
  ctx.strokeStyle = TRACK_BORDER_COLOR;
  ctx.lineWidth = 1;

  const timeScale = timeScaleForVisibleWindow(TRACK_SHELL_WIDTH, width);
  if (timeScale.timeSpan.duration > 0 && timeScale.widthPx > 0) {
    for (const {type, position} of new TickGenerator(timeScale)) {
      if (type === TickType.MAJOR) {
        ctx.beginPath();
        ctx.moveTo(position + 0.5, 0);
        ctx.lineTo(position + 0.5, height);
        ctx.stroke();
      }
    }
  }
}

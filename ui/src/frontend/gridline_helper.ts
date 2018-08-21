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

import {TimeScale} from './time_scale';

export const DESIRED_PX_PER_STEP = 80;

export function drawGridLines(
    ctx: CanvasRenderingContext2D,
    x: TimeScale,
    timeSpan: TimeSpan,
    height: number): void {
  const width = x.deltaTimeToPx(timeSpan.duration);
  const desiredSteps = width / DESIRED_PX_PER_STEP;
  const step = getGridStepSize(timeSpan.duration, desiredSteps);
  const start = Math.round(timeSpan.start / step) * step;

  ctx.strokeStyle = '#999999';
  ctx.lineWidth = 1;

  for (let sec = start; sec < timeSpan.end; sec += step) {
    const xPos = Math.floor(x.timeToPx(sec)) + 0.5;

    if (xPos >= 0 && xPos <= width) {
      ctx.beginPath();
      ctx.moveTo(xPos, 0);
      ctx.lineTo(xPos, height);
      ctx.stroke();
    }
  }
}

/**
 * Returns the step size of a grid line in seconds.
 * The returned step size has two properties:
 * (1) It is 1, 2, or 5, multiplied by some integer power of 10.
 * (2) The number steps in |range| produced by |stepSize| is as close as
 *     possible to |desiredSteps|.
 */
export function getGridStepSize(range: number, desiredSteps: number): number {
  // First, get the largest possible power of 10 that is smaller than the
  // desired step size, and set it to the current step size.
  // For example, if the range is 2345ms and the desired steps is 10, then the
  // desired step size is 234.5 and the step size will be set to 100.
  const desiredStepSize = range / desiredSteps;
  const zeros = Math.floor(Math.log10(desiredStepSize));
  const initialStepSize = Math.pow(10, zeros);

  // This function first calculates how many steps within the range a certain
  // stepSize will produce, and returns the difference between that and
  // desiredSteps.
  const distToDesired = (evaluatedStepSize: number) =>
      Math.abs(range / evaluatedStepSize - desiredSteps);

  // We know that |initialStepSize| is a power of 10, and
  // initialStepSize <= desiredStepSize <= 10 * initialStepSize. There are four
  // possible candidates for final step size: 1, 2, 5 or 10 * initialStepSize.
  // We pick the candidate that minimizes distToDesired(stepSize).
  const stepSizeMultipliers = [2, 5, 10];

  let minimalDistance = distToDesired(initialStepSize);
  let minimizingStepSize = initialStepSize;

  for (const multiplier of stepSizeMultipliers) {
    const newStepSize = multiplier * initialStepSize;
    const newDistance = distToDesired(newStepSize);
    if (newDistance < minimalDistance) {
      minimalDistance = newDistance;
      minimizingStepSize = newStepSize;
    }
  }
  return minimizingStepSize;
}
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

import {BigintMath} from '../base/bigint_math';
import {duration} from '../base/time';
import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';

/**
 * Work out an appropriate "resolution" for a given time span stretched over a
 * given number of pixels.
 *
 * The returned value will be rounded down to the nearest power of 2, and will
 * always be >= 1.
 *
 * @param timeSpan The span of time to represent.
 * @param widthPx How many pixels we have to represent the time span.
 * @returns The resultant resolution.
 */
export function calculateResolution(
  timeSpan: HighPrecisionTimeSpan,
  widthPx: number,
): duration {
  // Work out how much time corresponds to one pixel
  const timePerPixel = Number(timeSpan.duration) / widthPx;

  // Round down to the nearest power of 2, noting that the smallest value this
  // function can return is 1
  return BigintMath.bitFloor(BigInt(Math.floor(timePerPixel)));
}

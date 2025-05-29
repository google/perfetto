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

import {BigintMath} from '../../base/bigint_math';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {errResult, okResult, Result} from '../../base/result';
import {duration} from '../../base/time';

/**
 * Work out an appropriate "resolution" for a given time span stretched over a
 * given number of pixels, rounded down to the nearest power of 2.
 *
 * @param timeSpan The span of time to represent.
 * @param widthPx How many pixels we have to represent the time span.
 * @returns The resultant resolution, or an error if the input parameters are
 * invalid.
 */
export function calculateResolution(
  timeSpan: HighPrecisionTimeSpan,
  widthPx: number,
): Result<duration> {
  if (widthPx <= 0) {
    return errResult('Parameter "widthPx" must be greater than 0.');
  }

  const dur = timeSpan.duration;
  if (dur <= 0) {
    return errResult(
      'The duration of the "timeSpan" parameter must be greater than 0.',
    );
  }

  // Work out how much time corresponds to one pixel.
  const timePerPixel = Number(dur) / widthPx;

  // Convert to a bigint and round down to the nearest power of 2.
  return okResult(BigintMath.bitFloor(BigInt(Math.floor(timePerPixel))));
}

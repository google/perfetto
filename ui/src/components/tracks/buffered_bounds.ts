// Copyright (C) 2026 The Android Open Source Project
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

import {BigintMath as BIMath} from '../../base/bigint_math';
import {duration, time, Time} from '../../base/time';
import {TimeSpan} from '../../base/time';

/**
 * Manages buffered/skirt bounds for track data fetching.
 *
 * The strategy is to fetch data for a window larger than the visible area
 * (by padding each side by the visible duration). This reduces the frequency
 * of data fetches when panning/scrolling, as new data is only fetched when
 * the visible window exceeds the loaded bounds.
 *
 * The bounds are also quantized to the resolution to ensure consistent
 * cache keys.
 */
export class BufferedBounds {
  private loadedStart: time = Time.ZERO;
  private loadedEnd: time = Time.ZERO;
  private loadedResolution: duration = 0n;

  /**
   * Updates the buffered bounds if needed and returns the current bounds.
   *
   * @param visibleSpan The currently visible time span
   * @param resolution The current resolution (must be a power of 2)
   * @returns The buffered bounds to use for data fetching
   */
  update(
    visibleSpan: TimeSpan,
    resolution: duration,
  ): {start: time; end: time; resolution: duration} {
    const {start: visStart, end: visEnd} = visibleSpan;

    const needsUpdate =
      visStart < this.loadedStart ||
      visEnd > this.loadedEnd ||
      resolution !== this.loadedResolution;

    if (needsUpdate) {
      // Pad each side by the visible duration (so total is 3x visible width)
      const padded = visibleSpan.pad(visibleSpan.duration);
      this.loadedStart = Time.fromRaw(
        BIMath.quantFloor(padded.start, resolution),
      );
      this.loadedEnd = Time.fromRaw(BIMath.quantCeil(padded.end, resolution));
      this.loadedResolution = resolution;
    }

    return {
      start: this.loadedStart,
      end: this.loadedEnd,
      resolution: this.loadedResolution,
    };
  }

  /**
   * Gets the current loaded bounds without updating them.
   */
  get bounds(): {start: time; end: time; resolution: duration} {
    return {
      start: this.loadedStart,
      end: this.loadedEnd,
      resolution: this.loadedResolution,
    };
  }

  /**
   * Resets the bounds to their initial state, forcing a refetch on next update.
   */
  reset(): void {
    this.loadedStart = Time.ZERO;
    this.loadedEnd = Time.ZERO;
    this.loadedResolution = 0n;
  }
}

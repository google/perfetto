// Copyright (C) 2025 The Android Open Source Project
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

import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {duration, time} from '../base/time';

/**
 * Represents a single cell in the minimap, containing data for a specific time
 * range.
 */
export interface MinimapCell {
  /**
   * The start timestamp of the cell.
   */
  readonly ts: time;

  /**
   * The duration of the cell.
   */
  readonly dur: duration;

  /**
   * The load value for the cell, typically a normalized value between 0 and 1.
   */
  readonly load: number;
}

/**
 * Represents a row of data in the minimap.
 */
export type MinimapRow = readonly MinimapCell[];

/**
 * Provides content for the minimap.
 */
export interface MinimapContentProvider {
  /**
   * The priority of this content provider. Higher priority providers are
   * preferred.
   */
  readonly priority: number;

  /**
   * Gets the data for the minimap for a given time span and resolution.
   * @param timeSpan The time span for which to provide data.
   * @param resolution The resolution at which to provide the data.
   * @returns A promise that resolves to an array of minimap rows.
   */
  getData(
    timeSpan: HighPrecisionTimeSpan,
    resolution: duration,
  ): Promise<MinimapRow[]>;
}

/**
 * Manages content providers for the minimap.
 */
export interface MinimapManager {
  /**
   * Registers a new content provider for the minimap.
   * @param provider The content provider to register.
   */
  registerContentProvider(provider: MinimapContentProvider): void;
}

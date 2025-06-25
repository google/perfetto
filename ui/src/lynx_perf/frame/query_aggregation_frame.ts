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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {DROP_FRAME_THRESHOLD} from '../constants';
import {lynxPerfGlobals} from '../lynx_perf_globals';
import {isMainThreadTrack} from '../track_utils';
import {AreaSelection} from '../../public/selection';

/**
 * Interface representing aggregated frame statistics
 */
export interface FrameAggregationColumn {
  name: string;
  totalDuration: number;
  averageDuration: number;
  occurrences: number;
}

/**
 * Queries and aggregates frame rendering statistics for the selected area
 * @param area - The selected time range and tracks to analyze
 * @returns Array of frame statistics grouped by performance categories
 */
export function queryFrameRenderingAggregation(area: AreaSelection) {
  // Check if selection contains main thread tracks
  const containMainThread =
    area.tracks.find((value) => isMainThreadTrack(value.uri)) != undefined;
  const items: FrameAggregationColumn[] = [];
  if (containMainThread) {
    // Filter frame durations within selected time range
    const frameDurationArray = Array.from(
      lynxPerfGlobals.state.frameDurationMap.entries(),
    )
      .filter(([ts, _duration]) => ts >= area.start && ts <= area.end)
      .map(([_ts, duration]) => duration.dur);

    // Categorize frames by performance:
    // - Red: Very slow frames (≥2x threshold)
    // - Orange: Slow frames (≥threshold but <2x threshold)
    // - Green: Good frames (<threshold)
    const redFrames = frameDurationArray.filter(
      (duration) => duration >= DROP_FRAME_THRESHOLD * 2,
    );
    const orangeFrames = frameDurationArray.filter(
      (duration) =>
        duration >= DROP_FRAME_THRESHOLD && duration < DROP_FRAME_THRESHOLD * 2,
    );
    const greenFrames = frameDurationArray.filter(
      (duration) => duration < DROP_FRAME_THRESHOLD,
    );

    // Add aggregated stats for each category
    addFrameRenderingAggregationItem(items, redFrames, 'Frame Rendering: Red');
    addFrameRenderingAggregationItem(
      items,
      orangeFrames,
      'Frame Rendering: Orange',
    );
    addFrameRenderingAggregationItem(
      items,
      greenFrames,
      'Frame Rendering: Green',
    );
  }
  return items;
}

/**
 * Helper function to add aggregated frame stats to the results
 * @param items - Array to store the results
 * @param frameDurationArray - Array of frame durations for a category
 * @param tagName - Label for this category
 */
function addFrameRenderingAggregationItem(
  items: FrameAggregationColumn[],
  frameDurationArray: number[],
  tagName: string,
) {
  if (frameDurationArray.length > 0) {
    const totalDuration = frameDurationArray.reduce(
      (accumulator, currentValue) => {
        return accumulator + currentValue;
      },
      0,
    );
    items.push({
      name: tagName,
      totalDuration,
      averageDuration: Math.round(totalDuration / frameDurationArray.length),
      occurrences: frameDurationArray.length,
    });
  }
}

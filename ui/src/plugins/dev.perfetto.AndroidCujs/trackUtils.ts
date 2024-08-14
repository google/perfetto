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

import {globals} from '../../frontend/globals';
import {SimpleSliceTrackConfig} from '../../frontend/simple_slice_track';
import {addDebugSliceTrack, PluginContextTrace} from '../../public';
import {findCurrentSelection} from '../../frontend/keyboard_event_handler';
import {time, Time} from '../../base/time';
import {BigintMath} from '../../base/bigint_math';
import {reveal} from '../../frontend/scroll_helper';

/**
 * Adds debug tracks from SimpleSliceTrackConfig
 * Static tracks cannot be added on command
 * TODO: b/349502258 - To be removed later
 *
 * @param {PluginContextTrace} ctx Context for trace methods and properties
 * @param {SimpleSliceTrackConfig} config Track config to add
 * @param {string} trackName Track name to display
 */
export function addDebugTrackOnCommand(
  ctx: PluginContextTrace,
  config: SimpleSliceTrackConfig,
  trackName: string,
) {
  addDebugSliceTrack(
    ctx,
    config.data,
    trackName,
    config.columns,
    config.argColumns,
  );
}

/**
 * Registers and pins tracks on traceload or command
 *
 * @param {PluginContextTrace} ctx Context for trace methods and properties
 * @param {SimpleSliceTrackConfig} config Track config to add
 * @param {string} trackName Track name to display
 * type 'static' expects caller to pass uri string
 */
export function addAndPinSliceTrack(
  ctx: PluginContextTrace,
  config: SimpleSliceTrackConfig,
  trackName: string,
) {
  addDebugTrackOnCommand(ctx, config, trackName);
}

/**
 * Interface for slice identifier
 */
export interface SliceIdentifier {
  sliceId?: number;
  trackId?: number;
  ts?: time;
  dur?: bigint;
}

/**
 * Sets focus on a specific slice within the trace data.
 *
 * Takes and adds desired slice to current selection
 * Retrieves the track key and scrolls to the desired slice
 *
 * @param {SliceIdentifier} slice slice to focus on with trackId and sliceId
 */

export function focusOnSlice(slice: SliceIdentifier) {
  if (slice.sliceId == undefined || slice.trackId == undefined) {
    return;
  }
  const trackId = slice.trackId;
  const trackKey = getTrackKey(trackId);
  globals.setLegacySelection(
    {
      kind: 'SLICE',
      id: slice.sliceId,
      trackKey: trackKey,
      table: 'slice',
    },
    {
      clearSearch: true,
      pendingScrollId: slice.sliceId,
      switchToCurrentSelectionTab: true,
    },
  );
  findCurrentSelection;
}

/**
 * Given the trackId of the track, retrieves its trackKey
 *
 * @param {number} trackId track_id of the track
 * @returns {string} trackKey given to the track with queried trackId
 */
function getTrackKey(trackId: number): string | undefined {
  return globals.trackManager.trackKeyByTrackId.get(trackId);
}

/**
 * Sets focus on a specific time span and a track
 *
 * Takes a row object pans the view to that time span
 * Retrieves the track key and scrolls to the desired track
 *
 * @param {SliceIdentifier} slice slice to focus on with trackId and time data
 */

export async function focusOnTimeAndTrack(slice: SliceIdentifier) {
  if (
    slice.trackId == undefined ||
    slice.ts == undefined ||
    slice.dur == undefined
  ) {
    return;
  }
  const trackId = slice.trackId;
  const sliceStart = slice.ts;
  // row.dur can be negative. Clamp to 1ns.
  const sliceDur = BigintMath.max(slice.dur, 1n);
  const trackKey = getTrackKey(trackId);
  // true for whether to expand the process group the track belongs to
  if (trackKey == undefined) {
    return;
  }
  reveal(trackKey, sliceStart, Time.add(sliceStart, sliceDur), true);
}

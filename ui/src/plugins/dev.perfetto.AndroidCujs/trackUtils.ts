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
import {
  SimpleSliceTrack,
  SimpleSliceTrackConfig,
} from '../../frontend/simple_slice_track';
import {addDebugSliceTrack, PluginContextTrace} from '../../public';
import {findCurrentSelection} from '../../frontend/keyboard_event_handler';
import {time, Time} from '../../base/time';
import {BigintMath} from '../../base/bigint_math';
import {reveal} from '../../frontend/scroll_helper';

// Common TrackType for tracks when using registerStatic or addDebug
// TODO: b/349502258 - to be removed after single refactoring to single API
export type TrackType = 'static' | 'debug';

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
 * Registers and pins tracks on traceload, given params
 * TODO: b/349502258 - Refactor to single API
 *
 * @param {PluginContextTrace} ctx Context for trace methods and properties
 * @param {SimpleSliceTrackConfig} config Track config to add
 * @param {string} trackName Track name to display
 * @param {string} uri Unique identifier for the track
 */
export function addDebugTrackOnTraceLoad(
  ctx: PluginContextTrace,
  config: SimpleSliceTrackConfig,
  trackName: string,
  uri: string,
) {
  ctx.registerStaticTrack({
    uri: uri,
    title: trackName,
    isPinned: true,
    trackFactory: (trackCtx) => {
      return new SimpleSliceTrack(ctx.engine, trackCtx, config);
    },
  });
}

/**
 * Registers and pins tracks on traceload or command
 * Every enabled plugins' onTraceload is executed when the trace is first loaded
 * To add and pin tracks on traceload, need to use registerStaticTrack
 * After traceload, if plugin registered command invocated, then addDebugSliceTrack
 * TODO: b/349502258 - Refactor to single API
 *
 * @param {PluginContextTrace} ctx Context for trace methods and properties
 * @param {SimpleSliceTrackConfig} config Track config to add
 * @param {string} trackName Track name to display
 * @param {TrackType} type Whether to registerStaticTrack or addDebugSliceTrack
 * type 'static' expects caller to pass uri string
 * @param {string} uri Unique track identifier expected when type is 'static'
 */
export function addAndPinSliceTrack(
  ctx: PluginContextTrace,
  config: SimpleSliceTrackConfig,
  trackName: string,
  type: TrackType,
  uri?: string,
) {
  if (type == 'static') {
    addDebugTrackOnTraceLoad(ctx, config, trackName, uri ?? '');
  } else if (type == 'debug') {
    addDebugTrackOnCommand(ctx, config, trackName);
  }
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

export async function focusOnSlice(slice: SliceIdentifier) {
  if (slice.sliceId == undefined || slice.trackId == undefined) {
    return;
  }
  const trackId = slice.trackId;
  const trackKey = await getTrackKey(trackId);
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
async function getTrackKey(trackId: number): Promise<string | undefined> {
  // TODO: b/353466921 - update when waiForTraceLoad function added
  // waitForValue used to return trackKey when available
  // as we need to wait for the trace to load first.
  const trackKey = await waitForValue(() =>
    globals.trackManager.trackKeyByTrackId.get(trackId),
  );
  return trackKey;
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
  const trackKey = await getTrackKey(trackId);
  // true for whether to expand the process group the track belongs to
  if (trackKey == undefined) {
    return;
  }
  reveal(trackKey, sliceStart, Time.add(sliceStart, sliceDur), true);
}

/**
 * Function to check keep checking for object values at set intervals
 *
 * @param {T | undefined} getValue Function to retrieve object value
 * @returns {T} Value returned by getValue when available
 */
export async function waitForValue<T>(getValue: () => T): Promise<T> {
  while (true) {
    // TODO: b/353466921 - update when waiForTraceLoad function added
    const value = getValue();
    if (value !== undefined) {
      return value;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
}

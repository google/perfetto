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

import {SimpleSliceTrackConfig} from '../../frontend/simple_slice_track';
import {addDebugSliceTrack} from '../../public/debug_tracks';
import {Trace} from '../../public/trace';

/**
 * Adds debug tracks from SimpleSliceTrackConfig
 * Static tracks cannot be added on command
 * TODO: b/349502258 - To be removed later
 *
 * @param {Trace} ctx Context for trace methods and properties
 * @param {SimpleSliceTrackConfig} config Track config to add
 * @param {string} trackName Track name to display
 */
export function addDebugTrackOnCommand(
  ctx: Trace,
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
 * @param {Trace} ctx Context for trace methods and properties
 * @param {SimpleSliceTrackConfig} config Track config to add
 * @param {string} trackName Track name to display
 * type 'static' expects caller to pass uri string
 */
export function addAndPinSliceTrack(
  ctx: Trace,
  config: SimpleSliceTrackConfig,
  trackName: string,
) {
  addDebugTrackOnCommand(ctx, config, trackName);
}

/**
 * Sets focus on a specific slice within the trace data.
 *
 * Takes and adds desired slice to current selection
 * Retrieves the track key and scrolls to the desired slice
 */
export function focusOnSlice(ctx: Trace, sqlSliceId: number) {
  ctx.selection.selectSqlEvent('slice', sqlSliceId, {
    scrollToSelection: true,
  });
}

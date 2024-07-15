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

import {
  SimpleSliceTrack,
  SimpleSliceTrackConfig,
} from '../../frontend/simple_slice_track';
import {addDebugSliceTrack, PluginContextTrace} from '../../public';

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

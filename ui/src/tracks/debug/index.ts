// Copyright (C) 2023 The Android Open Source Project
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
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {SLICE_TRACK_KIND} from '../chrome_slices';

import {CounterDebugTrackConfig, DebugCounterTrack} from './counter_track';
import {DebugTrackV2, DebugTrackV2Config} from './slice_track';

export const DEBUG_SLICE_TRACK_URI = 'perfetto.DebugSlices';
export const DEBUG_COUNTER_TRACK_URI = 'perfetto.DebugCounter';

class DebugTrackPlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    // Add debug slice track
    ctx.addTrack({
      displayName: '',
      kind: SLICE_TRACK_KIND,
      uri: DEBUG_SLICE_TRACK_URI,
      track: (trackCtx) => {
        const store = trackCtx.mountStore((init) => init as DebugTrackV2Config);
        const track = new DebugTrackV2({
          engine: ctx.engine,
          trackId: trackCtx.trackInstanceId,
        });
        track.config = store.state;
        return track;
      },
    });

    // Add debug counter track
    ctx.addTrack({
      displayName: '',
      kind: SLICE_TRACK_KIND,
      uri: DEBUG_COUNTER_TRACK_URI,
      track: (trackCtx) => {
        const store =
            trackCtx.mountStore((init) => init as CounterDebugTrackConfig);
        const track = new DebugCounterTrack({
          engine: ctx.engine,
          trackId: trackCtx.trackInstanceId,
        });
        track.config = store.state;
        return track;
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.DebugSlices',
  plugin: DebugTrackPlugin,
};

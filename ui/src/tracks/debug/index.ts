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

import {uuidv4} from '../../base/uuid';
import {DEBUG_COUNTER_TRACK_URI, DEBUG_SLICE_TRACK_URI} from '../../frontend/debug_tracks';
import {
  BottomTabToSCSAdapter,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';

import {DebugCounterTrack} from './counter_track';
import {DebugSliceDetailsTab} from './details_tab';
import {DebugTrackV2} from './slice_track';
import {GenericSliceDetailsTabConfig} from '../../frontend/generic_slice_details_tab';

class DebugTrackPlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerTrack({
      uri: DEBUG_SLICE_TRACK_URI,
      trackFactory: (trackCtx) => new DebugTrackV2(ctx.engine, trackCtx),
    });

    ctx.registerDetailsPanel(new BottomTabToSCSAdapter({
      tabFactory: (selection) => {
        if (selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind === DebugSliceDetailsTab.kind) {
          const config = selection.detailsPanelConfig.config;
          return new DebugSliceDetailsTab({
            config: config as GenericSliceDetailsTabConfig,
            engine: ctx.engine,
            uuid: uuidv4(),
          });
        }
        return undefined;
      },
    }));

    ctx.registerTrack({
      uri: DEBUG_COUNTER_TRACK_URI,
      trackFactory: (trackCtx) => new DebugCounterTrack(ctx.engine, trackCtx),
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.DebugSlices',
  plugin: DebugTrackPlugin,
};

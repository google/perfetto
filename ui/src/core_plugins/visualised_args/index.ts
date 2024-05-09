// Copyright (C) 2022 The Android Open Source Project
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

import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {
  VISUALISED_ARGS_SLICE_TRACK_URI,
  VisualisedArgsState,
} from '../../frontend/visualized_args_tracks';
import {VisualisedArgsTrack} from './visualized_args_track';

class VisualisedArgsPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerTrack({
      uri: VISUALISED_ARGS_SLICE_TRACK_URI,
      tags: {
        metric: true, // TODO(stevegolton): Is this track really a metric?
      },
      trackFactory: (trackCtx) => {
        // TODO(stevegolton): Validate params properly. Note, this is no
        // worse than the situation we had before with track config.
        const params = trackCtx.params as VisualisedArgsState;
        return new VisualisedArgsTrack(
          {
            engine: ctx.engine,
            trackKey: trackCtx.trackKey,
          },
          params.trackId,
          params.maxDepth,
          params.argName,
        );
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.VisualisedArgs',
  plugin: VisualisedArgsPlugin,
};

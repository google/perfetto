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
  PluginDescriptor,
} from '../../public';

class LargeScreensPerf implements Plugin {
  onActivate(ctx: PluginContext): void {
    ctx.addCommand({
      id: 'dev.perfetto.LargeScreensPerf#PinUnfoldLatencyTracks',
      name: 'Pin: Unfold latency tracks',
      callback: () => {
        ctx.viewer.tracks.pin((tags) => {
          return !!tags.name?.includes('UNFOLD') ||
              tags.name?.includes('Screen on blocked') ||
              tags.name?.startsWith('waitForAllWindowsDrawn') ||
              tags.name?.endsWith('FoldUnfoldTransitionInProgress') ||
              tags.name == 'Waiting for KeyguardDrawnCallback#onDrawn';
        });
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.LargeScreensPerf',
  plugin: LargeScreensPerf,
};

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

import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';

class LargeScreensPerf implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.LargeScreensPerf#PinUnfoldLatencyTracks',
      name: 'Pin: Unfold latency tracks',
      callback: () => {
        ctx.workspace.flatTracks.forEach((track) => {
          if (
            !!track.displayName.includes('UnfoldTransition') ||
            track.displayName.includes('Screen on blocked') ||
            track.displayName.includes('hingeAngle') ||
            track.displayName.includes('UnfoldLightRevealOverlayAnimation') ||
            track.displayName.startsWith('waitForAllWindowsDrawn') ||
            track.displayName.endsWith('UNFOLD_ANIM>') ||
            track.displayName.endsWith('UNFOLD>') ||
            track.displayName == 'Waiting for KeyguardDrawnCallback#onDrawn' ||
            track.displayName == 'FoldedState' ||
            track.displayName == 'FoldUpdate'
          ) {
            track.pin();
          }
        });
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.LargeScreensPerf',
  plugin: LargeScreensPerf,
};

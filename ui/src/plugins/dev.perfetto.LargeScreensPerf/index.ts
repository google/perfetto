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
import {PerfettoPlugin} from '../../public/plugin';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.LargeScreensPerf';
  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.LargeScreensPerf#PinUnfoldLatencyTracks',
      name: 'Pin: Unfold latency tracks',
      callback: () => {
        ctx.workspace.flatTracks.forEach((track) => {
          if (
            !!track.name.includes('UnfoldTransition') ||
            track.name.includes('Screen on blocked') ||
            track.name.includes('hingeAngle') ||
            track.name.includes('UnfoldLightRevealOverlayAnimation') ||
            track.name.startsWith('waitForAllWindowsDrawn') ||
            track.name.endsWith('UNFOLD_ANIM>') ||
            track.name.endsWith('UNFOLD>') ||
            track.name == 'Waiting for KeyguardDrawnCallback#onDrawn' ||
            track.name == 'FoldedState' ||
            track.name == 'FoldUpdate'
          ) {
            track.pin();
          }
        });
      },
    });
  }
}

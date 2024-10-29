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
            !!track.title.includes('UnfoldTransition') ||
            track.title.includes('Screen on blocked') ||
            track.title.includes('hingeAngle') ||
            track.title.includes('UnfoldLightRevealOverlayAnimation') ||
            track.title.startsWith('waitForAllWindowsDrawn') ||
            track.title.endsWith('UNFOLD_ANIM>') ||
            track.title.endsWith('UNFOLD>') ||
            track.title == 'Waiting for KeyguardDrawnCallback#onDrawn' ||
            track.title == 'FoldedState' ||
            track.title == 'FoldUpdate'
          ) {
            track.pin();
          }
        });
      },
    });
  }
}

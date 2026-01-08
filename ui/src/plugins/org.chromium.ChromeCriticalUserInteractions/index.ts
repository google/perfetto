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
import {TrackNode} from '../../public/workspace';
import {createCriticalUserInteractionTrack} from './critical_user_interaction_track';

export default class implements PerfettoPlugin {
  static readonly id = 'org.chromium.CriticalUserInteraction';
  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query('include perfetto module chrome.interactions;');

    const uri = `/critical_user_interactions`;

    ctx.commands.registerCommand({
      id: 'org.chromium.CriticalUserInteraction.AddInteractionTrack',
      name: 'Add track: Chrome interactions',
      callback: () => {
        const track = new TrackNode({
          uri,
          name: 'Chrome Interactions',
        });
        ctx.currentWorkspace.addChildInOrder(track);
        track.pin();
      },
    });

    ctx.tracks.registerTrack({
      uri,
      renderer: createCriticalUserInteractionTrack(ctx, uri),
    });
  }
}

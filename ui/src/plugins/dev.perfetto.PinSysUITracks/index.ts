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

import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';

// List of tracks to pin
const TRACKS_TO_PIN: string[] = [
  'Actual Timeline',
  'Expected Timeline',
  'ndroid.systemui',
  'IKeyguardService',
  'Transition:',
  'L<',
  'UI Events',
];
const SYSTEM_UI_PROCESS: string = 'com.android.systemui';

// Plugin that pins the tracks relevant to System UI
class PinSysUITracks implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerCommand({
      id: 'dev.perfetto.PinSysUITracks#PinSysUITracks',
      name: 'Pin: System UI Related Tracks',
      callback: () => {
        ctx.timeline.pinTracksByPredicate((tags) => {
          return !!(
            TRACKS_TO_PIN.some((trackName) =>
              tags.name?.startsWith(trackName),
            ) && tags.groupName?.startsWith(SYSTEM_UI_PROCESS)
          );
        });

        // expand the sysui process tracks group
        ctx.timeline.expandGroupsByPredicate((groupRef) => {
          return groupRef.displayName?.startsWith(SYSTEM_UI_PROCESS) ?? false;
        });
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.PinSysUITracks',
  plugin: PinSysUITracks,
};

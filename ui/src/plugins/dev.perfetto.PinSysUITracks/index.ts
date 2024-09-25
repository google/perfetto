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

import {NUM} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';

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
class PinSysUITracks implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    // Find the upid for the sysui process
    const result = await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.process_metadata;
      select
        _process_available_info_summary.upid
      from _process_available_info_summary
      join process using(upid)
      where process.name = 'com.android.systemui';
    `);
    if (result.numRows() === 0) {
      return;
    }
    const sysuiUpid = result.firstRow({
      upid: NUM,
    }).upid;

    ctx.commands.registerCommand({
      id: 'dev.perfetto.PinSysUITracks#PinSysUITracks',
      name: 'Pin: System UI Related Tracks',
      callback: () => {
        ctx.workspace.flatTracks.forEach((track) => {
          if (!track.uri) return;
          // Ensure we only grab tracks that are in the SysUI process group
          if (!track.uri.startsWith(`/process_${sysuiUpid}`)) return;
          if (
            !TRACKS_TO_PIN.some((trackName) =>
              track.title.startsWith(trackName),
            )
          ) {
            return;
          }
          track.pin();
        });

        // expand the sysui process tracks group
        ctx.workspace.flatTracks.forEach((track) => {
          if (track.hasChildren && track.title.startsWith(SYSTEM_UI_PROCESS)) {
            track.expand();
          }
        });
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.PinSysUITracks',
  plugin: PinSysUITracks,
};

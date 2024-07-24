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
import {
  addDebugCounterTrack,
  addDebugSliceTrack,
} from '../../frontend/debug_tracks/debug_tracks';
import {
  BottomTabToSCSAdapter,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';

import {DebugSliceDetailsTab} from '../../frontend/debug_tracks/details_tab';
import {GenericSliceDetailsTabConfig} from '../../frontend/generic_slice_details_tab';
import {Optional, exists} from '../../base/utils';

class DebugTracksPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerCommand({
      id: 'perfetto.DebugTracks#addDebugSliceTrack',
      name: 'Add debug slice track',
      callback: async (arg: unknown) => {
        // This command takes a query and creates a debug track out of it The
        // query can be passed in using the first arg, or if this is not defined
        // or is the wrong type, we prompt the user for it.
        const query = await getStringFromArgOrPrompt(ctx, arg);
        if (exists(query)) {
          await addDebugSliceTrack(
            ctx,
            {
              sqlSource: query,
            },
            'Debug slice track',
            {ts: 'ts', dur: 'dur', name: 'name'},
            [],
          );
        }
      },
    });

    ctx.registerCommand({
      id: 'perfetto.DebugTracks#addDebugCounterTrack',
      name: 'Add debug counter track',
      callback: async (arg: unknown) => {
        const query = await getStringFromArgOrPrompt(ctx, arg);
        if (exists(query)) {
          await addDebugCounterTrack(
            ctx,
            {
              sqlSource: query,
            },
            'Debug slice track',
            {ts: 'ts', value: 'value'},
          );
        }
      },
    });

    // TODO(stevegolton): While debug tracks are in their current state, we rely
    // on this plugin to provide the details panel for them. In the future, this
    // details panel will become part of the debug track's definition.
    ctx.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (selection) => {
          if (
            selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind === DebugSliceDetailsTab.kind
          ) {
            const config = selection.detailsPanelConfig.config;
            return new DebugSliceDetailsTab({
              config: config as GenericSliceDetailsTabConfig,
              engine: ctx.engine,
              uuid: uuidv4(),
            });
          }
          return undefined;
        },
      }),
    );
  }
}

// If arg is a string, return it, otherwise prompt the user for a string. An
// exception is thrown if the prompt is cancelled, so this function handles this
// and returns undefined in this case.
async function getStringFromArgOrPrompt(
  ctx: PluginContextTrace,
  arg: unknown,
): Promise<Optional<string>> {
  if (typeof arg === 'string') {
    return arg;
  } else {
    try {
      return await ctx.prompt('Enter a query...');
    } catch {
      // Prompt was ignored
      return undefined;
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.DebugTracks',
  plugin: DebugTracksPlugin,
};

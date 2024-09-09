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

import {
  getTimeSpanOfSelectionOrVisibleWindow,
  globals,
} from '../../frontend/globals';
import {OmniboxMode} from '../../core/omnibox_manager';
import {Trace} from '../../public/trace';
import {PromptOption} from '../../public/omnibox';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';

class TrackUtilsPlugin implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'perfetto.RunQueryInSelectedTimeWindow',
      name: `Run query in selected time window`,
      callback: async () => {
        const window = await getTimeSpanOfSelectionOrVisibleWindow();
        globals.omnibox.setMode(OmniboxMode.Query);
        globals.omnibox.setText(
          `select  where ts >= ${window.start} and ts < ${window.end}`,
        );
        globals.omnibox.focus(7);
      },
    });

    ctx.commands.registerCommand({
      // Selects & reveals the first track on the timeline with a given URI.
      id: 'perfetto.FindTrack',
      name: 'Find track by URI',
      callback: async () => {
        const tracks = globals.trackManager.getAllTracks();
        const options = tracks.map(({uri}): PromptOption => {
          return {key: uri, displayName: uri};
        });

        // Sort tracks in a natural sort order
        const collator = new Intl.Collator('en', {
          numeric: true,
          sensitivity: 'base',
        });
        const sortedOptions = options.sort((a, b) => {
          return collator.compare(a.displayName, b.displayName);
        });

        const selectedUri = await ctx.omnibox.prompt(
          'Choose a track...',
          sortedOptions,
        );
        if (selectedUri === undefined) return; // Prompt cancelled.
        ctx.scrollTo({track: {uri: selectedUri, expandGroup: true}});
        const traceTime = globals.traceContext;
        globals.selectionManager.setArea({
          start: traceTime.start,
          end: traceTime.end,
          trackUris: [selectedUri],
        });
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.TrackUtils',
  plugin: TrackUtilsPlugin,
};

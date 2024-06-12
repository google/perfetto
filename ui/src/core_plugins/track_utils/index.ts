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

import {Actions} from '../../common/actions';
import {
  getTimeSpanOfSelectionOrVisibleWindow,
  globals,
} from '../../frontend/globals';
import {OmniboxMode} from '../../frontend/omnibox_manager';
import {verticalScrollToTrack} from '../../frontend/scroll_helper';
import {
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
  PromptOption,
} from '../../public';

class TrackUtilsPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerCommand({
      id: 'perfetto.RunQueryInSelectedTimeWindow',
      name: `Run query in selected time window`,
      callback: async () => {
        const window = await getTimeSpanOfSelectionOrVisibleWindow();
        globals.omnibox.setMode(OmniboxMode.Query);
        globals.omnibox.setText(
          `select  where ts >= ${window.start} and ts < ${window.end}`,
        );
        globals.omnibox.focusOmnibox(7);
      },
    });

    ctx.registerCommand({
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

        try {
          const selectedUri = await ctx.prompt(
            'Choose a track...',
            sortedOptions,
          );

          // Find the first track with this URI
          const firstTrack = Object.values(globals.state.tracks).find(
            ({uri}) => uri === selectedUri,
          );
          if (firstTrack) {
            console.log(firstTrack);
            verticalScrollToTrack(firstTrack.key, true);
            const traceTime = globals.stateTraceTimeTP();
            globals.makeSelection(
              Actions.selectArea({
                start: traceTime.start,
                end: traceTime.end,
                tracks: [firstTrack.key],
              }),
            );
          } else {
            alert(`No tracks with uri ${selectedUri} on the timeline`);
          }
        } catch {
          // Prompt was probably cancelled - do nothing.
        }
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.TrackUtils',
  plugin: TrackUtilsPlugin,
};

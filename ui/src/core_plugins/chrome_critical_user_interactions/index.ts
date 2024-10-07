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
import {PageLoadDetailsPanel} from './page_load_details_panel';
import {StartupDetailsPanel} from './startup_details_panel';
import {WebContentInteractionPanel} from './web_content_interaction_details_panel';
import {CriticalUserInteractionTrack} from './critical_user_interaction_track';
import {TrackNode} from '../../public/workspace';
import {TrackEventSelection} from '../../public/selection';
import {GenericSliceDetailsTab} from '../../frontend/generic_slice_details_tab';

class CriticalUserInteractionPlugin implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'perfetto.CriticalUserInteraction.AddInteractionTrack',
      name: 'Add track: Chrome interactions',
      callback: () => {
        const track = new TrackNode({
          uri: CriticalUserInteractionTrack.kind,
          title: 'Chrome Interactions',
        });
        ctx.workspace.addChildInOrder(track);
        track.pin();
      },
    });

    ctx.tracks.registerTrack({
      uri: CriticalUserInteractionTrack.kind,
      tags: {
        kind: CriticalUserInteractionTrack.kind,
      },
      title: 'Chrome Interactions',
      track: new CriticalUserInteractionTrack({
        trace: ctx,
        uri: CriticalUserInteractionTrack.kind,
      }),
      detailsPanel: (sel: TrackEventSelection) => {
        switch (sel.interactionType) {
          case 'chrome_page_loads':
            return new PageLoadDetailsPanel(ctx, sel.eventId);
          case 'chrome_startups':
            return new StartupDetailsPanel(ctx, sel.eventId);
          case 'chrome_web_content_interactions':
            return new WebContentInteractionPanel(ctx, sel.eventId);
          default:
            return new GenericSliceDetailsTab(
              ctx,
              'chrome_interactions',
              sel.eventId,
              'Chrome Interaction',
            );
        }
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CriticalUserInteraction',
  plugin: CriticalUserInteractionPlugin,
};

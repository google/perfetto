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

import {v4 as uuidv4} from 'uuid';

import {Actions} from '../../common/actions';
import {SCROLLING_TRACK_GROUP} from '../../common/state';
import {GenericSliceDetailsTabConfig} from '../../frontend/generic_slice_details_tab';
import {globals} from '../../frontend/globals';
import {
  BottomTabToSCSAdapter,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  PrimaryTrackSortKey,
} from '../../public';

import {PageLoadDetailsPanel} from './page_load_details_panel';
import {StartupDetailsPanel} from './startup_details_panel';
import {WebContentInteractionPanel} from './web_content_interaction_details_panel';
import {CriticalUserInteractionTrack} from './critical_user_interaction_track';

function addCriticalUserInteractionTrack() {
  const trackKey = uuidv4();
  globals.dispatchMultiple([
    Actions.addTrack({
      key: trackKey,
      uri: CriticalUserInteractionTrack.kind,
      name: `Chrome Interactions`,
      trackSortKey: PrimaryTrackSortKey.DEBUG_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
    }),
    Actions.toggleTrackPinned({trackKey}),
  ]);
}

class CriticalUserInteractionPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerTrack({
      uri: CriticalUserInteractionTrack.kind,
      kind: CriticalUserInteractionTrack.kind,
      displayName: 'Chrome Interactions',
      trackFactory: (trackCtx) =>
        new CriticalUserInteractionTrack({
          engine: ctx.engine,
          trackKey: trackCtx.trackKey,
        }),
    });

    ctx.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (selection) => {
          if (
            selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind === PageLoadDetailsPanel.kind
          ) {
            const config = selection.detailsPanelConfig.config;
            return new PageLoadDetailsPanel({
              config: config as GenericSliceDetailsTabConfig,
              engine: ctx.engine,
              uuid: uuidv4(),
            });
          }
          return undefined;
        },
      }),
    );

    ctx.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (selection) => {
          if (
            selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind === StartupDetailsPanel.kind
          ) {
            const config = selection.detailsPanelConfig.config;
            return new StartupDetailsPanel({
              config: config as GenericSliceDetailsTabConfig,
              engine: ctx.engine,
              uuid: uuidv4(),
            });
          }
          return undefined;
        },
      }),
    );

    ctx.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (selection) => {
          if (
            selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind ===
              WebContentInteractionPanel.kind
          ) {
            const config = selection.detailsPanelConfig.config;
            return new WebContentInteractionPanel({
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

  onActivate(ctx: PluginContext): void {
    ctx.registerCommand({
      id: 'perfetto.CriticalUserInteraction.AddInteractionTrack',
      name: 'Add track: Chrome interactions',
      callback: () => addCriticalUserInteractionTrack(),
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CriticalUserInteraction',
  plugin: CriticalUserInteractionPlugin,
};

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

import {featureFlags} from '../../common/feature_flags';
import {PluginContext} from '../../common/plugin_api';
import {Selection} from '../../common/state';
import {CURRENT_SELECTION_TAG} from '../../frontend/details_panel';
import {globals} from '../../frontend/globals';

import {EventLatencyTrack} from './event_latency_track';
import {TopLevelScrollDetailsTab} from './scroll_details_tab';
import {
  TOP_LEVEL_SCROLL_KIND,
  TopLevelScrollTrack,
} from './scroll_track';

export const INPUT_LATENCY_TRACK = 'InputLatency::';
export const SCROLL_JANK_PLUGIN_ID = 'perfetto.ScrollJank';
export const ENABLE_SCROLL_JANK_PLUGIN_V2 = featureFlags.register({
  id: 'enableScrollJankPluginV2',
  name: 'Enable Scroll Jank plugin V2',
  description: 'Adds new tracks and visualizations for scroll jank.',
  defaultValue: false,
});

function onDetailsPanelSelectionChange(newSelection?: Selection) {
  if (newSelection === undefined ||
      newSelection.kind !== TOP_LEVEL_SCROLL_KIND) {
    return;
  }
  const bottomTabList = globals.bottomTabList;
  if (!bottomTabList) return;
  bottomTabList.addTab({
    kind: TopLevelScrollDetailsTab.kind,
    tag: CURRENT_SELECTION_TAG,
    config: {
      sqlTableName: newSelection.sqlTableName,
      id: newSelection.id,
    },
  });
}

function activate(ctx: PluginContext) {
  ctx.registerTrack(TopLevelScrollTrack);
  ctx.registerTrack(EventLatencyTrack);
  ctx.registerOnDetailsPanelSelectionChange(onDetailsPanelSelectionChange);
}

export const plugin = {
  pluginId: SCROLL_JANK_PLUGIN_ID,
  activate,
};

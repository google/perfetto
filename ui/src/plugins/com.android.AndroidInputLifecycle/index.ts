// Copyright (C) 2026 The Android Open Source Project
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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {Dataset} from '../../trace_processor/dataset';
import RelatedEvents, {RelatedEvent} from '../dev.perfetto.RelatedEvents';
import {RuleBasedBfsStrategy} from '../dev.perfetto.RelatedEvents/relation_finding_strategies/bfs_relation_finding_strategy';
import {ANDROID_INPUT_RULES} from './android_input_dataset_rules';
import {ChannelRelationOverlay} from './channel_relation_overlay';
import {RelatedInputEventsTab} from './related_input_events_tab';

export type RelatedEventWithChannel = RelatedEvent & {
  channel: string;
  depth: number;
};
export interface OverlayUpdater {
  updateOverlayData(active: boolean, channel?: string, dataset?: Dataset): void;
}

export default class AndroidInputLifecycle
  implements PerfettoPlugin, OverlayUpdater
{
  static readonly id = 'com.android.AndroidInputLifecycle';
  static readonly dependencies = [RelatedEvents];

  private overlayInstance: ChannelRelationOverlay;

  constructor(trace: Trace) {
    this.overlayInstance = new ChannelRelationOverlay(trace);
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    this.overlayInstance = new ChannelRelationOverlay(trace);
    trace.tracks.registerOverlay(this.overlayInstance);

    const relatedEventsPlugin = trace.plugins.getPlugin(RelatedEvents);

    trace.commands.registerCommand({
      id: 'com.android.ViewRelatedSlices',
      name: 'Android: View related slices',
      callback: () => {
        const relatedEventsPromise = relatedEventsPlugin.getRelatedEvents(
          new RuleBasedBfsStrategy(ANDROID_INPUT_RULES),
        );

        const tab = new RelatedInputEventsTab(
          trace,
          relatedEventsPromise,
          this,
        );
        const tabUri = `${AndroidInputLifecycle.id}#${tab.getTitle()}`;
        trace.tabs.registerTab({
          isEphemeral: true,
          uri: tabUri,
          content: tab,
        });
        trace.tabs.showTab(tabUri);
        relatedEventsPlugin.setCurrentTab(tab);
      },
      defaultHotkey: 'V',
    });
  }

  updateOverlayData(
    active: boolean,
    channel?: string,
    dataset?: Dataset,
  ): void {
    this.overlayInstance.updateOverlayData(active, channel, dataset);
  }
}

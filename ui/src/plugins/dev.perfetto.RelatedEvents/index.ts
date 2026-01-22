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

import {time} from '../../base/time';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {Dataset} from '../../trace_processor/dataset';
import {RelatedEventsTabBase} from './related_events_tab';
import {RelationFindingStrategy} from './relation_finding_strategy';

export interface RelatedEvent {
  id: number;
  name: string;
  ts: time;
  dur: time;
  track_id: number;
  relation?: string;
}

export default class RelatedEvents implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.RelatedEvents';

  private currentTab: RelatedEventsTabBase | undefined;

  constructor(private readonly trace: Trace) {}

  // TODO(ivankc) Add caching
  async getRelatedEvents(
    strategy: RelationFindingStrategy,
  ): Promise<Dataset | undefined> {
    const selection = this.trace.selection.selection;
    if (selection.kind !== 'track_event') return undefined;
    const initialSliceId = selection.eventId;
    if (initialSliceId === undefined) return undefined;

    const relatedEvents = await strategy.findRelatedEvents(this.trace);
    return relatedEvents;
  }

  async setCurrentTab(tab: RelatedEventsTabBase) {
    this.currentTab = tab;
  }

  async onTraceLoad(
    trace: Trace,
    _args: {[key: string]: unknown},
  ): Promise<void> {
    trace.commands.registerCommand({
      id: 'dev.perfetto.RelatedEvents#goToPreviousRelatedEvent',
      name: 'Perfetto: Go To previous related event',
      callback: async () => {
        this.currentTab?.jumpToPreviousEvent();
      },
      defaultHotkey: 'ArrowLeft',
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.RelatedEvents#goToNextRelatedEvent',
      name: 'Perfetto: Go To next related event',
      callback: async () => {
        this.currentTab?.jumpToNextEvent();
      },
      defaultHotkey: 'ArrowRight',
    });
  }
}

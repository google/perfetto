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

import {Dataset} from '../../trace_processor/dataset';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {MenuItem} from '../../widgets/menu';
import {RelatedEvent} from '.';
import {RELATION_SCHEMA} from './relation_finding_strategy';
import {Section} from '../../widgets/section';
import {SelectionOpts} from '../../public/selection';
import {Tab} from '../../public/tab';
import {Time} from '../../base/time';
import {Trace} from '../../public/trace';
import m, {Children} from 'mithril';

export abstract class RelatedEventsTabBase implements Tab {
  private static readonly title = 'Related Events';

  protected isLoading: boolean = true;
  protected relatedEvents: RelatedEvent[] = [];

  constructor(
    protected trace: Trace,
    protected datasetOrPromise: Dataset | Promise<Dataset | undefined>,
  ) {}

  protected get events(): RelatedEvent[] {
    return this.relatedEvents;
  }

  protected async loadData(
    datasetOrPromise: Dataset | Promise<Dataset | undefined>,
  ) {
    this.isLoading = true;
    let resolvedDataset: Dataset | undefined;

    try {
      if (datasetOrPromise instanceof Promise) {
        resolvedDataset = await datasetOrPromise;
      } else {
        resolvedDataset = datasetOrPromise;
      }
    } catch (e) {
      console.error('RelatedEventsTab: Dataset promise failed', e);
    }

    if (!resolvedDataset) {
      this.isLoading = false;
      return;
    }

    try {
      const query = resolvedDataset.query(RELATION_SCHEMA);
      const result = await this.trace.engine.query(query);
      const it = result.iter(RELATION_SCHEMA);

      this.relatedEvents = [];

      while (it.valid()) {
        this.relatedEvents.push({
          id: Number(it.id),
          name: (it.name as string) || 'Unknown Event',
          ts: Time.fromRaw(it.ts),
          dur: Time.fromRaw(it.dur),
          track_id: Number(it.track_id),
        });
        it.next();
      }

      this.relatedEvents.sort((a, b) => Number(a.ts - b.ts));
    } catch (e) {
      console.error('RelatedEventsTab: Failed to query dataset', e);
    } finally {
      this.isLoading = false;
      m.redraw();
    }
  }

  jumpToNextEvent() {
    const events = this.events;
    if (events.length === 0) return;

    const index = this.findIndexOfCurrentEventSelection(events);
    if (index === -1) return;

    const newIndex = (index + 1) % events.length;
    this.focusOnEvent(events[newIndex]);
  }

  jumpToPreviousEvent() {
    const events = this.events;
    if (events.length === 0) return;

    const index = this.findIndexOfCurrentEventSelection(events);
    if (index === -1) return;

    const length = events.length;
    // Modulo operator for negatives as JS '%' is the remainder not modulo
    const newIndex = (((index - 1) % length) + length) % length;
    this.focusOnEvent(events[newIndex]);
  }

  focusOnEvent(event: RelatedEvent) {
    const selectionOpts: SelectionOpts = {
      switchToCurrentSelectionTab: false,
      scrollToSelection: true,
    };
    // Prefer selecting by Track URI if possible, otherwise fall back to SQL ID
    const trackUri = `/slice_${event.track_id}`;
    this.trace.selection.selectTrackEvent(trackUri, event.id, selectionOpts);
  }

  protected findIndexOfCurrentEventSelection(events: RelatedEvent[]): number {
    const selection = this.trace.selection.selection;
    if (selection.kind !== 'track_event') return -1;
    return events.findIndex((e) => e.id === Number(selection.eventId));
  }

  render(): Children {
    const events = this.events;
    return m(
      DetailsShell,
      {
        title: this.getTitle(),
      },
      m(
        GridLayout,
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Events'},
            events.map((event) => {
              return m(MenuItem, {
                label: event.name,
                onclick: () => this.focusOnEvent(event),
                key: event.id,
              });
            }),
          ),
        ),
      ),
    );
  }

  getTitle(): string {
    return RelatedEventsTabBase.title;
  }
}

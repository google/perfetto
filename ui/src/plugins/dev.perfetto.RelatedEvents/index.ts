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

export * from './interface';
export {drawRelatedEvents, ArrowVisualiser} from './arrow_visualiser';
export * from './utils';

// This plugin provides a framework for visualizing relationships between events.
// Key exports for consumers:
// - Interfaces from './interface' (e.g., RelatedEventData, EventSource)
//   to structure your event relationship data.
// - 'drawRelatedEvents' from './arrow_visualiser' to render arrows on the timeline.
// - 'GenericRelatedEventsOverlay' from './generic_overlay' for a quick overlay implementation.
// - Utility functions from './utils' for common tasks like track URI lookups.
//
// Example usage:
//
// // 1. Implement EventSource
// class SimpleEventSource implements EventSource {
//   async getRelatedEventData(eventId: number): Promise<RelatedEventData> {
//     // Dummy data for illustration
//     const events: RelatedEvent[] = [
//       {id: 1, ts: 100n, dur: 10n, trackUri: 'track_a', type: 'A'},
//       {id: 2, ts: 120n, dur: 10n, trackUri: 'track_b', type: 'B'},
//     ];
//     const relations: Relation[] = [{sourceId: 1, targetId: 2, type: 'flow'}];
//     return { events, relations, overlayEvents: events, overlayRelations: relations };
//   }
// }
//
// // 2. In your plugin's onTraceLoad or a command callback:
// async function setupOverlay(trace: Trace) {
//   const overlay = new GenericRelatedEventsOverlay(trace);
//   trace.tracks.registerOverlay(overlay);
//   const source = new SimpleEventSource();
//
//   // Load data for a specific event ID and update the overlay
//   const data = await source.getRelatedEventData(someEventId);
//   overlay.update(data);
// }
//
// // 3. Example of using the data in a custom Tab
// class MyRelatedEventsTab implements Tab {
//   constructor(private trace: Trace, private source: SimpleEventSource) {}
//
//   getTitle() { return 'My Related Events'; }
//
//   async onDetailsPanelSelectionChange(selection: Selection) {
//     if (selection.kind === 'track_event') {
//       this.data = await this.source.getRelatedEventData(selection.eventId);
//     }
//   }
//
//   render(): m.Children {
//     if (!this.data) return m('div', 'Select an event');
//     // Use this.data.events and this.data.relations to render a table or list
//     return m('div', `${this.data.events.length} related events found.`);
//   }
// }
//
// // In onTraceLoad:
// // const tab = new MyRelatedEventsTab(trace, source);
// // trace.tabs.registerTab({ uri: 'my.related.tab', content: tab });

export default class RelatedEventsPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.RelatedEvents';
}

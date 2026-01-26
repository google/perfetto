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

import m from 'mithril';
import {Anchor} from '../../widgets/anchor';
import {Checkbox} from '../../widgets/checkbox';
import {Grid, GridHeaderCell, GridCell} from '../../widgets/grid';
import {Icons} from '../../base/semantic_icons';
import {Timestamp} from '../../components/widgets/timestamp';
import {Tree, TreeNode} from '../../widgets/tree';
import {DetailsShell} from '../../widgets/details_shell';
import {Duration, Time} from '../../base/time';
import {GridLayoutColumn, GridLayout} from '../../widgets/grid_layout';
import {OverlayUpdater, RelatedEventWithChannel} from '.';
import {Section} from '../../widgets/section';
import {Trace} from '../../public/trace';
import {Dataset, SourceDataset} from '../../trace_processor/dataset';
import {RelatedEventsTabBase} from '../dev.perfetto.RelatedEvents/related_events_tab';
import {SelectionOpts} from '../../public/selection';
import {RelatedEvent} from '../dev.perfetto.RelatedEvents';
import {RELATION_SCHEMA} from '../dev.perfetto.RelatedEvents/relation_finding_strategy';
export class RelatedInputEventsTab extends RelatedEventsTabBase {
  private lifecycles = new Map<string, RelatedEventWithChannel[]>();
  private channelTrackIds = new Map<string, Set<number>>();
  private selectedChannel: string | undefined;
  private pinnedChannels = new Set<string>();
  private originalDataset: Dataset | undefined;
  private dataset: Dataset | undefined;

  constructor(
    trace: Trace,
    datasetOrPromise: Dataset | Promise<Dataset | undefined>,
    private overlayUpdater: OverlayUpdater,
  ) {
    super(trace, datasetOrPromise);
    this.loadData(datasetOrPromise);
  }

  protected get events(): RelatedEvent[] {
    if (this.selectedChannel && this.lifecycles.has(this.selectedChannel)) {
      return this.lifecycles.get(this.selectedChannel) || [];
    }
    return [];
  }

  override async loadData(
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
      console.error('RelatedInputEventsTab: Dataset promise failed', e);
    }

    if (!resolvedDataset) {
      this.isLoading = false;
      return;
    }

    this.dataset = resolvedDataset;
    this.originalDataset = resolvedDataset;

    if (!this.dataset.implements(RELATION_SCHEMA)) {
      this.isLoading = false;
      return;
    }

    try {
      const query = this.dataset.query(RELATION_SCHEMA);
      const result = await this.trace.engine.query(query);
      const it = result.iter(RELATION_SCHEMA);

      const allEvents: RelatedEventWithChannel[] = [];

      while (it.valid()) {
        const name = (it.name as string) || 'Unknown Event';

        allEvents.push({
          id: Number(it.id),
          name: name,
          ts: Time.fromRaw(it.ts),
          track_id: Number(it.track_id ?? 0),
          channel: this.parseChannelFromEventName(name),
          dur: Time.fromRaw(it.dur),
          depth: Number(it.depth),
        });

        it.next();
      }

      this.groupEvents(allEvents);

      this.isLoading = false;
      this.updateOverlay();
    } catch (e) {
      this.isLoading = false;
    }
  }

  private groupEvents(events: RelatedEventWithChannel[]) {
    this.lifecycles.clear();
    this.channelTrackIds.clear();

    const specificLifecycles = new Map<string, RelatedEventWithChannel[]>();
    const commonEvents: RelatedEventWithChannel[] = [];

    // Partition events into channels if possible
    for (const event of events) {
      if (event.channel === 'Uncatgorised') {
        commonEvents.push(event);
      } else {
        if (!specificLifecycles.has(event.channel)) {
          specificLifecycles.set(event.channel, []);
        }
        specificLifecycles.get(event.channel)!.push(event);
      }
    }

    // Distribute events not attributed to a specific channel to all lifecycles
    if (specificLifecycles.size > 0) {
      for (const [channel, specificEvents] of specificLifecycles) {
        const combined = [...specificEvents, ...commonEvents];
        combined.sort((a, b) => Number(a.ts - b.ts));
        this.lifecycles.set(channel, combined);
        this.mapTrackIdsForChannel(channel, combined);
      }
    } else if (commonEvents.length > 0) {
      commonEvents.sort((a, b) => Number(a.ts - b.ts));
      this.lifecycles.set('Uncatgorised', commonEvents);
      this.mapTrackIdsForChannel('Uncatgorised', commonEvents);
    }
  }

  private mapTrackIdsForChannel(
    channel: string,
    events: RelatedEventWithChannel[],
  ) {
    const ids = new Set<number>();
    events.forEach((e) => ids.add(e.track_id));
    this.channelTrackIds.set(channel, ids);
  }

  private parseChannelFromEventName(name: string): string {
    const match = name.match(/inputChannel=([^,]+)/);
    if (match) return match[1].trim();

    if (name.startsWith('InputConsumer processing on ')) {
      const parts = name.split(' on ')[1];
      return parts.split(' (')[0].trim();
    }

    return 'Uncatgorised';
  }

  // --- Pinning Logic ---

  private handlePinToggle(channel: string, checked: boolean) {
    if (checked) this.pinnedChannels.add(channel);
    else this.pinnedChannels.delete(channel);

    this.updateWorkspacePinning();
  }

  private updateWorkspacePinning() {
    const globalPinnedTrackIds = new Set<number>();
    for (const channel of this.pinnedChannels) {
      const ids = this.channelTrackIds.get(channel);
      if (ids) {
        ids.forEach((id) => globalPinnedTrackIds.add(id));
      }
    }

    this.trace.currentWorkspace.flatTracks.forEach((trackNode) => {
      if (!trackNode.uri) return;

      const trackDescriptor = this.trace.tracks.getTrack(trackNode.uri);
      if (!trackDescriptor) return;

      const trackSqlIds = trackDescriptor.tags?.trackIds;
      if (!trackSqlIds || trackSqlIds.length === 0) return;

      const shouldBePinned = trackSqlIds.some((id) =>
        globalPinnedTrackIds.has(id),
      );

      if (shouldBePinned && !trackNode.isPinned) {
        trackNode.pin();
      } else if (!shouldBePinned && trackNode.isPinned) {
        trackNode.unpin();
      }
    });
  }

  private handleSelectionToggle(channel: string, checked: boolean) {
    if (checked) this.selectedChannel = channel;
    else if (this.selectedChannel === channel) this.selectedChannel = undefined;

    this.updateOverlay();
  }

  // --- Base Class Overrides ---

  override jumpToNextEvent(): void {
    const channel = this.selectedChannel;
    if (!channel) return;

    const events = this.lifecycles.get(channel);
    if (!events || events.length === 0) return;

    const currentSelection = this.trace.selection.selection;
    let idx = -1;
    if (currentSelection.kind === 'track_event') {
      idx = events.findIndex((e) => e.id === Number(currentSelection.eventId));
    }

    const nextIdx = (idx + 1) % events.length;
    this.focusOnEvent(events[nextIdx]);
  }

  override jumpToPreviousEvent(): void {
    const channel = this.selectedChannel;
    if (!channel) return;

    const events = this.lifecycles.get(channel);
    if (!events || events.length === 0) return;

    const currentSelection = this.trace.selection.selection;
    let idx = -1;
    if (currentSelection.kind === 'track_event') {
      idx = events.findIndex((e) => e.id === Number(currentSelection.eventId));
    }

    const prevIdx = (idx - 1 + events.length) % events.length;
    this.focusOnEvent(events[prevIdx]);
  }

  override focusOnEvent(event: RelatedEvent) {
    const track = this.trace.currentWorkspace.flatTracks.find((t) => {
      if (!t.uri) return false;
      const desc = this.trace.tracks.getTrack(t.uri);
      return desc?.tags?.trackIds?.includes(event.track_id);
    });

    const selectionOpts: SelectionOpts = {
      scrollToSelection: true,
      switchToCurrentSelectionTab: false,
    };

    if (track && track.uri) {
      this.trace.selection.selectTrackEvent(track.uri, event.id, selectionOpts);
    } else {
      // Fallback to slice table if track not found in workspace
      this.trace.selection.selectSqlEvent('slice', event.id, selectionOpts);
    }
  }

  private updateOverlay() {
    if (this.selectedChannel && this.lifecycles.has(this.selectedChannel)) {
      const events = this.lifecycles.get(this.selectedChannel)!;
      const eventIds = events.map((e) => e.id);

      if (this.originalDataset instanceof SourceDataset) {
        // Create a new SourceDataset filtered by the IDs of the selected channel
        const filteredDataset = new SourceDataset({
          ...this.originalDataset,
          filter: {
            col: 'id',
            in: eventIds,
          },
        });
        this.overlayUpdater.updateOverlayData(
          true,
          this.selectedChannel,
          filteredDataset,
        );
      } else {
        this.overlayUpdater.updateOverlayData(false);
      }
    } else {
      this.overlayUpdater.updateOverlayData(false);
    }
  }

  override getTitle(): string {
    return 'Related Input Events';
  }

  override render(): m.Children {
    if (this.isLoading) {
      return m(
        DetailsShell,
        {title: this.getTitle()},
        m('div.p-2', 'Loading events...'),
      );
    }

    if (this.lifecycles.size === 0) {
      return m(
        DetailsShell,
        {title: this.getTitle()},
        m('div.p-2', 'No related input events found.'),
      );
    }

    const columns = Array.from(this.lifecycles.entries()).map(
      ([channel, events]) => {
        const startTime = events[0].ts;
        const endTime =
          events[events.length - 1].ts + events[events.length - 1].dur;
        const totalLatency = Duration.humanise(endTime - startTime);

        const isSelected = this.selectedChannel === channel;
        const isPinned = this.pinnedChannels.has(channel);

        return m(
          GridLayoutColumn,
          {key: channel},
          m(
            Section,
            {title: channel},
            m(
              Tree,
              m(TreeNode, {left: 'Total Latency', right: totalLatency}),
              m(TreeNode, {
                left: 'Show Relations',
                right: m(Checkbox, {
                  checked: isSelected,
                  onchange: (e: Event) =>
                    this.handleSelectionToggle(
                      channel,
                      (e.target as HTMLInputElement).checked,
                    ),
                }),
              }),
              m(TreeNode, {
                left: 'Pin Tracks',
                right: m(Checkbox, {
                  checked: isPinned,
                  onchange: (e: Event) =>
                    this.handlePinToggle(
                      channel,
                      (e.target as HTMLInputElement).checked,
                    ),
                }),
              }),
            ),

            m(
              Section,
              {title: 'Events'},
              m(Grid, {
                columns: [
                  {key: 'id', header: m(GridHeaderCell, 'ID')},
                  {key: 'name', header: m(GridHeaderCell, 'Name')},
                  {key: 'ts', header: m(GridHeaderCell, 'Timestamp')},
                  {key: 'dt', header: m(GridHeaderCell, 'Delta')},
                ],
                rowData: events.map((event, idx) => {
                  const prev = events.at(idx - 1);
                  const delta = prev
                    ? Duration.humanise(event.ts - prev.ts)
                    : '-';

                  return [
                    m(GridCell, event.id),
                    m(
                      GridCell,
                      m(
                        Anchor,
                        {
                          icon: Icons.UpdateSelection,
                          onclick: (e: MouseEvent) => {
                            e.preventDefault();
                            this.focusOnEvent(event);
                          },
                        },
                        event.name,
                      ),
                    ),
                    m(
                      GridCell,
                      m(Timestamp, {trace: this.trace, ts: event.ts}),
                    ),
                    m(GridCell, delta),
                  ];
                }),
              }),
            ),
          ),
        );
      },
    );

    return m(DetailsShell, {title: this.getTitle()}, m(GridLayout, columns));
  }
}

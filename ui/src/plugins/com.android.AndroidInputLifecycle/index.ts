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

import {RelatedEventsOverlay} from '../../components/related_events/related_events_overlay';
import type {ArrowConnection} from '../../components/related_events/arrow_visualiser';
import {TrackPinningManager} from '../../components/related_events/utils';
import {showModal} from '../../widgets/modal';
import {Select} from '../../widgets/select';
import {Form, FormLabel} from '../../widgets/form';
import {STR} from '../../trace_processor/query_result';
import {Time} from '../../base/time';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';

import {
  AndroidInputEventSource,
  type InputChainRow,
} from './android_input_event_source';
import {AndroidInputLifecycleTab} from './tab';
import type {QueryResult} from '../../base/query_slot';
import type {InputLifecycleExtension, NavTarget} from './extensions/interface';
import {PixelInputLifecycleExtension} from './extensions/pixel_extension';

export default class AndroidInputLifecyclePlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidInputLifecycle';
  static readonly description =
    'Visualise connected input events in the lifecycle from touch to frame, ' +
    "with latencies for the various input stages. Activate by running the command 'Android: View Input Lifecycle'.";

  private visibleRowIds = new Set<string>();
  private lastAppliedEventId?: number;
  private allEventConnections: ArrowConnection[] = [];
  private activeExcludeSpeculative?: boolean;
  private pinnedApp?: string;
  private pinnedTrackUris: string[] = [];

  async onTraceLoad(trace: Trace): Promise<void> {
    await trace.engine.query('INCLUDE PERFETTO MODULE android.input;');

    const extensions: InputLifecycleExtension[] = [
      new PixelInputLifecycleExtension(),
    ];

    const activeExtensions: InputLifecycleExtension[] = [];
    for (const ext of extensions) {
      if (await ext.isEligible(trace)) {
        if (ext.requiredModules !== undefined) {
          for (const mod of ext.requiredModules) {
            await trace.engine.query(`INCLUDE PERFETTO MODULE ${mod};`);
          }
        }
        activeExtensions.push(ext);
      }
    }

    const source = new AndroidInputEventSource(trace, activeExtensions);
    const pinningManager = new TrackPinningManager(trace);

    trace.tracks.registerOverlay(
      new RelatedEventsOverlay(trace, () => this.getConnections(trace, source)),
    );

    trace.tabs.registerTab({
      uri: 'com.android.AndroidInputLifecycleTab',
      isEphemeral: false,
      content: {
        getTitle: () => 'Android Input Lifecycle',
        render: () => {
          const {data: rows, isPending} = this.useRowState(trace, source);

          if (rows) {
            this.applyInitialSelection(trace, rows);
          }

          return m(AndroidInputLifecycleTab, {
            trace,
            rows: rows ?? [],
            visibleRowIds: this.visibleRowIds,
            loading: isPending,
            pinningManager,
            onToggleVisibility: (rowId) => this.toggleVisibility(rowId),
            onToggleAllVisibility: () => this.toggleAllVisibility(rows ?? []),
            activeExtensions,
          });
        },
      },
      onHide: () => {
        this.visibleRowIds.clear();
        this.lastAppliedEventId = undefined;
      },
    });

    trace.commands.registerCommand({
      id: 'com.android.openAndroidInputLifecycleTab',
      name: 'Android: View Input Lifecycle',
      callback: () => {
        trace.tabs.showTab('com.android.AndroidInputLifecycleTab');
      },
    });

    trace.commands.registerCommand({
      id: 'com.android.AndroidInputLifecycle.drawAllArrowsWithSpeculative',
      name: 'Android: Draw all input arrows (With speculative)',
      callback: async () => {
        await this.toggleDrawAllArrows(source, false);
      },
    });

    trace.commands.registerCommand({
      id: 'com.android.AndroidInputLifecycle.drawAllArrowsNoSpeculative',
      name: 'Android: Draw all input arrows (No speculative)',
      callback: async () => {
        await this.toggleDrawAllArrows(source, true);
      },
    });

    trace.commands.registerCommand({
      id: 'com.android.AndroidInputLifecycle.pinTracks',
      name: 'Android: Pin input pipeline related tracks',
      callback: async () => {
        const apps = await this.getUniqueInputApps(trace);
        if (apps.length === 0) {
          await showModal({
            title: 'Pin input pipeline related tracks',
            icon: 'warning',
            content: m(
              'p',
              'No Android apps were found receiving input in this trace.',
            ),
            buttons: [{text: 'OK', primary: true}],
          });
          return;
        }

        let selectedApp = apps[0];

        await showModal({
          title: 'Pin input pipeline tracks for app',
          icon: 'help',
          content: () =>
            m(
              Form,
              m(
                FormLabel,
                {for: 'app-select'},
                'Select the Android app you want to check:',
              ),
              m(
                Select,
                {
                  id: 'app-select',
                  onchange: (e: Event) => {
                    const target = e.target as HTMLSelectElement;
                    selectedApp = target.value;
                  },
                },
                apps.map((app) =>
                  m('option', {value: app, selected: app === selectedApp}, app),
                ),
              ),
            ),
          buttons: [
            {
              text: 'Pin Tracks',
              primary: true,
              action: async () => {
                this.pinnedApp = selectedApp;
                pinningManager.unpinTracks(this.pinnedTrackUris);

                const rows = await source.getRowsForApp(selectedApp);
                this.pinnedTrackUris = Array.from(
                  new Set(rows.flatMap((r) => r.allTrackUris)),
                );
                pinningManager.pinTracks(this.pinnedTrackUris);

                if (this.activeExcludeSpeculative !== undefined) {
                  this.allEventConnections = this.extractConnectionsFromRows(
                    source,
                    rows,
                  );
                }
              },
            },
            {
              text: 'Cancel',
            },
          ],
        });
      },
    });
  }

  // Fetch or reuse cached row data for the currently selected slice. Can call
  // this function every render cycle without performance concerns, as the
  // underlying data slot will ensure the query is only executed once per
  // sliceId.
  private useRowState(
    trace: Trace,
    source: AndroidInputEventSource,
  ): QueryResult<InputChainRow[]> {
    const selection = trace.selection.selection;

    if (selection.kind !== 'track_event') {
      return {data: [], isPending: false, isFresh: true};
    }

    return source.use(selection.eventId);
  }

  private applyInitialSelection(trace: Trace, rows: InputChainRow[]) {
    const selection = trace.selection.selection;
    if (selection.kind !== 'track_event') return;

    const eventId = selection.eventId;
    if (this.lastAppliedEventId === eventId) return;

    this.lastAppliedEventId = eventId;
    this.visibleRowIds.clear();

    for (const row of rows) {
      const ids: number[] = [];
      for (const stageData of row.stagesData.values()) {
        if (stageData.nav) {
          ids.push(stageData.nav.id);
        }
      }
      if (ids.includes(eventId)) {
        this.visibleRowIds.add(row.uiRowId);
        break;
      }
    }
  }

  private getConnections(
    trace: Trace,
    source: AndroidInputEventSource,
  ): ArrowConnection[] {
    const connections: ArrowConnection[] = [...this.allEventConnections];
    const {data: rows} = this.useRowState(trace, source);
    if (!rows) return connections;

    const visibleRows = rows.filter((r) => this.visibleRowIds.has(r.uiRowId));
    connections.push(...this.extractConnectionsFromRows(source, visibleRows));
    return connections;
  }

  private async getUniqueInputApps(trace: Trace): Promise<string[]> {
    const query = `
      SELECT DISTINCT process_name
      FROM android_input_events
      -- Filter for actual app window channels (e.g. "process/activity" containing a "/")
      -- and exclude system channels (like "PointerEventDispatcher" or "[Gesture Monitor]").
      WHERE process_name IS NOT NULL AND event_channel LIKE '%/%'
      ORDER BY process_name;
    `;
    const result = await trace.engine.query(query);
    const apps: string[] = [];
    const it = result.iter({process_name: STR});
    for (; it.valid(); it.next()) {
      apps.push(it.process_name);
    }
    return apps;
  }

  private async toggleDrawAllArrows(
    source: AndroidInputEventSource,
    excludeSpeculative: boolean,
  ): Promise<void> {
    if (this.activeExcludeSpeculative === excludeSpeculative) {
      this.allEventConnections = [];
      this.activeExcludeSpeculative = undefined;
      return;
    }

    const rows = await source.getRowsForApp(this.pinnedApp, excludeSpeculative);
    this.allEventConnections = this.extractConnectionsFromRows(source, rows);
    this.activeExcludeSpeculative = excludeSpeculative;
  }

  private extractConnectionsFromRows(
    source: AndroidInputEventSource,
    rows: InputChainRow[],
  ): ArrowConnection[] {
    const specs = source.getStageSpecs();
    const connections: ArrowConnection[] = [];
    for (const row of rows) {
      const steps: NavTarget[] = [];
      for (const spec of specs) {
        const stageData = row.stagesData.get(spec.key);
        if (stageData?.nav) {
          steps.push(stageData.nav);
        }
      }

      for (let i = 0; i < steps.length - 1; i++) {
        const start = steps[i];
        const end = steps[i + 1];
        connections.push({
          start: {
            trackUri: start.trackUri,
            ts: Time.add(start.ts, start.dur),
            depth: start.depth,
          },
          end: {
            trackUri: end.trackUri,
            ts: end.ts,
            depth: end.depth,
          },
        });
      }
    }
    return connections;
  }

  private toggleVisibility(rowId: string) {
    if (this.visibleRowIds.has(rowId)) {
      this.visibleRowIds.delete(rowId);
    } else {
      this.visibleRowIds.add(rowId);
    }
  }

  private toggleAllVisibility(rows: InputChainRow[]) {
    const allVisible = rows.every((r) => this.visibleRowIds.has(r.uiRowId));
    if (allVisible) {
      this.visibleRowIds.clear();
    } else {
      rows.forEach((r) => this.visibleRowIds.add(r.uiRowId));
    }
  }
}

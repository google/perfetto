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
import {Time} from '../../base/time';
import {z} from 'zod';
import type {App} from '../../public/app';
import type {Setting} from '../../public/settings';
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
import {AndroidFramesInputLifecycleExtension} from './extensions/android_frames_extension';

const EXTENSIONS: InputLifecycleExtension[] = [
  new PixelInputLifecycleExtension(),
  new AndroidFramesInputLifecycleExtension(),
];

export default class AndroidInputLifecyclePlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidInputLifecycle';
  static readonly description =
    'Visualise connected input events in the lifecycle from touch to frame, ' +
    "with latencies for the various input stages. Activate by running the command 'Android: View Input Lifecycle'.";

  private static extensionSettings = new Map<string, Setting<boolean>>();

  static onActivate(app: App): void {
    for (const ext of EXTENSIONS) {
      const setting = app.settings.register({
        id: `com.android.AndroidInputLifecycle.extension.${ext.id}`,
        name: `Enable ${ext.name} extension`,
        description: `Enable custom stages in the tab and overlay for ${ext.name} extension.`,
        schema: z.boolean(),
        defaultValue: true,
        requiresReload: true,
      });
      AndroidInputLifecyclePlugin.extensionSettings.set(ext.id, setting);
    }
  }

  static isExtensionEnabled(id: string): boolean {
    return AndroidInputLifecyclePlugin.extensionSettings.get(id)?.get() ?? true;
  }

  private visibleRowIds = new Set<string>();
  private lastAppliedEventId?: number;

  async onTraceLoad(trace: Trace): Promise<void> {
    await trace.engine.query('INCLUDE PERFETTO MODULE android.input;');

    const activeExtensions: InputLifecycleExtension[] = [];
    for (const ext of EXTENSIONS) {
      if (AndroidInputLifecyclePlugin.isExtensionEnabled(ext.id)) {
        if (await ext.isEligible(trace)) {
          if (ext.requiredModules !== undefined) {
            for (const mod of ext.requiredModules) {
              await trace.engine.query(`INCLUDE PERFETTO MODULE ${mod};`);
            }
          }
          activeExtensions.push(ext);
        }
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
    const {data: rows} = this.useRowState(trace, source);
    if (!rows) return [];

    const specs = source.getStageSpecs();

    const connections: ArrowConnection[] = [];
    for (const row of rows) {
      if (!this.visibleRowIds.has(row.uiRowId)) continue;

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

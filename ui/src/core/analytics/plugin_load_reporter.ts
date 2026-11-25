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

import type {AnalyticsInternal, TraceLoadId} from './analytics_impl';

// How many of the slowest plugins get a plugin_loaded event, even if they
// registered no tracks.
const SLOWEST_PLUGIN_COUNT = 10;

// Sends the plugin_loaded events for one trace load: one for each plugin that
// registered tracks for the trace, and one for each of the slowest plugins.
export class PluginLoadReporter {
  // The plugins that already have a plugin_loaded event for this trace load.
  private readonly reported = new Set<string>();

  // Each plugin's onTraceLoad() time, once loading has finished.
  private loadTimesMs?: ReadonlyMap<string, number>;

  constructor(
    private readonly traceLoadId: TraceLoadId,
    // Require a dependency injection of the analytics manager.
    // Used to send the event by calling analytics.logAdvancedEvent().
    private readonly analytics: AnalyticsInternal,
  ) {}

  // Call once all plugins have finished loading, with how long each
  // took (keyed by plugin ID) and which plugins registered tracks.
  reportLoad(
    loadTimesMs: ReadonlyMap<string, number>,
    pluginsWithTracks: ReadonlySet<string>,
  ): void {
    this.loadTimesMs = loadTimesMs;
    const slowest = new Set(
      [...loadTimesMs.entries()]
        .sort(([, a], [, b]) => b - a)
        .slice(0, SLOWEST_PLUGIN_COUNT)
        .map(([pluginId]) => pluginId),
    );
    for (const [pluginId, loadMs] of loadTimesMs) {
      const hasTracks = pluginsWithTracks.has(pluginId);
      if (hasTracks || slowest.has(pluginId)) {
        this.sendPluginLoaded(pluginId, loadMs, hasTracks);
      }
    }
  }

  // Call whenever a plugin registers a track. Check against the list of plugins
  // we've already sent plugin_loaded events for. If it's not in the list, then
  // send a new plugin_loaded event.
  //
  // Reason for this is to catch the edge case that a plugin registers tracks after
  // or outside of trace loading time, because we still want those counted.
  onTrackRegistered(pluginId: string): void {
    if (this.loadTimesMs === undefined || this.reported.has(pluginId)) return;
    const loadMs = this.loadTimesMs.get(pluginId);
    if (loadMs === undefined) return;
    this.sendPluginLoaded(pluginId, loadMs, true);
  }

  private sendPluginLoaded(
    pluginId: string,
    loadMs: number,
    hasTracks: boolean,
  ) {
    this.reported.add(pluginId);
    this.analytics.logAdvancedEvent({
      kind: 'plugin_loaded',
      traceLoadId: this.traceLoadId,
      plugin: pluginId,
      loadMs: Math.round(loadMs),
      hasTracks,
    });
  }
}

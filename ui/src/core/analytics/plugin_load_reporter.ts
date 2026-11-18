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

  private sendPluginLoaded(
    pluginId: string,
    loadMs: number,
    hasTracks: boolean,
  ) {
    this.analytics.logAdvancedEvent({
      kind: 'plugin_loaded',
      traceLoadId: this.traceLoadId,
      plugin: pluginId,
      loadMs: Math.round(loadMs),
      hasTracks,
    });
  }
}

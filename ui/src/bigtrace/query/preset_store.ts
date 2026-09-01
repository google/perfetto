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
import {BigtraceQueryClient, type TracePreset} from './bigtrace_query_client';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';

// In-memory cache of the analysis-presets catalog, fetched lazily on first
// home/settings mount (refetched on `load(true)`). A backend that doesn't
// serve /trace_presets, or is unreachable, yields an empty list.
class PresetStore {
  presets: ReadonlyArray<TracePreset> = [];
  isLoading = false;
  private loaded = false;

  async load(force = false): Promise<void> {
    if (this.loaded && !force) return;
    this.loaded = true;
    this.isLoading = true;
    try {
      const client = new BigtraceQueryClient(getBigtraceEndpoint());
      this.presets = await client.listTracePresets();
    } catch {
      // No /trace_presets endpoint, or backend unreachable: no presets.
      this.presets = [];
    } finally {
      this.isLoading = false;
      m.redraw();
    }
  }
}

export const presetStore = new PresetStore();

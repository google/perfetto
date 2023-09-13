// Copyright (C) 2023 The Android Open Source Project
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

import {
  MetricVisualisation,
  Plugin,
  PluginContext,
  PluginInfo,
  TracePluginContext,
  TrackInfo,
} from '../../public';

interface State {
  foo: string;
}

// SKELETON: Rename this class to match your plugin.
class Skeleton implements Plugin<State> {
  onActivate(_: PluginContext): void {
    //
  }

  migrate(_initialState: unknown): State {
    return {foo: 'bar'};
  }

  async onTraceLoad(_: TracePluginContext<State>): Promise<void> {
    //
  }

  async onTraceUnload(_: TracePluginContext<State>): Promise<void> {
    //
  }

  onDeactivate(_: PluginContext): void {
    //
  }

  async findPotentialTracks(_: TracePluginContext<State>):
      Promise<TrackInfo[]> {
    return [];
  }

  metricVisualisations(_: TracePluginContext<State>): MetricVisualisation[] {
    return [];
  }
}

export const plugin: PluginInfo<State> = {
  // SKELETON: Update pluginId to match the directory of the plugin.
  pluginId: 'com.example.Skeleton',
  plugin: Skeleton,
};

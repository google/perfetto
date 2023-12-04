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
  createStore,
  MetricVisualisation,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  Store,
} from '../../public';

interface State {
  foo: string;
}

// SKELETON: Rename this class to match your plugin.
class Skeleton implements Plugin {
  private store: Store<State> = createStore({foo: 'foo'});

  onActivate(_: PluginContext): void {
    //
  }

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    this.store = ctx.mountStore((_: unknown): State => {
      return {foo: 'bar'};
    });

    this.store.edit((state) => {
      state.foo = 'baz';
    });
  }

  async onTraceUnload(_: PluginContextTrace): Promise<void> {
    //
  }

  onDeactivate(_: PluginContext): void {
    //
  }

  metricVisualisations(_: PluginContextTrace): MetricVisualisation[] {
    return [];
  }
}

export const plugin: PluginDescriptor = {
  // SKELETON: Update pluginId to match the directory of the plugin.
  pluginId: 'com.example.Skeleton',
  plugin: Skeleton,
};

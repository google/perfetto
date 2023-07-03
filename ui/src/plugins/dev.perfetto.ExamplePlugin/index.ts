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

import {EngineProxy, PluginContext} from '../../common/plugin_api';
import {TracePlugin} from '../../common/plugins';
import {Store} from '../../frontend/store';

interface ExampleState {
  counter: number;
}

// This is just an example plugin, used to prove that the plugin system works.
class ExamplePlugin implements TracePlugin {
  static migrate(_initialState: unknown): ExampleState {
    return {counter: 0};
  }

  constructor(_store: Store<ExampleState>, _engine: EngineProxy) {
    // No-op
  }

  dispose(): void {
    // No-op
  }
}

function activate(ctx: PluginContext) {
  ctx.registerTracePluginFactory(ExamplePlugin);
}

export const plugin = {
  pluginId: 'dev.perfetto.ExamplePlugin',
  activate,
};

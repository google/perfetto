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
  Command,
  EngineProxy,
  PluginContext,
  Store,
  TracePlugin,
  Viewer,
} from '../../public';

interface State {
  counter: number;
}

// This example plugin shows using state that is persisted in the
// permalink.
class ExampleState implements TracePlugin {
  static migrate(_initialState: unknown): State {
    // TODO(hjd): Show validation example.

    return {
      counter: 0,
    };
  }

  private store: Store<State>;
  private viewer: Viewer;

  constructor(store: Store<State>, _engine: EngineProxy, viewer: Viewer) {
    this.store = store;
    this.viewer = viewer;
  }

  dispose(): void {
    // No-op
  }

  commands(): Command[] {
    return [
      {
        id: 'dev.perfetto.ExampleState#ShowCounter',
        name: 'Show ExampleState counter',
        callback: () => {
          const counter = this.store.state.counter;
          this.viewer.tabs.openQuery(
              `SELECT ${counter} as counter;`, `Show counter ${counter}`);
        },
      },
    ];
  }
}

export const plugin = {
  pluginId: 'dev.perfetto.ExampleState',
  activate(ctx: PluginContext) {
    ctx.registerTracePluginFactory(ExampleState);
  },
};

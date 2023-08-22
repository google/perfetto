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
  Plugin,
  PluginContext,
  PluginInfo,
  TracePluginContext,
} from '../../public';

interface State {
  counter: number;
}

// This example plugin shows using state that is persisted in the
// permalink.
class ExampleState implements Plugin<State> {
  migrate(initialState: unknown): State {
    if (initialState && typeof initialState === 'object' &&
        'counter' in initialState && typeof initialState.counter === 'number') {
      return {counter: initialState.counter};
    } else {
      return {counter: 0};
    }
  }

  onActivate(_: PluginContext): void {
    //
  }

  traceCommands(ctx: TracePluginContext<State>): Command[] {
    const {viewer, store} = ctx;
    return [
      {
        id: 'dev.perfetto.ExampleState#ShowCounter',
        name: 'Show ExampleState counter',
        callback: () => {
          const counter = store.state.counter;
          viewer.tabs.openQuery(
              `SELECT ${counter} as counter;`, `Show counter ${counter}`);
          store.edit((draft) => ++draft.counter);
        },
      },
    ];
  }
}

export const plugin: PluginInfo<State> = {
  pluginId: 'dev.perfetto.ExampleState',
  plugin: ExampleState,
};

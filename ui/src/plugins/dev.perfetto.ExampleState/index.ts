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

import {createStore, Store} from '../../base/store';
import {exists} from '../../base/utils';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {addQueryResultsTab} from '../../public/lib/query_table/query_result_tab';

interface State {
  counter: number;
}

// This example plugin shows using state that is persisted in the
// permalink.
class ExampleState implements PerfettoPlugin {
  private store: Store<State> = createStore({counter: 0});

  private migrate(initialState: unknown): State {
    if (
      exists(initialState) &&
      typeof initialState === 'object' &&
      'counter' in initialState &&
      typeof initialState.counter === 'number'
    ) {
      return {counter: initialState.counter};
    } else {
      return {counter: 0};
    }
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.store = ctx.mountStore((init: unknown) => this.migrate(init));

    ctx.commands.registerCommand({
      id: 'dev.perfetto.ExampleState#ShowCounter',
      name: 'Show ExampleState counter',
      callback: () => {
        const counter = this.store.state.counter;
        addQueryResultsTab(ctx, {
          query: `SELECT ${counter} as counter;`,
          title: `Show counter ${counter}`,
        });
        this.store.edit((draft) => {
          ++draft.counter;
        });
      },
    });
  }

  async onTraceUnload(_: Trace): Promise<void> {
    this.store[Symbol.dispose]();
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.ExampleState',
  plugin: ExampleState,
};

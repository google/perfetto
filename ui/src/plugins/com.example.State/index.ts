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

import {z} from 'zod';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import QueryPagePlugin from '../dev.perfetto.QueryPage';

const STATE_SCHEMA = z.object({
  counter: z.number().default(0),
});

type State = z.infer<typeof STATE_SCHEMA>;

const DEFAULT_STATE: State = {
  counter: 0,
};

// This example plugin shows using state that is persisted in the
// permalink.
export default class implements PerfettoPlugin {
  static readonly id = 'com.example.State';
  static readonly dependencies = [QueryPagePlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const storage = ctx.registerStorage({
      id: 'com.example.SkeletonStore',
      schema: STATE_SCHEMA,
      defaultValue: DEFAULT_STATE,
    });

    ctx.commands.registerCommand({
      id: 'com.example.ShowCounter',
      name: 'Show ExampleState counter',
      callback: () => {
        const counter = storage.get().counter;
        ctx.plugins.getPlugin(QueryPagePlugin).addQueryResultsTab({
          query: `SELECT ${counter} as counter;`,
          title: `Show counter ${counter}`,
        });
        storage.set({
          counter: counter + 1,
        });
      },
    });
  }
}

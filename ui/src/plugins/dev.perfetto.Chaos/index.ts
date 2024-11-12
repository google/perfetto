// Copyright (C) 2024 The Android Open Source Project
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

import {Trace} from '../../public/trace';
import {App} from '../../public/app';
import {addDebugSliceTrack} from '../../public/debug_tracks';
import {PerfettoPlugin} from '../../public/plugin';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Chaos';

  static onActivate(ctx: App): void {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.Chaos#CrashNow',
      name: 'Chaos: crash now',
      callback: () => {
        throw new Error('Manual crash from dev.perfetto.Chaos#CrashNow');
      },
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.Chaos#CrashNowQuery',
      name: 'Chaos: run crashing query',
      callback: () => {
        ctx.engine.query(`this is a
          syntactically
          invalid
          query
          over
          many
          lines
        `);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.Chaos#AddCrashingDebugTrack',
      name: 'Chaos: add crashing debug track',
      callback: () => {
        addDebugSliceTrack({
          trace: ctx,
          data: {
            sqlSource: `
              syntactically
              invalid
              query
              over
              many
            `,
          },
          title: `Chaos track`,
        });
      },
    });
  }
}

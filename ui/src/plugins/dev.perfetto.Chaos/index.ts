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
import {addDebugSliceTrack} from '../../components/tracks/debug_tracks';
import {PerfettoPlugin} from '../../public/plugin';
import {
  assertDefined,
  assertExists,
  assertFalse,
  assertIsInstanceOf,
  assertTrue,
  assertUnreachable,
  fail,
} from '../../base/logging';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Chaos';

  static onActivate(ctx: App): void {
    const testObject = {
      a: 123,
      b: null,
      c: undefined,
      d: 'invalid' as 'a' | 'b',
    };

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CrashNow',
      name: 'Chaos: crash now',
      callback: () => {
        throw new Error('Manual crash from dev.perfetto.Chaos#CrashNow');
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AssertTrue',
      name: 'Chaos: assertTrue failure',
      callback: () => {
        assertTrue(testObject.a > 200);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AssertFalse',
      name: 'Chaos: assertFalse failure',
      callback: () => {
        assertFalse(testObject.a < 200);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AssertExists',
      name: 'Chaos: assertExists failure',
      callback: () => {
        assertExists(testObject.b);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AssertDefined',
      name: 'Chaos: assertDefined failure',
      callback: () => {
        assertDefined(testObject.c);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AssertUnreachable',
      name: 'Chaos: assertUnreachable failure',
      callback: () => {
        switch (testObject.d) {
          case 'a':
            break;
          case 'b':
            break;
          default:
            assertUnreachable(testObject.d);
        }
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.assertIsInstanceOf',
      name: 'Chaos: assertIsInstanceOf failure',
      callback: () => {
        assertIsInstanceOf(testObject, Array);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.Fail',
      name: 'Chaos: fail()',
      callback: () => {
        fail('Intentional failure from Chaos plugin');
      },
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.CrashNowQuery',
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
      id: 'dev.perfetto.AddCrashingDebugTrack',
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

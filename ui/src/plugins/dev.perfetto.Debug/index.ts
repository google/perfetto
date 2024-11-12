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
  addDebugCounterTrack,
  addDebugSliceTrack,
} from '../../public/lib/tracks/debug_tracks';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {exists} from '../../base/utils';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.DebugTracks';
  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'perfetto.DebugTracks#addDebugSliceTrack',
      name: 'Add debug slice track',
      callback: async (arg: unknown) => {
        // This command takes a query and creates a debug track out of it The
        // query can be passed in using the first arg, or if this is not defined
        // or is the wrong type, we prompt the user for it.
        const query = await getStringFromArgOrPrompt(ctx, arg);
        if (exists(query)) {
          await addDebugSliceTrack({
            trace: ctx,
            data: {
              sqlSource: query,
            },
            title: 'Debug slice track',
          });
        }
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.DebugTracks#addDebugCounterTrack',
      name: 'Add debug counter track',
      callback: async (arg: unknown) => {
        const query = await getStringFromArgOrPrompt(ctx, arg);
        if (exists(query)) {
          await addDebugCounterTrack({
            trace: ctx,
            data: {
              sqlSource: query,
            },
            title: 'Debug slice track',
          });
        }
      },
    });
  }
}

// If arg is a string, return it, otherwise prompt the user for a string. An
// exception is thrown if the prompt is cancelled, so this function handles this
// and returns undefined in this case.
async function getStringFromArgOrPrompt(
  ctx: Trace,
  arg: unknown,
): Promise<string | undefined> {
  if (typeof arg === 'string') {
    return arg;
  } else {
    return await ctx.omnibox.prompt('Enter a query...');
  }
}

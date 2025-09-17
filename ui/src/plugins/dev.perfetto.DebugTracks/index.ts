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
} from '../../components/tracks/debug_tracks';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {exists} from '../../base/utils';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.DebugTracks';
  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.AddDebugSliceTrack',
      name: 'Add debug slice track',
      callback: async (queryArg: unknown, titleArg: unknown) => {
        // This command takes a query and creates a debug track out of it The
        // query can be passed in using the first arg, or if this is not defined
        // or is the wrong type, we prompt the user for it.
        const query = await getStringFromArgOrPrompt(
          ctx,
          queryArg,
          'Enter a query...',
        );
        if (!exists(query)) return;

        const title = getStringFromArgOrDefault(titleArg, 'Debug slice track');

        await addDebugSliceTrack({
          trace: ctx,
          data: {
            sqlSource: query,
          },
          title,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AddDebugCounterTrack',
      name: 'Add debug counter track',
      callback: async (queryArg: unknown, titleArg: unknown) => {
        const query = await getStringFromArgOrPrompt(
          ctx,
          queryArg,
          'Enter a query...',
        );
        if (!exists(query)) return;

        const title = getStringFromArgOrDefault(
          titleArg,
          'Debug counter track',
        );

        await addDebugCounterTrack({
          trace: ctx,
          data: {
            sqlSource: query,
          },
          title,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AddDebugSliceTrackWithPivot',
      name: 'Add debug slice track with pivot',
      callback: async (
        queryArg: unknown,
        pivotArg: unknown,
        titleArg: unknown,
      ) => {
        const query = await getStringFromArgOrPrompt(
          ctx,
          queryArg,
          'Enter a query...',
        );
        if (!exists(query)) return;

        const pivotColumn = await getStringFromArgOrPrompt(
          ctx,
          pivotArg,
          'Enter column name to pivot on...',
        );
        if (!pivotColumn) return;

        const title = getStringFromArgOrDefault(titleArg, 'Debug slice track');

        await addDebugSliceTrack({
          trace: ctx,
          data: {
            sqlSource: query,
          },
          title,
          pivotOn: pivotColumn,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AddDebugCounterTrackWithPivot',
      name: 'Add debug counter track with pivot',
      callback: async (
        queryArg: unknown,
        pivotArg: unknown,
        titleArg: unknown,
      ) => {
        const query = await getStringFromArgOrPrompt(
          ctx,
          queryArg,
          'Enter a query...',
        );
        if (!exists(query)) return;

        const pivotColumn = await getStringFromArgOrPrompt(
          ctx,
          pivotArg,
          'Enter column name to pivot on...',
        );
        if (!pivotColumn) return;

        const title = getStringFromArgOrDefault(
          titleArg,
          'Debug counter track',
        );

        await addDebugCounterTrack({
          trace: ctx,
          data: {
            sqlSource: query,
          },
          title,
          pivotOn: pivotColumn,
        });
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
  promptText: string,
): Promise<string | undefined> {
  if (typeof arg === 'string') {
    return arg;
  } else {
    return await ctx.omnibox.prompt(promptText);
  }
}

// If arg is a string, return it, otherwise return the default value.
function getStringFromArgOrDefault(arg: unknown, defaultValue: string): string {
  return typeof arg === 'string' ? arg : defaultValue;
}

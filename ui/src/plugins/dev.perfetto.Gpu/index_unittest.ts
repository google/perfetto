// Copyright (C) 2026 The Android Open Source Project
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

import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import GpuPlugin from './index';

function mockQueryResult(rows: ReadonlyArray<Record<string, unknown>>) {
  return {
    firstRow: () => rows[0] ?? {},
    iter: () => {
      let i = 0;
      return new Proxy(
        {},
        {
          get(_target, prop: string) {
            if (prop === 'valid') return () => i < rows.length;
            if (prop === 'next') {
              return () => {
                i++;
              };
            }
            return rows[i]?.[prop];
          },
        },
      );
    },
  };
}

test('GpuPlugin registers unique URIs when multiple traces emit gpufreq for the same ugpu', async () => {
  const registeredUris = new Set<string>();
  const gpuGroup = new TrackNode({name: 'GPU', isSummary: true});

  const trace = {
    engine: {
      query: vi.fn(async (sql: string) => {
        if (sql.includes('select count(*) as cnt from gpu')) {
          return mockQueryResult([{cnt: 2}]);
        }
        if (sql.includes('from machine')) {
          return mockQueryResult([{cnt: 2}]);
        }
        if (sql.includes("where gct.name = 'gpufreq'")) {
          return mockQueryResult([
            {
              id: 16,
              gpuId: 0,
              machineId: 0,
              machineName: null,
              machineLabelIndex: 0,
              ugpu: 0,
              gpuName: 'AMD Radeon Graphics',
            },
            {
              id: 48,
              gpuId: 0,
              machineId: 0,
              machineName: null,
              machineLabelIndex: 0,
              ugpu: 0,
              gpuName: 'NVIDIA H100',
            },
          ]);
        }
        return mockQueryResult([]);
      }),
      tryQuery: vi.fn(async () => ({success: true})),
    },
    defaultWorkspace: new TrackNode(),
    plugins: {
      getPlugin: () => ({
        getOrCreateStandardGroup: () => gpuGroup,
      }),
    },
    tracks: {
      registerTrack: vi.fn(({uri}: {uri: string}) => {
        if (registeredUris.has(uri)) {
          throw new Error(`Registrant ${uri} already exists in the registry`);
        }
        registeredUris.add(uri);
      }),
    },
  } as unknown as Trace;

  const plugin = new GpuPlugin();
  await plugin.onTraceLoad(trace);

  expect([...registeredUris]).toEqual([
    '/gpu_frequency_0_16',
    '/gpu_frequency_0_48',
  ]);
});

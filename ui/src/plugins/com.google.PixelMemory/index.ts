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

import {addDebugCounterTrack} from '../../components/tracks/debug_tracks';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';

const CREATE_BREAKDOWN_TABLE_SQL = `
  DROP TABLE IF EXISTS process_memory_breakdown;
  CREATE VIRTUAL TABLE process_memory_breakdown
  USING
    SPAN_OUTER_JOIN(
      android_gpu_memory_per_process PARTITIONED upid,
      memory_rss_and_swap_per_process PARTITIONED upid
    );
`;

export default class implements PerfettoPlugin {
  static readonly id = 'com.google.PixelMemory';

  async onTraceLoad(ctx: Trace): Promise<void> {
    // Helper function to set up the breakdown tables.
    const setupTable = async () => {
      await ctx.engine.query('INCLUDE PERFETTO MODULE android.gpu.memory;');
      await ctx.engine.query('INCLUDE PERFETTO MODULE linux.memory.process;');
      await ctx.engine.query(CREATE_BREAKDOWN_TABLE_SQL);
    };

    // This command shows the total memory track.
    ctx.commands.registerCommand({
      id: 'dev.perfetto.PixelMemory#ShowTotalMemory',
      name: 'Add tracks: show a process total memory',
      callback: async (pid) => {
        if (pid === undefined) {
          pid = prompt('Enter a process pid', '');
          if (pid === null) return;
        }

        await setupTable();

        await addDebugCounterTrack({
          trace: ctx,
          data: {
            sqlSource: `
                SELECT
                  ts,
                  COALESCE(rss_and_swap, 0) + COALESCE(gpu_memory, 0) AS value
                FROM process_memory_breakdown
                WHERE pid = ${pid}
            `,
            columns: ['ts', 'value'],
          },
          title: pid + '_rss_anon_file_swap_shmem_gpu',
        });
      },
    });

    // This command shows the memory track excluding file RSS.
    ctx.commands.registerCommand({
      id: 'dev.perfetto.PixelMemory#ShowRssAnonShmemSwapGpuMemory',
      name: 'Add tracks: show a process total memory (excluding file RSS)',
      callback: async (pid) => {
        if (pid === undefined) {
          pid = prompt('Enter a process pid', '');
          if (pid === null) return;
        }

        await setupTable();

        await addDebugCounterTrack({
          trace: ctx,
          data: {
            sqlSource: `
              SELECT
                ts,
                COALESCE(anon_rss_and_swap, 0) + COALESCE(shmem_rss, 0) + COALESCE(gpu_memory, 0) AS value
              FROM process_memory_breakdown
              WHERE pid = ${pid}
            `,
            columns: ['ts', 'value'],
          },
          title: `${pid}_rss_anon_shmem_swap_gpu`,
        });
      },
    });
  }
}

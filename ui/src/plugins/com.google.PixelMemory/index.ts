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
  private tablesInitialized = false;

  // Helper to set up the breakdown tables idempotently.
  private async setupTables(ctx: Trace) {
    if (this.tablesInitialized) {
      return;
    }
    await ctx.engine.query('INCLUDE PERFETTO MODULE android.gpu.memory;');
    await ctx.engine.query('INCLUDE PERFETTO MODULE linux.memory.process;');
    await ctx.engine.query(CREATE_BREAKDOWN_TABLE_SQL);
    this.tablesInitialized = true;
  }

  // Helper to register a command that adds a memory counter track.
  private registerMemoryCommand(
    ctx: Trace,
    id: string,
    name: string,
    sqlValueExpr: string,
    titleSuffix: string,
  ) {
    ctx.commands.registerCommand({
      id,
      name,
      callback: async (pid?: string) => {
        if (pid === undefined) {
          pid = prompt('Enter a process pid', '') || '';
          if (!pid) return;
        }

        await this.setupTables(ctx);

        await addDebugCounterTrack({
          trace: ctx,
          data: {
            sqlSource: `
              SELECT
                ts,
                (${sqlValueExpr}) AS value
              FROM process_memory_breakdown
              WHERE pid = ${pid}
            `,
            columns: ['ts', 'value'],
          },
          title: `${pid}${titleSuffix}`,
        });
      },
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.registerMemoryCommand(
      ctx,
      'dev.perfetto.PixelMemory#ShowTotalMemory',
      'Add tracks: show a process total memory',
      'COALESCE(rss_and_swap, 0) + COALESCE(gpu_memory, 0)',
      '_rss_anon_file_swap_shmem_gpu',
    );

    this.registerMemoryCommand(
      ctx,
      'dev.perfetto.PixelMemory#ShowRssAnonShmemSwapGpuMemory',
      'Add tracks: show a process total memory (excluding file RSS)',
      'COALESCE(anon_rss_and_swap, 0) + COALESCE(shmem_rss, 0) + ' +
        'COALESCE(gpu_memory, 0)',
      '_rss_anon_shmem_swap_gpu',
    );
  }
}

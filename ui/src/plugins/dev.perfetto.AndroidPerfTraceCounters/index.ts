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

import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {addDebugSliceTrack} from '../../public/debug_tracks';
import {addQueryResultsTab} from '../../public/lib/query_table/query_result_tab';

const PERF_TRACE_COUNTERS_PRECONDITION = `
  SELECT
    str_value
  FROM metadata
  WHERE
    name = 'trace_config_pbtxt'
    AND str_value GLOB '*ftrace_events: "perf_trace_counters/sched_switch_with_ctrs"*'
`;

class AndroidPerfTraceCounters implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    const resp = await ctx.engine.query(PERF_TRACE_COUNTERS_PRECONDITION);
    if (resp.numRows() === 0) return;
    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidPerfTraceCounters#ThreadRuntimeIPC',
      name: 'Add a track to show a thread runtime ipc',
      callback: async (tid) => {
        if (tid === undefined) {
          tid = prompt('Enter a thread tid', '');
          if (tid === null) return;
        }
        const sqlPrefix = `
          WITH
            sched_switch_ipc AS (
              SELECT
                ts,
                EXTRACT_ARG(arg_set_id, 'prev_pid') AS tid,
                EXTRACT_ARG(arg_set_id, 'prev_comm') AS thread_name,
                EXTRACT_ARG(arg_set_id, 'inst') / (EXTRACT_ARG(arg_set_id, 'cyc') * 1.0) AS ipc,
                EXTRACT_ARG(arg_set_id, 'inst') AS instruction,
                EXTRACT_ARG(arg_set_id, 'cyc') AS cycle,
                EXTRACT_ARG(arg_set_id, 'stallbm') AS stall_backend_mem,
                EXTRACT_ARG(arg_set_id, 'l3dm') AS l3_cache_miss
              FROM ftrace_event
              WHERE name = 'sched_switch_with_ctrs' AND tid = ${tid}
            ),
            target_thread_sched_slice AS (
              SELECT s.*, t.tid, t.name FROM sched s LEFT JOIN thread t USING (utid)
                WHERE t.tid = ${tid}
            ),
            target_thread_ipc_slice AS (
              SELECT
                (
                  SELECT
                    ts
                  FROM target_thread_sched_slice ts
                  WHERE ts.tid = ssi.tid AND ts.ts < ssi.ts
                  ORDER BY ts.ts DESC
                  LIMIT 1
                ) AS ts,
                (
                  SELECT
                    dur
                  FROM target_thread_sched_slice ts
                  WHERE ts.tid = ssi.tid AND ts.ts < ssi.ts
                  ORDER BY ts.ts DESC
                  LIMIT 1
                ) AS dur,
                ssi.ipc,
                ssi.instruction,
                ssi.cycle,
                ssi.stall_backend_mem,
                ssi.l3_cache_miss
              FROM sched_switch_ipc ssi
            )
        `;

        await addDebugSliceTrack(
          ctx,
          {
            sqlSource:
              sqlPrefix +
              `
              SELECT * FROM target_thread_ipc_slice WHERE ts IS NOT NULL`,
          },
          'Rutime IPC:' + tid,
          {ts: 'ts', dur: 'dur', name: 'ipc'},
          ['instruction', 'cycle', 'stall_backend_mem', 'l3_cache_miss'],
        );
        addQueryResultsTab(ctx, {
          query:
            sqlPrefix +
            `
            SELECT
              (sum(instruction) * 1.0 / sum(cycle)*1.0) AS avg_ipc,
              sum(dur)/1e6 as total_runtime_ms,
              sum(instruction) AS total_instructions,
              sum(cycle) AS total_cycles,
              sum(stall_backend_mem) as total_stall_backend_mem,
              sum(l3_cache_miss) as total_l3_cache_miss
            FROM target_thread_ipc_slice WHERE ts IS NOT NULL`,
          title: 'target thread ipc statistic',
        });
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.AndroidPerfTraceCounters',
  plugin: AndroidPerfTraceCounters,
};

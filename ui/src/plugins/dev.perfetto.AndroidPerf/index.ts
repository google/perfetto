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

import {addDebugSliceTrack} from '../../public/debug_tracks';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {addQueryResultsTab} from '../../public/lib/query_table/query_result_tab';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AndroidPerf';
  async addAppProcessStartsDebugTrack(
    ctx: Trace,
    reason: string,
    sliceName: string,
  ): Promise<void> {
    const sliceColumns = [
      'id',
      'ts',
      'dur',
      'reason',
      'process_name',
      'intent',
      'table_name',
    ];
    await addDebugSliceTrack({
      trace: ctx,
      data: {
        sqlSource: `
                    SELECT
                      start_id AS id,
                      proc_start_ts AS ts,
                      total_dur AS dur,
                      reason,
                      process_name,
                      intent,
                      'slice' AS table_name
                    FROM android_app_process_starts
                    WHERE reason = '${reason}'
                 `,
        columns: sliceColumns,
      },
      title: 'app_' + sliceName + '_start reason: ' + reason,
      argColumns: sliceColumns,
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidPerf#BinderSystemServerIncoming',
      name: 'Run query: system_server incoming binder graph',
      callback: () =>
        addQueryResultsTab(ctx, {
          query: `INCLUDE PERFETTO MODULE android.binder;
           SELECT * FROM android_binder_incoming_graph((SELECT upid FROM process WHERE name = 'system_server'))`,
          title: 'system_server incoming binder graph',
        }),
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidPerf#BinderSystemServerOutgoing',
      name: 'Run query: system_server outgoing binder graph',
      callback: () =>
        addQueryResultsTab(ctx, {
          query: `INCLUDE PERFETTO MODULE android.binder;
           SELECT * FROM android_binder_outgoing_graph((SELECT upid FROM process WHERE name = 'system_server'))`,
          title: 'system_server outgoing binder graph',
        }),
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidPerf#MonitorContentionSystemServer',
      name: 'Run query: system_server monitor_contention graph',
      callback: () =>
        addQueryResultsTab(ctx, {
          query: `INCLUDE PERFETTO MODULE android.monitor_contention;
           SELECT * FROM android_monitor_contention_graph((SELECT upid FROM process WHERE name = 'system_server'))`,
          title: 'system_server monitor_contention graph',
        }),
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidPerf#BinderAll',
      name: 'Run query: all process binder graph',
      callback: () =>
        addQueryResultsTab(ctx, {
          query: `INCLUDE PERFETTO MODULE android.binder;
           SELECT * FROM android_binder_graph(-1000, 1000, -1000, 1000)`,
          title: 'all process binder graph',
        }),
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidPerf#ThreadClusterDistribution',
      name: 'Run query: runtime cluster distribution for a thread',
      callback: async (tid) => {
        if (tid === undefined) {
          tid = prompt('Enter a thread tid', '');
          if (tid === null) return;
        }
        addQueryResultsTab(ctx, {
          query: `
          INCLUDE PERFETTO MODULE android.cpu.cluster_type;
          WITH
            total_runtime AS (
              SELECT sum(dur) AS total_runtime
              FROM sched s
              LEFT JOIN thread t
                USING (utid)
              WHERE t.tid = ${tid}
            )
            SELECT
              c.cluster_type AS cluster, sum(dur)/1e6 AS total_dur_ms,
              sum(dur) * 1.0 / (SELECT * FROM total_runtime) AS percentage
            FROM sched s
            LEFT JOIN thread t
              USING (utid)
            LEFT JOIN android_cpu_cluster_mapping c
              USING (cpu)
            WHERE t.tid = ${tid}
            GROUP BY 1`,
          title: `runtime cluster distrubtion for tid ${tid}`,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidPerf#SchedLatency',
      name: 'Run query: top 50 sched latency for a thread',
      callback: async (tid) => {
        if (tid === undefined) {
          tid = prompt('Enter a thread tid', '');
          if (tid === null) return;
        }
        addQueryResultsTab(ctx, {
          query: `
          SELECT ts.*, t.tid, t.name, tt.id AS track_id
          FROM thread_state ts
          LEFT JOIN thread_track tt
           USING (utid)
          LEFT JOIN thread t
           USING (utid)
          WHERE ts.state IN ('R', 'R+') AND tid = ${tid}
           ORDER BY dur DESC
          LIMIT 50`,
          title: `top 50 sched latency slice for tid ${tid}`,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidPerf#SchedLatencyInSelectedWindow',
      name: 'Top 50 sched latency in selected time window',
      callback: async () => {
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        addQueryResultsTab(ctx, {
          title: 'top 50 sched latency slice in selcted time window',
          query: `SELECT
            ts.*,
            t.tid,
            t.name AS thread_name,
            tt.id AS track_id,
            p.name AS process_name
          FROM thread_state ts
          LEFT JOIN thread_track tt
           USING (utid)
          LEFT JOIN thread t
           USING (utid)
          LEFT JOIN process p
           USING (upid)
          WHERE ts.state IN ('R', 'R+')
           AND ts.ts >= ${window.start} and ts.ts < ${window.end}
          ORDER BY dur DESC
          LIMIT 50`,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidPerf#AppProcessStarts',
      name: 'Add tracks: app process starts',
      callback: async () => {
        await ctx.engine.query(
          `INCLUDE PERFETTO MODULE android.app_process_starts;`,
        );

        const startReason = ['activity', 'service', 'broadcast', 'provider'];
        for (const reason of startReason) {
          await this.addAppProcessStartsDebugTrack(ctx, reason, 'process_name');
        }
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidPerf#AppIntentStarts',
      name: 'Add tracks: app intent starts',
      callback: async () => {
        await ctx.engine.query(
          `INCLUDE PERFETTO MODULE android.app_process_starts;`,
        );

        const startReason = ['activity', 'service', 'broadcast'];
        for (const reason of startReason) {
          await this.addAppProcessStartsDebugTrack(ctx, reason, 'intent');
        }
      },
    });
  }
}

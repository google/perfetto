// Copyright (C) 2021 The Android Open Source Project
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
import {PerfettoPlugin} from '../../public/plugin';
import {getThreadOrProcUri} from '../../public/utils';
import {NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {
  Config as ProcessSchedulingTrackConfig,
  PROCESS_SCHEDULING_TRACK_KIND,
  ProcessSchedulingTrack,
} from './process_scheduling_track';
import {
  Config as ProcessSummaryTrackConfig,
  PROCESS_SUMMARY_TRACK,
  ProcessSummaryTrack,
} from './process_summary_track';
import ThreadPlugin from '../dev.perfetto.Thread';

// This plugin is responsible for adding summary tracks for process and thread
// groups.
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ProcessSummary';
  static readonly dependencies = [ThreadPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.addProcessTrackGroups(ctx);
    await this.addKernelThreadSummary(ctx);
  }

  private async addProcessTrackGroups(ctx: Trace): Promise<void> {
    const threads = ctx.plugins.getPlugin(ThreadPlugin).getThreadMap();

    const cpuCount = Math.max(...ctx.traceInfo.cpus, -1) + 1;

    const result = await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.process_metadata;

      select *
      from (
        select
          _process_available_info_summary.upid,
          null as utid,
          process.pid,
          null as tid,
          process.name as processName,
          null as threadName,
          sum_running_dur > 0 as hasSched,
          android_process_metadata.debuggable as isDebuggable,
          ifnull((
            select group_concat(string_value)
            from args
            where
              process.arg_set_id is not null and
              arg_set_id = process.arg_set_id and
              flat_key = 'chrome.process_label'
          ), '') as chromeProcessLabels
        from _process_available_info_summary
        join process using(upid)
        left join android_process_metadata using(upid)
      )
      union all
      select *
      from (
        select
          null,
          utid,
          null as pid,
          tid,
          null as processName,
          thread.name threadName,
          sum_running_dur > 0 as hasSched,
          0 as isDebuggable,
          '' as chromeProcessLabels
        from _thread_available_info_summary
        join thread using (utid)
        where upid is null
      )
  `);

    const it = result.iter({
      upid: NUM_NULL,
      utid: NUM_NULL,
      pid: NUM_NULL,
      tid: NUM_NULL,
      hasSched: NUM_NULL,
      isDebuggable: NUM_NULL,
      chromeProcessLabels: STR,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const utid = it.utid;
      const pid = it.pid;
      const tid = it.tid;
      const hasSched = Boolean(it.hasSched);
      const isDebuggable = Boolean(it.isDebuggable);
      const subtitle = it.chromeProcessLabels;

      // Group by upid if present else by utid.
      const pidForColor = pid ?? tid ?? upid ?? utid ?? 0;
      const uri = getThreadOrProcUri(upid, utid);

      const chips: string[] = [];
      isDebuggable && chips.push('debuggable');

      if (hasSched) {
        const config: ProcessSchedulingTrackConfig = {
          pidForColor,
          upid,
          utid,
        };

        ctx.tracks.registerTrack({
          uri,
          title: `${upid === null ? tid : pid} schedule`,
          tags: {
            kind: PROCESS_SCHEDULING_TRACK_KIND,
          },
          chips,
          track: new ProcessSchedulingTrack(ctx, config, cpuCount, threads),
          subtitle,
        });
      } else {
        const config: ProcessSummaryTrackConfig = {
          pidForColor,
          upid,
          utid,
        };

        ctx.tracks.registerTrack({
          uri,
          title: `${upid === null ? tid : pid} summary`,
          tags: {
            kind: PROCESS_SUMMARY_TRACK,
          },
          chips,
          track: new ProcessSummaryTrack(ctx.engine, config),
          subtitle,
        });
      }
    }
  }

  private async addKernelThreadSummary(ctx: Trace): Promise<void> {
    const {engine} = ctx;

    // Identify kernel threads if this is a linux system trace, and sufficient
    // process information is available. Kernel threads are identified by being
    // children of kthreadd (always pid 2).
    // The query will return the kthreadd process row first, which must exist
    // for any other kthreads to be returned by the query.
    // TODO(rsavitski): figure out how to handle the idle process (swapper),
    // which has pid 0 but appears as a distinct process (with its own comm) on
    // each cpu. It'd make sense to exclude its thread state track, but still
    // put process-scoped tracks in this group.
    const result = await engine.query(`
      select
        t.utid, p.upid, (case p.pid when 2 then 1 else 0 end) isKthreadd
      from
        thread t
        join process p using (upid)
        left join process parent on (p.parent_upid = parent.upid)
        join
          (select true from metadata m
             where (m.name = 'system_name' and m.str_value = 'Linux')
           union
           select 1 from (select true from sched limit 1))
      where
        p.pid = 2 or parent.pid = 2
      order by isKthreadd desc
    `);

    const it = result.iter({
      utid: NUM,
      upid: NUM,
    });

    // Not applying kernel thread grouping.
    if (!it.valid()) {
      return;
    }

    const config: ProcessSummaryTrackConfig = {
      pidForColor: 2,
      upid: it.upid,
      utid: it.utid,
    };

    ctx.tracks.registerTrack({
      uri: '/kernel',
      title: `Kernel thread summary`,
      tags: {
        kind: PROCESS_SUMMARY_TRACK,
      },
      track: new ProcessSummaryTrack(ctx.engine, config),
    });
  }
}

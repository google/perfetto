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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {getThreadOrProcUri} from '../../public/utils';
import {
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
} from '../../trace_processor/query_result';
import ThreadPlugin from '../dev.perfetto.Thread';
import {createPerfettoIndex} from '../../trace_processor/sql_utils';
import {uuidv4Sql} from '../../base/uuid';
import {
  Config,
  SLICE_TRACK_SUMMARY_KIND,
  GroupSummaryTrack,
} from './group_summary_track';

// This plugin is responsible for adding summary tracks for process and thread
// groups.
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ProcessSummary';
  static readonly dependencies = [ThreadPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.addProcessTrackGroups(ctx);
  }

  private async addProcessTrackGroups(ctx: Trace): Promise<void> {
    // Makes the queries in `SliceTrackSummary` significantly faster when using
    // scheduling data.
    // TODO(lalitm): figure out a better way to do this without hardcoding this
    // here.
    await createPerfettoIndex({
      engine: ctx.engine,
      name: `__process_scheduling_${uuidv4Sql()}`,
      on: `__intrinsic_sched_slice(utid)`,
    });
    // Makes the queries in `SliceTrackSummary` significantly faster when using
    // slice data.
    // TODO(lalitm): figure out a better way to do this without hardcoding this
    // here.
    await createPerfettoIndex({
      engine: ctx.engine,
      name: `__slice_track_summary_${uuidv4Sql()}`,
      on: `__intrinsic_slice(track_id)`,
    });

    const threads = ctx.plugins.getPlugin(ThreadPlugin).getThreadMap();
    const result = await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.process_metadata;

      WITH machine_cpu_counts AS (
        SELECT
          machine_id AS machine,
          COUNT(*) AS cpu_count
        FROM cpu
        GROUP BY machine
      )
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
          case
            when process.name = 'system_server' then
              ifnull(extract_metadata_for_machine(machine_id, 'android_profile_system_server'), 0)
            when process.name GLOB 'zygote*' then
              ifnull(extract_metadata_for_machine(machine_id, 'android_profile_boot_classpath'), 0)
            else 0
          end as isBootImageProfiling,
          ifnull((
            select group_concat(string_value)
            from args
            where
              process.arg_set_id is not null and
              arg_set_id = process.arg_set_id and
              flat_key = 'chrome.process_label'
          ), '') as chromeProcessLabels,
          machine_id as machine,
          IFNULL(machine_cpu_counts.cpu_count, 0) AS cpuCount
        from _process_available_info_summary
        join process using(upid)
        left join android_process_metadata using(upid)
        LEFT JOIN machine_cpu_counts
          ON machine_cpu_counts.machine = machine_id
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
          0 as isBootImageProfiling,
          '' as chromeProcessLabels,
          machine_id as machine,
          IFNULL(machine_cpu_counts.cpu_count, 0) AS cpuCount
        from _thread_available_info_summary
        join thread using (utid)
        LEFT JOIN machine_cpu_counts
          ON machine_cpu_counts.machine = machine_id
        where upid is null
      )
    `);
    const it = result.iter({
      upid: NUM_NULL,
      utid: NUM_NULL,
      pid: LONG_NULL,
      tid: LONG_NULL,
      hasSched: NUM_NULL,
      isDebuggable: NUM_NULL,
      isBootImageProfiling: NUM_NULL,
      chromeProcessLabels: STR,
      machine: NUM,
      cpuCount: NUM,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const utid = it.utid;
      const pid = it.pid;
      const tid = it.tid;
      const hasSched = Boolean(it.hasSched);
      const isDebuggable = Boolean(it.isDebuggable);
      const isBootImageProfiling = Boolean(it.isBootImageProfiling);
      const subtitle = it.chromeProcessLabels;
      const cpuCount = it.cpuCount;

      // Group by upid if present else by utid.
      const pidForColor = pid ?? tid ?? upid ?? utid ?? 0;
      const uri = getThreadOrProcUri(upid, utid);

      const chips: string[] = [];
      isDebuggable && chips.push('debuggable');

      // When boot image profiling is enabled for the bootclasspath or system
      // server, performance characteristics of the device can vary wildly.
      // Surface that detail in the process tracks for zygote and system_server
      // to make it clear to the user.
      // See https://source.android.com/docs/core/runtime/boot-image-profiles
      // for additional details.
      isBootImageProfiling && chips.push('boot image profiling');

      const config: Config = {
        pidForColor,
        upid,
        utid,
      };
      const track = new GroupSummaryTrack(
        ctx,
        config,
        cpuCount,
        threads,
        hasSched,
      );
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [SLICE_TRACK_SUMMARY_KIND],
        },
        chips,
        renderer: track,
        subtitle,
      });
    }
  }
}

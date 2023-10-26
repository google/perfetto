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

import {v4 as uuidv4} from 'uuid';

import {
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../common/query_result';
import {TrackWithControllerAdapter} from '../../common/track_adapter';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';

import {
  Config as ProcessSchedulingTrackConfig,
  Data as ProcessSchedulingTrackData,
  PROCESS_SCHEDULING_TRACK_KIND,
  ProcessSchedulingTrack,
  ProcessSchedulingTrackController,
} from './process_scheduling_track';
import {
  Config as ProcessSummaryTrackConfig,
  Data as ProcessSummaryTrackData,
  PROCESS_SUMMARY_TRACK,
  ProcessSummaryTrack,
  ProcessSummaryTrackController,
} from './process_summary_track';

// This plugin now manages both process "scheduling" and "summary" tracks.
class ProcessSummaryPlugin implements Plugin {
  private upidToUuid = new Map<number, string>();
  private utidToUuid = new Map<number, string>();

  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    await this.addProcessTrackGroups(ctx);
    await this.addKernelThreadSummary(ctx);
  }

  private async addProcessTrackGroups(ctx: PluginContextTrace): Promise<void> {
    this.upidToUuid.clear();
    this.utidToUuid.clear();

    // We want to create groups of tracks in a specific order.
    // The tracks should be grouped:
    //    by upid
    //    or (if upid is null) by utid
    // the groups should be sorted by:
    //  Chrome-based process rank based on process names (e.g. Browser)
    //  has a heap profile or not
    //  total cpu time *for the whole parent process*
    //  process name
    //  upid
    //  thread name
    //  utid
    const result = await ctx.engine.query(`
    select
      the_tracks.upid,
      the_tracks.utid,
      total_dur as hasSched,
      hasHeapProfiles,
      process.pid as pid,
      thread.tid as tid,
      process.name as processName,
      thread.name as threadName,
      package_list.debuggable as isDebuggable,
      ifnull((
        select group_concat(string_value)
        from args
        where
          process.arg_set_id is not null and
          arg_set_id = process.arg_set_id and
          flat_key = 'chrome.process_label'
      ), '') AS chromeProcessLabels,
      (case process.name
         when 'Browser' then 3
         when 'Gpu' then 2
         when 'Renderer' then 1
         else 0
      end) as chromeProcessRank
    from (
      select upid, 0 as utid from process_track
      union
      select upid, 0 as utid from process_counter_track
      union
      select upid, utid from thread_counter_track join thread using(utid)
      union
      select upid, utid from thread_track join thread using(utid)
      union
      select upid, utid from sched join thread using(utid) group by utid
      union
      select upid, 0 as utid from (
        select distinct upid
        from perf_sample join thread using (utid) join process using (upid)
        where callsite_id is not null)
      union
      select upid, utid from (
        select distinct(utid) from cpu_profile_stack_sample
      ) join thread using(utid)
      union
      select distinct(upid) as upid, 0 as utid from heap_profile_allocation
      union
      select distinct(upid) as upid, 0 as utid from heap_graph_object
    ) the_tracks
    left join (
      select upid, sum(thread_total_dur) as total_dur
      from (
        select utid, sum(dur) as thread_total_dur
        from sched where dur != -1 and utid != 0
        group by utid
      )
      join thread using (utid)
      group by upid
    ) using(upid)
    left join (
      select
        distinct(upid) as upid,
        true as hasHeapProfiles
      from heap_profile_allocation
      union
      select
        distinct(upid) as upid,
        true as hasHeapProfiles
      from heap_graph_object
    ) using (upid)
    left join (
      select
        thread.upid as upid,
        sum(cnt) as perfSampleCount
      from (
          select utid, count(*) as cnt
          from perf_sample where callsite_id is not null
          group by utid
      ) join thread using (utid)
      group by thread.upid
    ) using (upid)
    left join (
      select
        process.upid as upid,
        sum(cnt) as sliceCount
      from (select track_id, count(*) as cnt from slice group by track_id)
        left join thread_track on track_id = thread_track.id
        left join thread on thread_track.utid = thread.utid
        left join process_track on track_id = process_track.id
        join process on process.upid = thread.upid
          or process_track.upid = process.upid
      where process.upid is not null
      group by process.upid
    ) using (upid)
    left join thread using(utid)
    left join process using(upid)
    left join package_list using(uid)
    order by
      chromeProcessRank desc,
      hasHeapProfiles desc,
      perfSampleCount desc,
      total_dur desc,
      sliceCount desc,
      processName asc nulls last,
      the_tracks.upid asc nulls last,
      threadName asc nulls last,
      the_tracks.utid asc nulls last;
  `);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      pid: NUM_NULL,
      threadName: STR_NULL,
      processName: STR_NULL,
      hasSched: NUM_NULL,
      hasHeapProfiles: NUM_NULL,
      isDebuggable: NUM_NULL,
      chromeProcessLabels: STR,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const upid = it.upid;
      const pid = it.pid;
      const hasSched = !!it.hasSched;
      const isDebuggable = !!it.isDebuggable;

      // Group by upid if present else by utid.
      let pUuid =
          upid === null ? this.utidToUuid.get(utid) : this.upidToUuid.get(upid);
      // These should only happen once for each track group.
      if (pUuid === undefined) {
        pUuid = this.getOrCreateUuid(utid, upid);
        const pidForColor = pid || tid || upid || utid || 0;
        const type = hasSched ? 'schedule' : 'summary';
        const uri = `perfetto.ProcessScheduling#${upid}.${utid}.${type}`;

        if (hasSched) {
          const config: ProcessSchedulingTrackConfig = {
            pidForColor,
            upid,
            utid,
          };

          ctx.registerStaticTrack({
            uri,
            displayName: `${upid === null ? tid : pid} schedule`,
            kind: PROCESS_SCHEDULING_TRACK_KIND,
            tags: {
              isDebuggable,
            },
            track: ({trackKey}) => {
              return new TrackWithControllerAdapter<
                  ProcessSchedulingTrackConfig,
                  ProcessSchedulingTrackData>(
                  ctx.engine,
                  trackKey,
                  config,
                  ProcessSchedulingTrack,
                  ProcessSchedulingTrackController);
            },
          });
        } else {
          const config: ProcessSummaryTrackConfig = {
            pidForColor,
            upid,
            utid,
          };

          ctx.registerStaticTrack({
            uri,
            displayName: `${upid === null ? tid : pid} summary`,
            kind: PROCESS_SUMMARY_TRACK,
            tags: {
              isDebuggable,
            },
            track: ({trackKey}) => {
              return new TrackWithControllerAdapter<
                  ProcessSummaryTrackConfig,
                  ProcessSummaryTrackData>(
                  ctx.engine,
                  trackKey,
                  config,
                  ProcessSummaryTrack,
                  ProcessSummaryTrackController);
            },
          });
        }
      }
    }
  }

  private async addKernelThreadSummary(ctx: PluginContextTrace): Promise<void> {
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

    ctx.registerStaticTrack({
      uri: 'perfetto.ProcessSummary#kernel',
      displayName: `Kernel thread summary`,
      kind: PROCESS_SUMMARY_TRACK,
      track: ({trackKey}) => {
        return new TrackWithControllerAdapter<
            ProcessSummaryTrackConfig,
            ProcessSummaryTrackData>(
            ctx.engine,
            trackKey,
            config,
            ProcessSummaryTrack,
            ProcessSummaryTrackController);
      },
    });
  }

  private getOrCreateUuid(utid: number, upid: number|null) {
    let uuid = this.getUuidUnchecked(utid, upid);
    if (uuid === undefined) {
      uuid = uuidv4();
      if (upid === null) {
        this.utidToUuid.set(utid, uuid);
      } else {
        this.upidToUuid.set(upid, uuid);
      }
    }
    return uuid;
  }

  getUuidUnchecked(utid: number, upid: number|null) {
    return upid === null ? this.utidToUuid.get(utid) :
                           this.upidToUuid.get(upid);
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.ProcessSummary',
  plugin: ProcessSummaryPlugin,
};

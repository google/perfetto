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

import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {
  getOrCreateGroupForProcess,
  getOrCreateGroupForThread,
} from '../../public/standard_groups';
import {TrackNode} from '../../public/workspace';
import {NUM, STR, STR_NULL} from '../../trace_processor/query_result';

function stripPathFromExecutable(path: string) {
  if (path[0] === '/') {
    return path.split('/').slice(-1)[0];
  } else {
    return path;
  }
}

function getThreadDisplayName(threadName: string | undefined, tid: number) {
  if (threadName) {
    return `${stripPathFromExecutable(threadName)} ${tid}`;
  } else {
    return `Thread ${tid}`;
  }
}

// This plugin is responsible for organizing all process and thread groups
// including the kernel groups, sorting, and adding summary tracks.
class ProcessThreadGroupsPlugin implements PerfettoPlugin {
  private readonly processGroups = new Map<number, TrackNode>();
  private readonly threadGroups = new Map<number, TrackNode>();

  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.addProcessAndThreadGroups(ctx);
  }

  private async addProcessAndThreadGroups(ctx: Trace): Promise<void> {
    this.processGroups.clear();
    this.threadGroups.clear();

    // Pre-group all kernel "threads" (actually processes) if this is a linux
    // system trace. Below, addProcessTrackGroups will skip them due to an
    // existing group uuid, and addThreadStateTracks will fill in the
    // per-thread tracks. Quirk: since all threads will appear to be
    // TrackKindPriority.MAIN_THREAD, any process-level tracks will end up
    // pushed to the bottom of the group in the UI.
    await this.addKernelThreadGrouping(ctx);

    // Create the per-process track groups. Note that this won't necessarily
    // create a track per process. If a process has been completely idle and has
    // no sched events, no track group will be emitted.
    // Will populate this.addTrackGroupActions
    await this.addProcessGroups(ctx);
    await this.addThreadGroups(ctx);
  }

  private async addKernelThreadGrouping(ctx: Trace): Promise<void> {
    // Identify kernel threads if this is a linux system trace, and sufficient
    // process information is available. Kernel threads are identified by being
    // children of kthreadd (always pid 2).
    // The query will return the kthreadd process row first, which must exist
    // for any other kthreads to be returned by the query.
    // TODO(rsavitski): figure out how to handle the idle process (swapper),
    // which has pid 0 but appears as a distinct process (with its own comm) on
    // each cpu. It'd make sense to exclude its thread state track, but still
    // put process-scoped tracks in this group.
    const result = await ctx.engine.query(`
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

    // Create the track group. Use kthreadd's PROCESS_SUMMARY_TRACK for the
    // main track. It doesn't summarise the kernel threads within the group,
    // but creating a dedicated track type is out of scope at the time of
    // writing.
    const kernelThreadsGroup = new TrackNode({
      title: 'Kernel threads',
      uri: '/kernel',
      sortOrder: 50,
      isSummary: true,
    });
    ctx.workspace.addChildInOrder(kernelThreadsGroup);

    // Set the group for all kernel threads (including kthreadd itself).
    for (; it.valid(); it.next()) {
      const {utid} = it;

      const threadGroup = getOrCreateGroupForThread(ctx.workspace, utid);
      threadGroup.headless = true;
      kernelThreadsGroup.addChildInOrder(threadGroup);

      this.threadGroups.set(utid, threadGroup);
    }
  }

  // Adds top level groups for processes and thread that don't belong to a
  // process.
  private async addProcessGroups(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      with processGroups as (
        select
          upid,
          process.pid as pid,
          process.name as processName,
          sum_running_dur as sumRunningDur,
          thread_slice_count + process_slice_count as sliceCount,
          perf_sample_count as perfSampleCount,
          allocation_count as heapProfileAllocationCount,
          graph_object_count as heapGraphObjectCount,
          (
            select group_concat(string_value)
            from args
            where
              process.arg_set_id is not null and
              arg_set_id = process.arg_set_id and
              flat_key = 'chrome.process_label'
          ) chromeProcessLabels,
          case process.name
            when 'Browser' then 3
            when 'Gpu' then 2
            when 'Renderer' then 1
            else 0
          end as chromeProcessRank
        from _process_available_info_summary
        join process using(upid)
      ),
      threadGroups as (
        select
          utid,
          tid,
          thread.name as threadName,
          sum_running_dur as sumRunningDur,
          slice_count as sliceCount,
          perf_sample_count as perfSampleCount
        from _thread_available_info_summary
        join thread using (utid)
        where upid is null
      )
      select *
      from (
        select
          'process' as kind,
          upid as uid,
          pid as id,
          processName as name
        from processGroups
        order by
          chromeProcessRank desc,
          heapProfileAllocationCount desc,
          heapGraphObjectCount desc,
          perfSampleCount desc,
          sumRunningDur desc,
          sliceCount desc,
          processName asc,
          upid asc
      )
      union all
      select *
      from (
        select
          'thread' as kind,
          utid as uid,
          tid as id,
          threadName as name
        from threadGroups
        order by
          perfSampleCount desc,
          sumRunningDur desc,
          sliceCount desc,
          threadName asc,
          utid asc
      )
  `);

    const it = result.iter({
      kind: STR,
      uid: NUM,
      id: NUM,
      name: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const {kind, uid, id, name} = it;

      if (kind === 'process') {
        // Ignore kernel process groups
        if (this.processGroups.has(uid)) {
          continue;
        }

        function getProcessDisplayName(
          processName: string | undefined,
          pid: number,
        ) {
          if (processName) {
            return `${stripPathFromExecutable(processName)} ${pid}`;
          } else {
            return `Process ${pid}`;
          }
        }

        const displayName = getProcessDisplayName(name ?? undefined, id);
        const group = getOrCreateGroupForProcess(ctx.workspace, uid);
        group.title = displayName;
        group.uri = `/process_${uid}`; // Summary track URI
        group.sortOrder = 50;

        // Re-insert the child node to sort it
        ctx.workspace.addChildInOrder(group);
        this.processGroups.set(uid, group);
      } else {
        // Ignore kernel process groups
        if (this.threadGroups.has(uid)) {
          continue;
        }

        const displayName = getThreadDisplayName(name ?? undefined, id);
        const group = getOrCreateGroupForThread(ctx.workspace, uid);
        group.title = displayName;
        group.uri = `/thread_${uid}`; // Summary track URI
        group.sortOrder = 50;

        // Re-insert the child node to sort it
        ctx.workspace.addChildInOrder(group);
        this.threadGroups.set(uid, group);
      }
    }
  }

  // Create all the nested & headless thread groups that live inside existing
  // process groups.
  private async addThreadGroups(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      with threadGroups as (
        select
          utid,
          upid,
          tid,
          thread.name as threadName,
          CASE
            WHEN thread.is_main_thread = 1 THEN 10
            WHEN thread.name = 'CrBrowserMain' THEN 10
            WHEN thread.name = 'CrRendererMain' THEN 10
            WHEN thread.name = 'CrGpuMain' THEN 10
            WHEN thread.name glob '*RenderThread*' THEN 9
            WHEN thread.name glob '*GPU completion*' THEN 8
            WHEN thread.name = 'Chrome_ChildIOThread' THEN 7
            WHEN thread.name = 'Chrome_IOThread' THEN 7
            WHEN thread.name = 'Compositor' THEN 6
            WHEN thread.name = 'VizCompositorThread' THEN 6
            ELSE 5
          END as priority
        from _thread_available_info_summary
        join thread using (utid)
        where upid is not null
      )
      select *
      from (
        select
          utid,
          upid,
          tid,
          threadName
        from threadGroups
        order by
          priority desc,
          tid asc
      )
  `);

    const it = result.iter({
      utid: NUM,
      tid: NUM,
      upid: NUM,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const {utid, tid, upid, threadName} = it;

      // Ignore kernel thread groups
      if (this.threadGroups.has(utid)) {
        continue;
      }

      const group = getOrCreateGroupForThread(ctx.workspace, utid);
      group.title = getThreadDisplayName(threadName ?? undefined, tid);
      this.threadGroups.set(utid, group);
      group.headless = true;
      this.processGroups.get(upid)?.addChildInOrder(group);
    }
  }

  async onTraceReady(_: Trace): Promise<void> {
    // If, by the time the trace has finished loading, some of the process or
    // thread group tracks nodes have no children, just remove them.
    const removeIfEmpty = (g: TrackNode) => {
      if (!g.hasChildren) {
        g.remove();
      }
    };
    this.processGroups.forEach(removeIfEmpty);
    this.threadGroups.forEach(removeIfEmpty);
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.ProcessThreadGroups',
  plugin: ProcessThreadGroupsPlugin,
};

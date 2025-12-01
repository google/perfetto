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
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {maybeMachineLabel} from '../../public/utils';

function stripPathFromExecutable(path: string) {
  if (path[0] === '/') {
    return path.split('/').slice(-1)[0];
  } else {
    return path;
  }
}

function getThreadDisplayName(
  threadName: string | undefined,
  tid: bigint | number,
) {
  if (threadName) {
    return `${stripPathFromExecutable(threadName)} ${tid}`;
  } else {
    return `Thread ${tid}`;
  }
}

// This plugin is responsible for organizing all process and thread groups
// (including kernel threads), sorting, and adding summary tracks.
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ProcessThreadGroups';

  private readonly processGroups = new Map<number, TrackNode>();
  private readonly threadGroups = new Map<number, TrackNode>();

  constructor(private readonly ctx: Trace) {}

  getGroupForProcess(upid: number): TrackNode | undefined {
    return this.processGroups.get(upid);
  }

  getGroupForThread(utid: number): TrackNode | undefined {
    return this.threadGroups.get(utid);
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    // Pre-group all kernel "threads" (actually processes) if this is a linux
    // system trace. Below, addProcessTrackGroups will skip them due to an
    // existing group uuid, and addThreadStateTracks will fill in the
    // per-thread tracks. Quirk: since all threads will appear to be
    // TrackKindPriority.MAIN_THREAD, any process-level tracks will end up
    // pushed to the bottom of the group in the UI.
    await this.addKernelThreadGrouping();

    // Create the per-process track groups. Note that this won't necessarily
    // create a track per process. If a process has been completely idle and has
    // no sched events, no track group will be emitted.
    // Will populate this.addTrackGroupActions
    await this.addProcessGroups();
    await this.addThreadGroups();

    ctx.onTraceReady.addListener(() => {
      // If, by the time the trace has finished loading, some of the process or
      // thread group tracks nodes have no children, just remove them.
      const removeIfEmpty = (g: TrackNode) => {
        if (!g.hasChildren) {
          g.remove();
        }
      };
      this.processGroups.forEach(removeIfEmpty);
      this.threadGroups.forEach(removeIfEmpty);
    });
  }

  private async addKernelThreadGrouping(): Promise<void> {
    // Identify kernel threads if this is a linux system trace, and sufficient
    // process information is available. Kernel threads are identified by being
    // children of kthreadd (always pid 2).
    // TODO(rsavitski): figure out how to handle the idle process (swapper),
    // which has pid 0 but appears as a distinct process (with its own comm) on
    // each cpu. It'd make sense to exclude its thread state track, but still
    // put process-scoped tracks in this group.
    const result = await this.ctx.engine.query(`
       include perfetto module viz.threads;

       select utid, upid
       from _threads_with_kernel_flag
       where is_kernel_thread
    `);

    const it = result.iter({
      utid: NUM,
      upid: NUM,
    });
    if (!it.valid()) {
      return; // no kernel thread grouping
    }

    const kernelThreadsGroup = new TrackNode({
      name: 'Kernel threads',
      uri: '/kernel',
      sortOrder: 50,
      isSummary: true,
    });
    this.ctx.defaultWorkspace.addChildInOrder(kernelThreadsGroup);

    // Set the group for all kernel threads (including kthreadd itself).
    for (; it.valid(); it.next()) {
      const {utid, upid} = it;

      const threadGroup = new TrackNode({
        uri: `thread${utid}`,
        name: `Thread ${utid}`,
        isSummary: true,
        headless: true,
      });
      kernelThreadsGroup.addChildInOrder(threadGroup);
      this.processGroups.set(upid, threadGroup);
      this.threadGroups.set(utid, threadGroup);
    }
  }

  // Adds top level groups for processes and thread that don't belong to a
  // process.
  private async addProcessGroups(): Promise<void> {
    const result = await this.ctx.engine.query(`
      with processGroups as (
        select
          upid,
          process.pid as pid,
          process.name as processName,
          sum_running_dur as sumRunningDur,
          thread_slice_count + process_slice_count as sliceCount,
          perf_sample_count as perfSampleCount,
          instruments_sample_count as instrumentsSampleCount,
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
          ifnull(extract_arg(process.arg_set_id, 'process_sort_index_hint'), 0) as processSortIndexHint,
          case process.name
            when 'Browser' then 3
            when 'Gpu' then 2
            when 'Renderer' then 1
            else 0
          end as chromeProcessRank,
          ifnull(machine_id, 0) as machine
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
          perf_sample_count as perfSampleCount,
          instruments_sample_count as instrumentsSampleCount,
          ifnull(extract_arg(thread.arg_set_id, 'thread_sort_index_hint'), 0) as threadSortIndexHint,
          ifnull(machine_id, 0) as machine
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
          processName as name,
          machine
        from processGroups
        order by
          processSortIndexHint asc,
          chromeProcessRank desc,
          heapProfileAllocationCount desc,
          heapGraphObjectCount desc,
          perfSampleCount desc,
          instrumentsSampleCount desc,
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
          threadName as name,
          machine
        from threadGroups
        order by
          threadSortIndexHint asc,
          perfSampleCount desc,
          instrumentsSampleCount desc,
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
      machine: NUM,
    });
    for (; it.valid(); it.next()) {
      const {kind, uid, id, name} = it;

      if (kind === 'process') {
        // Skip pre-grouped kthread tracks.
        if (this.processGroups.has(uid)) {
          continue;
        }

        const machineLabel = maybeMachineLabel(it.machine);
        function getProcessDisplayName(
          processName: string | undefined,
          pid: number,
        ) {
          if (processName) {
            return `${stripPathFromExecutable(processName)} ${pid}${
              machineLabel
            }`;
          } else {
            return `Process ${pid}${machineLabel}`;
          }
        }

        const displayName = getProcessDisplayName(name ?? undefined, id);
        const group = new TrackNode({
          uri: `/process_${uid}`,
          name: displayName,
          isSummary: true,
          sortOrder: 50,
        });

        // Re-insert the child node to sort it
        this.ctx.defaultWorkspace.addChildInOrder(group);
        this.processGroups.set(uid, group);
      } else {
        // Skip pre-grouped kthread tracks.
        if (this.threadGroups.has(uid)) {
          continue;
        }

        const displayName = getThreadDisplayName(name ?? undefined, id);
        const group = new TrackNode({
          uri: `/thread_${uid}`,
          name: displayName,
          isSummary: true,
          sortOrder: 50,
        });

        // Re-insert the child node to sort it
        this.ctx.defaultWorkspace.addChildInOrder(group);
        this.threadGroups.set(uid, group);
      }
    }
  }

  // Create all the nested & headless thread groups that live inside existing
  // process groups.
  private async addThreadGroups(): Promise<void> {
    const result = await this.ctx.engine.query(`
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
      tid: LONG,
      upid: NUM,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const {utid, tid, upid, threadName} = it;

      // Skip pre-grouped kthread tracks.
      if (this.threadGroups.has(utid)) {
        continue;
      }

      const group = new TrackNode({
        uri: `/thread_${utid}`,
        name: getThreadDisplayName(threadName ?? undefined, tid),
        isSummary: true,
        headless: true,
      });
      this.threadGroups.set(utid, group);
      this.processGroups.get(upid)?.addChildInOrder(group);
    }
  }
}

// Copyright (C) 2020 The Android Open Source Project
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

import {assertExists} from '../base/logging';
import {globals} from '../frontend/globals';
import {TrackDescriptor} from '../public';
import {Engine, EngineBase} from '../trace_processor/engine';
import {NUM, STR, STR_NULL} from '../trace_processor/query_result';
import {
  ACTUAL_FRAMES_SLICE_TRACK_KIND,
  ASYNC_SLICE_TRACK_KIND,
  CHROME_EVENT_LATENCY_TRACK_KIND,
  CHROME_SCROLL_JANK_TRACK_KIND,
  CHROME_TOPLEVEL_SCROLLS_KIND,
  COUNTER_TRACK_KIND,
  CPU_FREQ_TRACK_KIND,
  CPU_PROFILE_TRACK_KIND,
  CPU_SLICE_TRACK_KIND,
  EXPECTED_FRAMES_SLICE_TRACK_KIND,
  HEAP_PROFILE_TRACK_KIND,
  PERF_SAMPLES_PROFILE_TRACK_KIND,
  SCROLL_JANK_V3_TRACK_KIND,
  THREAD_SLICE_TRACK_KIND,
  THREAD_STATE_TRACK_KIND,
} from '../core/track_kinds';
import {exists, Optional} from '../base/utils';
import {GroupNode, ContainerNode, TrackNode} from '../frontend/workspace';

const MEM_DMA_COUNTER_NAME = 'mem.dma_heap';
const MEM_DMA = 'mem.dma_buffer';
const MEM_ION = 'mem.ion';
const F2FS_IOSTAT_TAG = 'f2fs_iostat.';
const F2FS_IOSTAT_GROUP_NAME = 'f2fs_iostat';
const F2FS_IOSTAT_LAT_TAG = 'f2fs_iostat_latency.';
const F2FS_IOSTAT_LAT_GROUP_NAME = 'f2fs_iostat_latency';
const DISK_IOSTAT_TAG = 'diskstat.';
const DISK_IOSTAT_GROUP_NAME = 'diskstat';
const BUDDY_INFO_TAG = 'mem.buddyinfo';
const UFS_CMD_TAG_REGEX = new RegExp('^io.ufs.command.tag.*$');
const UFS_CMD_TAG_GROUP = 'io.ufs.command.tags';
// NB: Userspace wakelocks start with "WakeLock" not "Wakelock".
const KERNEL_WAKELOCK_REGEX = new RegExp('^Wakelock.*$');
const KERNEL_WAKELOCK_GROUP = 'Kernel wakelocks';
const NETWORK_TRACK_REGEX = new RegExp('^.* (Received|Transmitted)( KB)?$');
const NETWORK_TRACK_GROUP = 'Networking';
const ENTITY_RESIDENCY_REGEX = new RegExp('^Entity residency:');
const ENTITY_RESIDENCY_GROUP = 'Entity residency';
const UCLAMP_REGEX = new RegExp('^UCLAMP_');
const UCLAMP_GROUP = 'Scheduler Utilization Clamping';
const POWER_RAILS_GROUP = 'Power Rails';
const POWER_RAILS_REGEX = new RegExp('^power.');
const FREQUENCY_GROUP = 'Frequency Scaling';
const TEMPERATURE_REGEX = new RegExp('^.* Temperature$');
const TEMPERATURE_GROUP = 'Temperature';
const IRQ_GROUP = 'IRQs';
const IRQ_REGEX = new RegExp('^(Irq|SoftIrq) Cpu.*');
const CHROME_TRACK_REGEX = new RegExp('^Chrome.*|^InputLatency::.*');
const CHROME_TRACK_GROUP = 'Chrome Global Tracks';
const MISC_GROUP = 'Misc Global Tracks';

export async function decideTracks(engine: EngineBase): Promise<void> {
  await new TrackDecider(engine).decideTracks();
}

class TrackDecider {
  private engine: EngineBase;
  private threadGroups = new Map<number, GroupNode>();
  private processGroups = new Map<number, GroupNode>();

  constructor(engine: EngineBase) {
    this.engine = engine;
  }

  private groupGlobalIonTracks(): void {
    const ionTracks: TrackNode[] = [];
    let hasSummary = false;

    for (const track of globals.workspace.children) {
      if (!(track instanceof TrackNode)) continue;

      const isIon = track.displayName.startsWith(MEM_ION);
      const isIonCounter = track.displayName === MEM_ION;
      const isDmaHeapCounter = track.displayName === MEM_DMA_COUNTER_NAME;
      const isDmaBuffferSlices = track.displayName === MEM_DMA;
      if (isIon || isIonCounter || isDmaHeapCounter || isDmaBuffferSlices) {
        ionTracks.push(track);
      }
      hasSummary = hasSummary || isIonCounter;
      hasSummary = hasSummary || isDmaHeapCounter;
    }

    if (ionTracks.length === 0 || !hasSummary) {
      return;
    }

    let group: Optional<GroupNode>;
    for (const track of ionTracks) {
      if (!group && [MEM_DMA_COUNTER_NAME, MEM_ION].includes(track.uri)) {
        globals.workspace.removeChild(track);
        group = new GroupNode(track.displayName);
        group.headerTrackUri = track.uri;
        globals.workspace.addChild(group);
      } else {
        group?.addChild(track);
      }
    }
  }

  private groupGlobalIostatTracks(tag: string, groupName: string): void {
    const devMap = new Map<string, GroupNode>();

    for (const track of globals.workspace.children) {
      if (track instanceof TrackNode && track.displayName.startsWith(tag)) {
        const name = track.displayName.split('.', 3);
        const key = name[1];

        let parentGroup = devMap.get(key);
        if (!parentGroup) {
          const group = new GroupNode(groupName);
          globals.workspace.addChild(group);
          devMap.set(key, group);
          parentGroup = group;
        }

        track.displayName = name[2];
        parentGroup.addChild(track);
      }
    }
  }

  private groupGlobalBuddyInfoTracks(): void {
    const devMap = new Map<string, GroupNode>();

    for (const track of globals.workspace.children) {
      if (
        track instanceof TrackNode &&
        track.displayName.startsWith(BUDDY_INFO_TAG)
      ) {
        const tokens = track.uri.split('[');
        const node = tokens[1].slice(0, -1);
        const zone = tokens[2].slice(0, -1);
        const size = tokens[3].slice(0, -1);

        const groupName = 'Buddyinfo:  Node: ' + node + ' Zone: ' + zone;
        if (!devMap.has(groupName)) {
          const group = new GroupNode(groupName);
          devMap.set(groupName, group);
          globals.workspace.addChild(group);
        }
        track.displayName = 'Chunk size: ' + size;
        const group = devMap.get(groupName)!;
        group.addChild(track);
      }
    }
  }

  private groupFrequencyTracks(groupName: string): void {
    const group = new GroupNode(groupName);

    for (const track of globals.workspace.children) {
      if (!(track instanceof TrackNode)) continue;
      // Group all the frequency tracks together (except the CPU and GPU
      // frequency ones).
      if (
        track.displayName.endsWith('Frequency') &&
        !track.displayName.startsWith('Cpu') &&
        !track.displayName.startsWith('Gpu')
      ) {
        group.addChild(track);
      }
    }

    if (group.children.length > 0) {
      globals.workspace.addChild(group);
    }
  }

  private groupMiscNonAllowlistedTracks(groupName: string): void {
    // List of allowlisted track names.
    const ALLOWLIST_REGEXES = [
      new RegExp('^Cpu .*$', 'i'),
      new RegExp('^Gpu .*$', 'i'),
      new RegExp('^Trace Triggers$'),
      new RegExp('^Android App Startups$'),
      new RegExp('^Device State.*$'),
      new RegExp('^Android logs$'),
    ];

    const group = new GroupNode(groupName);
    for (const track of globals.workspace.children) {
      if (!(track instanceof TrackNode)) continue;
      let allowlisted = false;
      for (const regex of ALLOWLIST_REGEXES) {
        allowlisted = allowlisted || regex.test(track.displayName);
      }
      if (allowlisted) {
        continue;
      }
      group.addChild(track);
    }

    if (group.children.length > 0) {
      globals.workspace.addChild(group);
    }
  }

  private groupTracksByRegex(regex: RegExp, groupName: string): void {
    const group = new GroupNode(groupName);

    for (const track of globals.workspace.children) {
      if (track instanceof TrackNode && regex.test(track.displayName)) {
        group.addChild(track);
      }
    }

    if (group.children.length > 0) {
      globals.workspace.addChild(group);
    }
  }

  private addAnnotationTracks(tracks: ReadonlyArray<TrackDescriptor>): void {
    const annotationTracks = tracks.filter(
      ({tags}) => tags?.scope === 'annotation',
    );
    const groups = new Map<string, GroupNode>();

    annotationTracks
      .filter(({tags}) => tags?.kind === THREAD_SLICE_TRACK_KIND)
      .forEach((td) => {
        const upid = assertExists(td.tags?.upid);
        const groupName = td.tags?.groupName;

        // We want to try and find a group to put this track in. If groupName is
        // defined, create a new group or place in existing one if it already
        // exists Otherwise, try upid to see if we can put this in a process
        // group

        let container: ContainerNode;
        if (groupName) {
          const existingGroup = groups.get(groupName);
          if (!existingGroup) {
            const group = new GroupNode(groupName);
            group.headerTrackUri = td.uri;
            container = group;
            groups.set(groupName, group);
            globals.workspace.addChild(group);
          } else {
            container = existingGroup;
          }
        } else {
          const procGroup = this.processGroups.get(upid);
          if (upid !== 0 && procGroup) {
            container = procGroup;
          } else {
            container = globals.workspace;
          }
        }

        container.addChild(new TrackNode(td.uri, td.title));
      });

    annotationTracks
      .filter(({tags}) => tags?.kind === COUNTER_TRACK_KIND)
      .forEach((td) => {
        const upid = td.tags?.upid;
        const parent =
          (exists(upid) && this.processGroups.get(upid)) || globals.workspace;
        parent.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addThreadStateTracks(tracks: ReadonlyArray<TrackDescriptor>): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === THREAD_STATE_TRACK_KIND && tags?.utid !== undefined,
      )
      .forEach((td) => {
        const utid = assertExists(td.tags?.utid);
        const group = this.getThreadGroup(utid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addThreadCpuSampleTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === CPU_PROFILE_TRACK_KIND && tags?.utid !== undefined,
      )
      .forEach((td) => {
        const utid = assertExists(td.tags?.utid);
        const group = this.getThreadGroup(utid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addThreadCounterTracks(tracks: ReadonlyArray<TrackDescriptor>): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === COUNTER_TRACK_KIND &&
          tags?.utid !== undefined &&
          tags?.scope === 'thread',
      )
      .forEach((td) => {
        const utid = assertExists(td.tags?.utid);
        const group = this.getThreadGroup(utid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addProcessAsyncSliceTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === ASYNC_SLICE_TRACK_KIND &&
          tags?.upid !== undefined &&
          tags?.scope === 'process',
      )
      .forEach((td) => {
        const upid = assertExists(td.tags?.upid);
        const group = this.getProcGroup(upid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addUserAsyncSliceTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    const groupMap = new Map<string, GroupNode>();
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === ASYNC_SLICE_TRACK_KIND &&
          tags?.scope === 'user' &&
          tags?.rawName !== undefined,
      )
      .forEach((td) => {
        const rawName = td.tags?.rawName;
        if (typeof rawName === 'string') {
          const track = new TrackNode(td.uri, td.title);
          const existingGroup = groupMap.get(rawName);
          if (existingGroup) {
            existingGroup.addChild(track);
          } else {
            const group = new GroupNode(rawName);
            globals.workspace.addChild(group);
            groupMap.set(rawName, group);
            group.addChild(track);
          }
        }
      });
  }

  private addActualFramesTracks(tracks: ReadonlyArray<TrackDescriptor>): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === ACTUAL_FRAMES_SLICE_TRACK_KIND &&
          tags?.upid !== undefined,
      )
      .forEach((td) => {
        const upid = assertExists(td.tags?.upid);
        const group = this.getProcGroup(upid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addExpectedFramesTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === EXPECTED_FRAMES_SLICE_TRACK_KIND &&
          tags?.upid !== undefined,
      )
      .forEach((td) => {
        const upid = assertExists(td.tags?.upid);
        const group = this.getProcGroup(upid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addThreadSliceTracks(tracks: ReadonlyArray<TrackDescriptor>): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === THREAD_SLICE_TRACK_KIND && tags?.utid !== undefined,
      )
      .forEach((td) => {
        const utid = assertExists(td.tags?.utid);
        // const upid = td.tags?.upid;
        // const isDefaultTrackForScope = Boolean(td.tags?.isDefaultTrackForScope);
        const group = this.getThreadGroup(utid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addAsyncThreadSliceTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === ASYNC_SLICE_TRACK_KIND &&
          tags?.utid !== undefined &&
          tags?.scope === 'thread',
      )
      .forEach((td) => {
        const utid = assertExists(td.tags?.utid);
        const group = this.getThreadGroup(utid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private async addProcessCounterTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): Promise<void> {
    const processCounterTracks = tracks.filter(
      ({tags}) =>
        tags?.kind === COUNTER_TRACK_KIND &&
        tags?.scope === 'process' &&
        tags?.upid !== undefined,
    );

    for (const td of processCounterTracks) {
      const upid = assertExists(td.tags?.upid);
      const group = this.getProcGroup(upid);
      // const trackNameTag = td.tags?.trackName;
      // const trackName =
      //   typeof trackNameTag === 'string' ? trackNameTag : undefined;
      group.addChild(new TrackNode(td.uri, td.title));
    }
  }

  private addProcessHeapProfileTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === HEAP_PROFILE_TRACK_KIND && tags?.upid !== undefined,
      )
      .forEach((td) => {
        const upid = assertExists(td.tags?.upid);
        const group = this.getProcGroup(upid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addProcessPerfSamplesTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === PERF_SAMPLES_PROFILE_TRACK_KIND &&
          tags.upid !== undefined &&
          tags.utid === undefined,
      )
      .forEach((td) => {
        const upid = assertExists(td.tags?.upid);
        const group = this.getProcGroup(upid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addThreadPerfSamplesTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === PERF_SAMPLES_PROFILE_TRACK_KIND &&
          tags?.utid !== undefined,
      )
      .forEach((td) => {
        // const upid = td.tags?.upid;
        const utid = assertExists(td.tags?.utid);
        const group = this.getThreadGroup(utid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private getProcGroup(upid: number): GroupNode {
    const group = this.processGroups.get(upid);
    if (group) {
      return group;
    } else {
      throw new Error(`Unable to find proc group with upid ${upid}`);
    }
  }

  private getThreadGroup(utid: number): GroupNode {
    const group = this.threadGroups.get(utid);
    if (group) {
      return group;
    } else {
      throw new Error(`Unable to find thread group with utid ${utid}`);
    }
  }

  private async addKernelThreadGrouping(engine: Engine): Promise<void> {
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

    // Create the track group. Use kthreadd's PROCESS_SUMMARY_TRACK for the
    // main track. It doesn't summarise the kernel threads within the group,
    // but creating a dedicated track type is out of scope at the time of
    // writing.
    const group = new GroupNode('Kernel threads');
    group.headerTrackUri = '/kernel'; // Summary track
    globals.workspace.addChild(group);

    // Set the group for all kernel threads (including kthreadd itself).
    for (; it.valid(); it.next()) {
      const {utid} = it;

      const threadGroup = new GroupNode(`Kernel Thread ${utid}`);
      threadGroup.headless = true;
      group.addChild(threadGroup);

      this.threadGroups.set(utid, threadGroup);
    }
  }

  // Adds top level groups for processes and thread that don't belong to a
  // process.
  private async addProcessGroups(engine: Engine): Promise<void> {
    const result = await engine.query(`
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

        const group = new GroupNode(displayName);
        group.headerTrackUri = `/process_${uid}`; // Summary track URI
        globals.workspace.addChild(group);
        this.processGroups.set(uid, group);
      } else {
        // Ignore kernel process groups
        if (this.threadGroups.has(uid)) {
          continue;
        }

        function getThreadDisplayName(
          threadName: string | undefined,
          pid: number,
        ) {
          if (threadName) {
            return `${stripPathFromExecutable(threadName)} ${pid}`;
          } else {
            return `Thread ${pid}`;
          }
        }

        const displayName = getThreadDisplayName(name ?? undefined, id);

        const group = new GroupNode(displayName);
        group.headerTrackUri = `/thread_${uid}`; // Summary track URI
        globals.workspace.addChild(group);
        this.threadGroups.set(uid, group);
      }
    }
  }

  // Create all the nested & headless thread groups that live inside existing
  // process groups.
  private async addThreadGroups(engine: Engine): Promise<void> {
    const result = await engine.query(`
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

      const threadGroup = new GroupNode(threadName ?? `Thread ${tid}`);
      this.threadGroups.set(utid, threadGroup);
      threadGroup.headless = true;
      threadGroup.expand();

      this.processGroups.get(upid)?.addChild(threadGroup);
    }
  }

  private addPluginTracks(): void {
    const groups = new Map<string, GroupNode>();
    const tracks = globals.trackManager.findPotentialTracks();

    for (const info of tracks) {
      const groupName = info.groupName;
      let container: ContainerNode = globals.workspace;

      if (groupName) {
        const existingGroup = groups.get(groupName);
        if (existingGroup) {
          container = existingGroup;
        } else {
          // Add the group
          const group = new GroupNode(groupName);
          container = group;
          globals.workspace.addChild(group);
          groups.set(groupName, group);
        }
      }

      const track = new TrackNode(info.uri, info.title);
      container.addChild(track);

      if (info.isPinned) {
        track.pin();
      }
    }
  }

  private addScrollJankPluginTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    const group = new GroupNode('Chrome Scroll Jank');
    tracks
      .filter(({tags}) => tags?.kind === CHROME_TOPLEVEL_SCROLLS_KIND)
      .forEach((td) => {
        group.addChild(new TrackNode(td.uri, td.title));
      });
    tracks
      .filter(({tags}) => tags?.kind === SCROLL_JANK_V3_TRACK_KIND)
      .forEach((td) => {
        group.addChild(new TrackNode(td.uri, td.title));
      });
    tracks
      .filter(({tags}) => tags?.kind === CHROME_EVENT_LATENCY_TRACK_KIND)
      .forEach((td) => {
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  private addChromeScrollJankTrack(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    tracks
      .filter(({tags}) => tags?.kind === CHROME_SCROLL_JANK_TRACK_KIND)
      .forEach((td) => {
        const utid = assertExists(td.tags?.utid);
        const group = this.getThreadGroup(utid);
        group.addChild(new TrackNode(td.uri, td.title));
      });
  }

  // Add an ordinary track from a track descriptor
  private addTrack(track: TrackDescriptor): void {
    globals.workspace.addChild(new TrackNode(track.uri, track.title));
  }

  // Add tracks that match some predicate
  private addTracks(
    source: ReadonlyArray<TrackDescriptor>,
    predicate: (td: TrackDescriptor) => boolean,
  ): ReadonlyArray<TrackDescriptor> {
    const filteredTracks = source.filter(predicate);
    filteredTracks.forEach((a) => this.addTrack(a));
    return filteredTracks;
  }

  public async decideTracks(): Promise<void> {
    const tracks = globals.trackManager.getAllTracks();

    // Add first the global tracks that don't require per-process track groups.
    this.addTracks(tracks, ({uri}) => uri === 'screenshots');
    this.addTracks(tracks, ({tags}) => tags?.kind === CPU_SLICE_TRACK_KIND);
    this.addTracks(tracks, ({tags}) => tags?.kind === CPU_FREQ_TRACK_KIND);
    this.addScrollJankPluginTracks(tracks);
    this.addTracks(
      tracks,
      ({tags}) =>
        tags?.kind === ASYNC_SLICE_TRACK_KIND && tags?.scope === 'global',
    );
    this.addTracks(
      tracks,
      ({tags}) =>
        tags?.kind === COUNTER_TRACK_KIND && tags?.scope === 'gpuFreq',
    );
    this.addTracks(
      tracks,
      ({tags}) =>
        tags?.kind === COUNTER_TRACK_KIND && tags?.scope === 'cpuFreqLimit',
    );
    this.addTracks(
      tracks,
      ({tags}) =>
        tags?.kind === COUNTER_TRACK_KIND && tags?.scope === 'cpuPerf',
    );
    this.addPluginTracks();
    this.addAnnotationTracks(tracks);

    this.groupGlobalIonTracks();
    this.groupGlobalIostatTracks(F2FS_IOSTAT_TAG, F2FS_IOSTAT_GROUP_NAME);
    this.groupGlobalIostatTracks(
      F2FS_IOSTAT_LAT_TAG,
      F2FS_IOSTAT_LAT_GROUP_NAME,
    );
    this.groupGlobalIostatTracks(DISK_IOSTAT_TAG, DISK_IOSTAT_GROUP_NAME);
    this.groupTracksByRegex(UFS_CMD_TAG_REGEX, UFS_CMD_TAG_GROUP);
    this.groupGlobalBuddyInfoTracks();
    this.groupTracksByRegex(KERNEL_WAKELOCK_REGEX, KERNEL_WAKELOCK_GROUP);
    this.groupTracksByRegex(NETWORK_TRACK_REGEX, NETWORK_TRACK_GROUP);
    this.groupTracksByRegex(ENTITY_RESIDENCY_REGEX, ENTITY_RESIDENCY_GROUP);
    this.groupTracksByRegex(UCLAMP_REGEX, UCLAMP_GROUP);
    this.groupFrequencyTracks(FREQUENCY_GROUP);
    this.groupTracksByRegex(POWER_RAILS_REGEX, POWER_RAILS_GROUP);
    this.groupTracksByRegex(TEMPERATURE_REGEX, TEMPERATURE_GROUP);
    this.groupTracksByRegex(IRQ_REGEX, IRQ_GROUP);
    this.groupTracksByRegex(CHROME_TRACK_REGEX, CHROME_TRACK_GROUP);
    this.groupMiscNonAllowlistedTracks(MISC_GROUP);

    // Add user slice tracks before listing the processes. These tracks will
    // be listed with their user/package name only, and they will be grouped
    // under on their original shared track names. E.g. "GPU Work Period"
    this.addUserAsyncSliceTracks(tracks);

    // Pre-group all kernel "threads" (actually processes) if this is a linux
    // system trace. Below, addProcessTrackGroups will skip them due to an
    // existing group uuid, and addThreadStateTracks will fill in the
    // per-thread tracks. Quirk: since all threads will appear to be
    // TrackKindPriority.MAIN_THREAD, any process-level tracks will end up
    // pushed to the bottom of the group in the UI.
    await this.addKernelThreadGrouping(
      this.engine.getProxy('TrackDecider::addKernelThreadGrouping'),
    );

    // Create the per-process track groups. Note that this won't necessarily
    // create a track per process. If a process has been completely idle and has
    // no sched events, no track group will be emitted.
    // Will populate this.addTrackGroupActions
    await this.addProcessGroups(
      this.engine.getProxy('TrackDecider::addProcessTrackGroups'),
    );

    this.addExpectedFramesTracks(tracks);
    this.addActualFramesTracks(tracks);
    this.addProcessPerfSamplesTracks(tracks);
    this.addProcessHeapProfileTracks(tracks);

    await this.addThreadGroups(
      this.engine.getProxy('TrackDecider::addThreadTrackGroups'),
    );

    this.addThreadPerfSamplesTracks(tracks);
    this.addThreadCpuSampleTracks(tracks);
    this.addThreadStateTracks(tracks);
    this.addThreadSliceTracks(tracks);
    this.addThreadCounterTracks(tracks);

    await this.addProcessCounterTracks(tracks);
    this.addProcessAsyncSliceTracks(tracks);
    this.addAsyncThreadSliceTracks(tracks);

    this.addChromeScrollJankTrack(tracks);

    // Remove any empty groups
    globals.workspace.children.forEach((n) => {
      if (n instanceof GroupNode && n.children.length === 0) {
        globals.workspace.removeChild(n);
      }
    });

    // Move groups underneath tracks
    Array.from(globals.workspace.children)
      .sort((a, b) => {
        // Define the desired order
        const order = [TrackNode, GroupNode];

        // Get the index in the order array
        const indexA = order.findIndex((type) => a instanceof type);
        const indexB = order.findIndex((type) => b instanceof type);

        // Sort based on the index in the order array
        return indexA - indexB;
      })
      .forEach((n) => globals.workspace.addChild(n));

    // If there is only one group, expand it
    const groups = globals.workspace.flatGroups;
    if (groups.length === 1) {
      groups[0].expand();
    }
  }
}

function stripPathFromExecutable(path: string) {
  if (path[0] === '/') {
    return path.split('/').slice(-1)[0];
  } else {
    return path;
  }
}

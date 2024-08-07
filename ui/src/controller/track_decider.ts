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

import {v4 as uuidv4} from 'uuid';

import {assertExists} from '../base/logging';
import {Actions, AddTrackArgs, DeferredAction} from '../common/actions';
import {
  InThreadTrackSortKey,
  SCROLLING_TRACK_GROUP,
  TrackSortKey,
  UtidToTrackSortKey,
} from '../common/state';
import {globals} from '../frontend/globals';
import {PrimaryTrackSortKey, TrackDescriptor} from '../public';
import {getThreadOrProcUri, getTrackName} from '../public/utils';
import {Engine, EngineBase} from '../trace_processor/engine';
import {NUM, NUM_NULL, STR_NULL} from '../trace_processor/query_result';
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
import {exists} from '../base/utils';
import {sqliteString} from '../base/string_utils';

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
const SCROLL_JANK_GROUP_ID = 'chrome-scroll-jank-track-group';

export async function decideTracks(
  engine: EngineBase,
): Promise<DeferredAction[]> {
  return new TrackDecider(engine).decideTracks();
}

class TrackDecider {
  private engine: EngineBase;
  private upidToUuid = new Map<number, string>();
  private utidToUuid = new Map<number, string>();
  private tracksToAdd: AddTrackArgs[] = [];
  private tracksToPin: string[] = [];
  private addTrackGroupActions: DeferredAction[] = [];

  constructor(engine: EngineBase) {
    this.engine = engine;
  }

  private groupGlobalIonTracks(): void {
    const ionTracks: AddTrackArgs[] = [];
    let hasSummary = false;
    for (const track of this.tracksToAdd) {
      const isIon = track.name.startsWith(MEM_ION);
      const isIonCounter = track.name === MEM_ION;
      const isDmaHeapCounter = track.name === MEM_DMA_COUNTER_NAME;
      const isDmaBuffferSlices = track.name === MEM_DMA;
      if (isIon || isIonCounter || isDmaHeapCounter || isDmaBuffferSlices) {
        ionTracks.push(track);
      }
      hasSummary = hasSummary || isIonCounter;
      hasSummary = hasSummary || isDmaHeapCounter;
    }

    if (ionTracks.length === 0 || !hasSummary) {
      return;
    }

    const groupUuid = uuidv4();
    const summaryTrackKey = uuidv4();
    let foundSummary = false;

    for (const track of ionTracks) {
      if (
        !foundSummary &&
        [MEM_DMA_COUNTER_NAME, MEM_ION].includes(track.name)
      ) {
        foundSummary = true;
        track.key = summaryTrackKey;
        track.trackGroup = undefined;
      } else {
        track.trackGroup = groupUuid;
      }
    }

    const addGroup = Actions.addTrackGroup({
      summaryTrackKey,
      name: MEM_DMA_COUNTER_NAME,
      key: groupUuid,
      collapsed: true,
    });
    this.addTrackGroupActions.push(addGroup);
  }

  private groupGlobalIostatTracks(tag: string, group: string): void {
    const iostatTracks: AddTrackArgs[] = [];
    const devMap = new Map<string, string>();

    for (const track of this.tracksToAdd) {
      if (track.name.startsWith(tag)) {
        iostatTracks.push(track);
      }
    }

    if (iostatTracks.length === 0) {
      return;
    }

    for (const track of iostatTracks) {
      const name = track.name.split('.', 3);

      if (!devMap.has(name[1])) {
        devMap.set(name[1], uuidv4());
      }
      track.name = name[2];
      track.trackGroup = devMap.get(name[1]);
    }

    for (const [key, value] of devMap) {
      const groupName = group + key;
      const addGroup = Actions.addTrackGroup({
        name: groupName,
        key: value,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }
  }

  private groupGlobalBuddyInfoTracks(): void {
    const buddyInfoTracks: AddTrackArgs[] = [];
    const devMap = new Map<string, string>();

    for (const track of this.tracksToAdd) {
      if (track.name.startsWith(BUDDY_INFO_TAG)) {
        buddyInfoTracks.push(track);
      }
    }

    if (buddyInfoTracks.length === 0) {
      return;
    }

    for (const track of buddyInfoTracks) {
      const tokens = track.name.split('[');
      const node = tokens[1].slice(0, -1);
      const zone = tokens[2].slice(0, -1);
      const size = tokens[3].slice(0, -1);

      const groupName = 'Buddyinfo:  Node: ' + node + ' Zone: ' + zone;
      if (!devMap.has(groupName)) {
        devMap.set(groupName, uuidv4());
      }
      track.name = 'Chunk size: ' + size;
      track.trackGroup = devMap.get(groupName);
    }

    for (const [key, value] of devMap) {
      const groupName = key;
      const addGroup = Actions.addTrackGroup({
        name: groupName,
        key: value,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }
  }

  private groupFrequencyTracks(groupName: string): void {
    let groupUuid = undefined;
    for (const track of this.tracksToAdd) {
      // Group all the frequency tracks together (except the CPU and GPU
      // frequency ones).
      if (
        track.name.endsWith('Frequency') &&
        !track.name.startsWith('Cpu') &&
        !track.name.startsWith('Gpu')
      ) {
        if (
          track.trackGroup !== undefined &&
          track.trackGroup !== SCROLLING_TRACK_GROUP
        ) {
          continue;
        }
        if (groupUuid === undefined) {
          groupUuid = uuidv4();
        }
        track.trackGroup = groupUuid;
      }
    }

    if (groupUuid !== undefined) {
      const addGroup = Actions.addTrackGroup({
        name: groupName,
        key: groupUuid,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
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

    let groupUuid = undefined;
    for (const track of this.tracksToAdd) {
      if (
        track.trackGroup !== undefined &&
        track.trackGroup !== SCROLLING_TRACK_GROUP
      ) {
        continue;
      }
      let allowlisted = false;
      for (const regex of ALLOWLIST_REGEXES) {
        allowlisted = allowlisted || regex.test(track.name);
      }
      if (allowlisted) {
        continue;
      }
      if (groupUuid === undefined) {
        groupUuid = uuidv4();
      }
      track.trackGroup = groupUuid;
    }

    if (groupUuid !== undefined) {
      const addGroup = Actions.addTrackGroup({
        name: groupName,
        key: groupUuid,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }
  }

  private groupTracksByRegex(regex: RegExp, groupName: string): void {
    let groupUuid = undefined;

    for (const track of this.tracksToAdd) {
      if (regex.test(track.name)) {
        if (
          track.trackGroup !== undefined &&
          track.trackGroup !== SCROLLING_TRACK_GROUP
        ) {
          continue;
        }
        if (groupUuid === undefined) {
          groupUuid = uuidv4();
        }
        track.trackGroup = groupUuid;
      }
    }

    if (groupUuid !== undefined) {
      const addGroup = Actions.addTrackGroup({
        name: groupName,
        key: groupUuid,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }
  }

  private addAnnotationTracks(tracks: ReadonlyArray<TrackDescriptor>): void {
    const annotationTracks = tracks.filter(
      ({tags}) => tags?.scope === 'annotation',
    );

    interface GroupIds {
      id: string;
      summaryTrackKey: string;
    }

    const groupNameToKeys = new Map<string, GroupIds>();

    annotationTracks
      .filter(({tags}) => tags?.kind === THREAD_SLICE_TRACK_KIND)
      .forEach((td) => {
        const upid = assertExists(td.tags?.upid);
        const groupName = td.tags?.groupName;

        let summaryTrackKey = undefined;
        let trackGroupId =
          upid === 0 ? SCROLLING_TRACK_GROUP : this.upidToUuid.get(upid);

        if (groupName !== undefined) {
          // If this is the first track encountered for a certain group,
          // create an id for the group and use this track as the group's
          // summary track.
          const groupKeys = groupNameToKeys.get(groupName);
          if (groupKeys) {
            trackGroupId = groupKeys.id;
          } else {
            trackGroupId = uuidv4();
            summaryTrackKey = uuidv4();
            groupNameToKeys.set(groupName, {
              id: trackGroupId,
              summaryTrackKey,
            });
          }
        }

        this.tracksToAdd.push({
          uri: td.uri,
          key: summaryTrackKey,
          name: td.title,
          trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
          trackGroup: trackGroupId,
        });
      });

    for (const [groupName, groupKeys] of groupNameToKeys) {
      const addGroup = Actions.addTrackGroup({
        summaryTrackKey: groupKeys.summaryTrackKey,
        name: groupName,
        key: groupKeys.id,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }

    annotationTracks
      .filter(({tags}) => tags?.kind === COUNTER_TRACK_KIND)
      .forEach((td) => {
        const upid = td.tags?.upid;

        this.tracksToAdd.push({
          uri: td.uri,
          key: td.uri,
          name: td.title,
          trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
          trackGroup: exists(upid)
            ? this.upidToUuid.get(upid)
            : SCROLLING_TRACK_GROUP,
        });
      });
  }

  private addThreadStateTracks(tracks: ReadonlyArray<TrackDescriptor>): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === THREAD_STATE_TRACK_KIND && tags?.utid !== undefined,
      )
      .forEach((td) => {
        const upid = td.tags?.upid ?? null;
        const utid = assertExists(td.tags?.utid);

        const groupId = this.getUuidUnchecked(utid, upid);
        if (groupId === undefined) {
          // If a thread has no scheduling activity (i.e. the sched table has zero
          // rows for that uid) no track group will be created and we want to skip
          // the track creation as well.
          return;
        }

        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackGroup: groupId,
          trackSortKey: {
            utid,
            priority: InThreadTrackSortKey.THREAD_SCHEDULING_STATE_TRACK,
          },
        });
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
        const upid = td.tags?.upid ?? null;
        const groupId = this.getUuid(utid, upid);
        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackSortKey: {
            utid,
            priority: InThreadTrackSortKey.CPU_STACK_SAMPLES_TRACK,
          },
          trackGroup: groupId,
        });
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
        const upid = td.tags?.upid ?? null;
        const groupId = this.getUuid(utid, upid);
        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackSortKey: {
            utid,
            priority: InThreadTrackSortKey.ORDINARY,
          },
          trackGroup: groupId,
        });
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
        const groupId = this.getUuid(null, upid);
        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
          trackGroup: groupId,
        });
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
        const groupId = this.getUuid(null, upid);

        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackSortKey: PrimaryTrackSortKey.ACTUAL_FRAMES_SLICE_TRACK,
          trackGroup: groupId,
        });
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
        const groupId = this.getUuid(null, upid);

        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackSortKey: PrimaryTrackSortKey.EXPECTED_FRAMES_SLICE_TRACK,
          trackGroup: groupId,
        });
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
        const upid = td.tags?.upid ?? null;
        const isDefaultTrackForScope = Boolean(td.tags?.isDefaultTrackForScope);
        const groupId = this.getUuid(utid, upid);

        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackGroup: groupId,
          trackSortKey: {
            utid,
            priority: isDefaultTrackForScope
              ? InThreadTrackSortKey.DEFAULT_TRACK
              : InThreadTrackSortKey.ORDINARY,
          },
        });
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
      const groupId = this.getUuid(null, upid);
      const trackNameTag = td.tags?.trackName;
      const trackName =
        typeof trackNameTag === 'string' ? trackNameTag : undefined;

      this.tracksToAdd.push({
        key: td.uri,
        uri: td.uri,
        name: td.title,
        trackSortKey: await this.resolveTrackSortKeyForProcessCounterTrack(
          upid,
          trackName,
        ),
        trackGroup: groupId,
      });
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
        const groupId = this.getUuid(null, upid);
        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackSortKey: PrimaryTrackSortKey.HEAP_PROFILE_TRACK,
          trackGroup: groupId,
        });
      });
  }

  private addProcessPerfSamplesTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    tracks
      .filter(
        ({tags}) =>
          tags?.kind === PERF_SAMPLES_PROFILE_TRACK_KIND &&
          tags?.upid !== undefined,
      )
      .forEach((td) => {
        const upid = assertExists(td.tags?.upid);
        const groupId = this.getUuid(null, upid);
        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackSortKey: PrimaryTrackSortKey.PERF_SAMPLES_PROFILE_TRACK,
          trackGroup: groupId,
        });
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
        const upid = td.tags?.upid ?? null;
        const utid = assertExists(td.tags?.utid);
        const groupId = this.getUuid(utid, upid);
        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackSortKey: PrimaryTrackSortKey.PERF_SAMPLES_PROFILE_TRACK,
          trackGroup: groupId,
        });
      });
  }

  private getUuidUnchecked(utid: number | null, upid: number | null) {
    return upid === null
      ? this.utidToUuid.get(utid!)
      : this.upidToUuid.get(upid);
  }

  private getUuid(utid: number | null, upid: number | null) {
    return assertExists(this.getUuidUnchecked(utid, upid));
  }

  private getOrCreateUuid(utid: number | null, upid: number | null) {
    let uuid = this.getUuidUnchecked(utid, upid);
    if (uuid === undefined) {
      uuid = uuidv4();
      if (upid === null) {
        this.utidToUuid.set(utid!, uuid);
      } else {
        this.upidToUuid.set(upid, uuid);
      }
    }
    return uuid;
  }

  private setUuidForUpid(upid: number, uuid: string) {
    this.upidToUuid.set(upid, uuid);
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
    const kthreadGroupUuid = uuidv4();
    const summaryTrackKey = uuidv4();
    this.tracksToAdd.push({
      uri: '/kernel',
      key: summaryTrackKey,
      trackSortKey: PrimaryTrackSortKey.PROCESS_SUMMARY_TRACK,
      name: `Kernel thread summary`,
    });
    const addTrackGroup = Actions.addTrackGroup({
      summaryTrackKey,
      name: `Kernel threads`,
      key: kthreadGroupUuid,
      collapsed: true,
    });
    this.addTrackGroupActions.push(addTrackGroup);

    // Set the group for all kernel threads (including kthreadd itself).
    for (; it.valid(); it.next()) {
      this.setUuidForUpid(it.upid, kthreadGroupUuid);
    }
  }

  private async addProcessTrackGroups(engine: Engine): Promise<void> {
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
          upid,
          null as utid,
          pid,
          null as tid,
          processName,
          null as threadName,
          sumRunningDur > 0 as hasSched,
          heapProfileAllocationCount > 0
            or heapGraphObjectCount > 0 as hasHeapInfo,
          ifnull(chromeProcessLabels, '') as chromeProcessLabels
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
          null,
          utid,
          null as pid,
          tid,
          null as processName,
          threadName,
          sumRunningDur > 0 as hasSched,
          0 as hasHeapInfo,
          '' as chromeProcessLabels
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
      upid: NUM_NULL,
      utid: NUM_NULL,
      pid: NUM_NULL,
      tid: NUM_NULL,
      processName: STR_NULL,
      threadName: STR_NULL,
      hasSched: NUM_NULL,
      hasHeapInfo: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const upid = it.upid;
      const pid = it.pid;
      const tid = it.tid;
      const threadName = it.threadName;
      const processName = it.processName;
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      const hasSched = !!it.hasSched;
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      const hasHeapInfo = !!it.hasHeapInfo;

      const summaryTrackKey = uuidv4();

      const uri = getThreadOrProcUri(upid, utid);

      // If previous groupings (e.g. kernel threads) picked up there tracks,
      // don't try to regroup them.
      const pUuid =
        upid === null ? this.utidToUuid.get(utid!) : this.upidToUuid.get(upid);
      if (pUuid !== undefined) {
        continue;
      }

      this.tracksToAdd.push({
        uri,
        key: summaryTrackKey,
        trackSortKey: hasSched
          ? PrimaryTrackSortKey.PROCESS_SCHEDULING_TRACK
          : PrimaryTrackSortKey.PROCESS_SUMMARY_TRACK,
        name: `${upid === null ? tid : pid} summary`,
      });

      const name = getTrackName({
        utid,
        processName,
        pid,
        threadName,
        tid,
        upid,
      });

      const addTrackGroup = Actions.addTrackGroup({
        summaryTrackKey,
        name: stripPathFromExecutable(name),
        key: this.getOrCreateUuid(utid, upid),
        // Perf profiling tracks remain collapsed, otherwise we would have too
        // many expanded process tracks for some perf traces, leading to
        // jankyness.
        collapsed: !hasHeapInfo,
      });
      this.addTrackGroupActions.push(addTrackGroup);
    }
  }

  private async computeThreadOrderingMetadata(): Promise<UtidToTrackSortKey> {
    const result = await this.engine.query(`
      select
        utid,
        tid,
        (select pid from process p where t.upid = p.upid) as pid,
        t.name as threadName
      from thread t
    `);

    const it = result.iter({
      utid: NUM,
      tid: NUM_NULL,
      pid: NUM_NULL,
      threadName: STR_NULL,
    });

    const threadOrderingMetadata: UtidToTrackSortKey = {};
    for (; it.valid(); it.next()) {
      threadOrderingMetadata[it.utid] = {
        tid: it.tid === null ? undefined : it.tid,
        sortKey: TrackDecider.getThreadSortKey(it.threadName, it.tid, it.pid),
      };
    }
    return threadOrderingMetadata;
  }

  private addPluginTracks(): void {
    const groupNameToUuid = new Map<string, string>();
    const tracks = globals.trackManager.findPotentialTracks();

    for (const info of tracks) {
      const groupName = info.groupName;

      let groupUuid = SCROLLING_TRACK_GROUP;
      if (groupName) {
        const uuid = groupNameToUuid.get(groupName);
        if (uuid) {
          groupUuid = uuid;
        } else {
          // Add the group
          groupUuid = uuidv4();
          const addGroup = Actions.addTrackGroup({
            name: groupName,
            key: groupUuid,
            collapsed: true,
            fixedOrdering: true,
          });
          this.addTrackGroupActions.push(addGroup);

          // Add group to the map
          groupNameToUuid.set(groupName, groupUuid);
        }
      }

      this.tracksToAdd.push({
        uri: info.uri,
        key: info.uri,
        name: info.title,
        // TODO(hjd): Fix how sorting works. Plugins should expose
        // 'sort keys' which the user can use to choose a sort order.
        trackSortKey: info.sortKey ?? PrimaryTrackSortKey.ORDINARY_TRACK,
        trackGroup: groupUuid,
      });

      if (info.isPinned) {
        this.tracksToPin.push(info.uri);
      }
    }
  }

  private addScrollJankPluginTracks(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    let scrollTracks = this.addTracks(
      tracks,
      ({tags}) => tags?.kind === CHROME_TOPLEVEL_SCROLLS_KIND,
      SCROLL_JANK_GROUP_ID,
    );
    scrollTracks = scrollTracks.concat(
      this.addTracks(
        tracks,
        ({tags}) => tags?.kind === SCROLL_JANK_V3_TRACK_KIND,
        SCROLL_JANK_GROUP_ID,
      ),
    );
    scrollTracks = scrollTracks.concat(
      this.addTracks(
        tracks,
        ({tags}) => tags?.kind === CHROME_EVENT_LATENCY_TRACK_KIND,
        SCROLL_JANK_GROUP_ID,
      ),
    );
    if (scrollTracks.length > 0) {
      this.addTrackGroupActions.push(
        Actions.addTrackGroup({
          name: 'Chrome Scroll Jank',
          key: SCROLL_JANK_GROUP_ID,
          collapsed: false,
          fixedOrdering: true,
        }),
      );
    }
  }

  private addChromeScrollJankTrack(
    tracks: ReadonlyArray<TrackDescriptor>,
  ): void {
    tracks
      .filter(({tags}) => tags?.kind === CHROME_SCROLL_JANK_TRACK_KIND)
      .forEach((td) => {
        const upid = assertExists(td.tags?.upid);
        const utid = assertExists(td.tags?.utid);
        const groupId = this.getUuid(utid, upid);
        this.tracksToAdd.push({
          key: td.uri,
          uri: td.uri,
          name: td.title,
          trackSortKey: {
            utid,
            priority: InThreadTrackSortKey.ORDINARY,
          },
          trackGroup: groupId,
        });
      });
  }

  // Add an ordinary track from a track descriptor
  private addTrack(track: TrackDescriptor, groupId?: string): void {
    this.tracksToAdd.push({
      key: track.uri,
      uri: track.uri,
      name: track.title,
      trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
      trackGroup: groupId ?? SCROLLING_TRACK_GROUP,
    });
  }

  // Add tracks that match some predicate
  private addTracks(
    source: ReadonlyArray<TrackDescriptor>,
    predicate: (td: TrackDescriptor) => boolean,
    groupId?: string,
  ): ReadonlyArray<TrackDescriptor> {
    const filteredTracks = source.filter(predicate);
    filteredTracks.forEach((a) => this.addTrack(a, groupId));
    return filteredTracks;
  }

  public async decideTracks(): Promise<DeferredAction[]> {
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
    this.addTracks(
      tracks,
      ({tags}) =>
        tags?.kind === ASYNC_SLICE_TRACK_KIND && tags?.scope === 'user',
    );

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
    await this.addProcessTrackGroups(
      this.engine.getProxy('TrackDecider::addProcessTrackGroups'),
    );

    this.addProcessHeapProfileTracks(tracks);
    this.addProcessPerfSamplesTracks(tracks);
    this.addThreadPerfSamplesTracks(tracks);
    await this.addProcessCounterTracks(tracks);
    this.addProcessAsyncSliceTracks(tracks);
    this.addActualFramesTracks(tracks);
    this.addExpectedFramesTracks(tracks);
    this.addThreadCounterTracks(tracks);
    this.addThreadStateTracks(tracks);
    this.addThreadSliceTracks(tracks);
    this.addThreadCpuSampleTracks(tracks);

    this.addChromeScrollJankTrack(tracks);

    this.addTrackGroupActions.push(
      Actions.addTracks({tracks: this.tracksToAdd}),
    );

    // Add the actions to pin any tracks we need to pin
    for (const trackKey of this.tracksToPin) {
      this.addTrackGroupActions.push(Actions.toggleTrackPinned({trackKey}));
    }

    const threadOrderingMetadata = await this.computeThreadOrderingMetadata();
    this.addTrackGroupActions.push(
      Actions.setUtidToTrackSortKey({threadOrderingMetadata}),
    );

    return this.addTrackGroupActions;
  }

  // Some process counter tracks are tied to specific threads based on their
  // name.
  private async resolveTrackSortKeyForProcessCounterTrack(
    upid: number,
    threadName?: string,
  ): Promise<TrackSortKey> {
    if (threadName !== 'GPU completion') {
      return PrimaryTrackSortKey.COUNTER_TRACK;
    }
    const result = await this.engine.query(`
      select utid
      from thread
      where upid=${upid} and name=${sqliteString(threadName)}
    `);
    const it = result.iter({
      utid: NUM,
    });
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    for (; it; it.next()) {
      return {
        utid: it.utid,
        priority: InThreadTrackSortKey.THREAD_COUNTER_TRACK,
      };
    }
    return PrimaryTrackSortKey.COUNTER_TRACK;
  }

  private static getThreadSortKey(
    threadName?: string | null,
    tid?: number | null,
    pid?: number | null,
  ): PrimaryTrackSortKey {
    if (pid !== undefined && pid !== null && pid === tid) {
      return PrimaryTrackSortKey.MAIN_THREAD;
    }
    if (threadName === undefined || threadName === null) {
      return PrimaryTrackSortKey.ORDINARY_THREAD;
    }

    // Chrome main threads should always come first within their process.
    if (
      threadName === 'CrBrowserMain' ||
      threadName === 'CrRendererMain' ||
      threadName === 'CrGpuMain'
    ) {
      return PrimaryTrackSortKey.MAIN_THREAD;
    }

    // Chrome IO threads should always come immediately after the main thread.
    if (
      threadName === 'Chrome_ChildIOThread' ||
      threadName === 'Chrome_IOThread'
    ) {
      return PrimaryTrackSortKey.CHROME_IO_THREAD;
    }

    // A Chrome process can have only one compositor thread, so we want to put
    // it next to other named processes.
    if (threadName === 'Compositor' || threadName === 'VizCompositorThread') {
      return PrimaryTrackSortKey.CHROME_COMPOSITOR_THREAD;
    }

    switch (true) {
      case /.*RenderThread.*/.test(threadName):
        return PrimaryTrackSortKey.RENDER_THREAD;
      case /.*GPU completion.*/.test(threadName):
        return PrimaryTrackSortKey.GPU_COMPLETION_THREAD;
      default:
        return PrimaryTrackSortKey.ORDINARY_THREAD;
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

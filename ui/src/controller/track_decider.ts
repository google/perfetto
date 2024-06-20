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
import {sqliteString} from '../base/string_utils';
import {Actions, AddTrackArgs, DeferredAction} from '../common/actions';
import {
  InThreadTrackSortKey,
  SCROLLING_TRACK_GROUP,
  TrackSortKey,
  UtidToTrackSortKey,
} from '../common/state';
import {globals} from '../frontend/globals';
import {PERF_SAMPLE_FLAG} from '../core/feature_flags';
import {PrimaryTrackSortKey} from '../public';
import {getTrackName} from '../public/utils';
import {Engine, EngineBase} from '../trace_processor/engine';
import {NUM, NUM_NULL, STR, STR_NULL} from '../trace_processor/query_result';
import {
  ENABLE_SCROLL_JANK_PLUGIN_V2,
  getScrollJankTracks,
} from '../core_plugins/chrome_scroll_jank';
import {decideTracks as scrollJankDecideTracks} from '../core_plugins/chrome_scroll_jank/chrome_tasks_scroll_jank_track';
import {COUNTER_TRACK_KIND} from '../core_plugins/counter';
import {decideTracks as screenshotDecideTracks} from '../core_plugins/screenshots';
import {
  ACTUAL_FRAMES_SLICE_TRACK_KIND,
  ASYNC_SLICE_TRACK_KIND,
  EXPECTED_FRAMES_SLICE_TRACK_KIND,
  THREAD_SLICE_TRACK_KIND,
  THREAD_STATE_TRACK_KIND,
} from '../core/track_kinds';

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

  async guessCpuSizes(): Promise<Map<number, string>> {
    const cpuToSize = new Map<number, string>();
    await this.engine.query(`
      include perfetto module viz.core_type;
    `);
    const result = await this.engine.query(`
      select cpu, _guess_core_type(cpu) as size
      from cpu_counter_track
      join _counter_track_summary using (id);
    `);

    const it = result.iter({
      cpu: NUM,
      size: STR_NULL,
    });

    for (; it.valid(); it.next()) {
      const size = it.size;
      if (size !== null) {
        cpuToSize.set(it.cpu, size);
      }
    }

    return cpuToSize;
  }

  async addCpuSchedulingTracks(): Promise<void> {
    const cpus = globals.traceContext.cpus;
    const cpuToSize = await this.guessCpuSizes();

    for (const cpu of cpus) {
      const size = cpuToSize.get(cpu);
      const name = size === undefined ? `Cpu ${cpu}` : `Cpu ${cpu} (${size})`;
      this.tracksToAdd.push({
        uri: `perfetto.CpuSlices#cpu${cpu}`,
        trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
        name,
        trackGroup: SCROLLING_TRACK_GROUP,
      });
    }
  }

  async addCpuFreqTracks(engine: Engine): Promise<void> {
    const cpus = globals.traceContext.cpus;

    for (const cpu of cpus) {
      // Only add a cpu freq track if we have
      // cpu freq data.
      // TODO(hjd): Find a way to display cpu idle
      // events even if there are no cpu freq events.
      const cpuFreqIdleResult = await engine.query(`
        select
          id as cpuFreqId,
          (
            select id
            from cpu_counter_track
            where name = 'cpuidle'
            and cpu = ${cpu}
            limit 1
          ) as cpuIdleId
        from cpu_counter_track
        join _counter_track_summary using (id)
        where name = 'cpufreq' and cpu = ${cpu}
        limit 1;
      `);

      if (cpuFreqIdleResult.numRows() > 0) {
        this.tracksToAdd.push({
          uri: `perfetto.CpuFreq#${cpu}`,
          trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
          name: `Cpu ${cpu} Frequency`,
          trackGroup: SCROLLING_TRACK_GROUP,
        });
      }
    }
  }

  async addGlobalAsyncTracks(engine: Engine): Promise<void> {
    const rawGlobalAsyncTracks = await engine.query(`
      with global_tracks_grouped as (
        select distinct t.parent_id, t.name
        from track t
        join _slice_track_summary using (id)
        where t.type in ('track', 'gpu_track', 'cpu_track')
      )
      select
        t.name as name,
        t.parent_id as parentId,
        p.name as parentName
      from global_tracks_grouped AS t
      left join track p on (t.parent_id = p.id)
      order by p.name, t.name
    `);
    const it = rawGlobalAsyncTracks.iter({
      name: STR_NULL,
      parentId: NUM_NULL,
      parentName: STR_NULL,
    });

    const parentIdToGroupKey = new Map<number, string>();
    for (; it.valid(); it.next()) {
      const kind = ASYNC_SLICE_TRACK_KIND;
      const rawName = it.name === null ? undefined : it.name;
      const rawParentName = it.parentName === null ? undefined : it.parentName;
      const name = getTrackName({name: rawName, kind});
      const parentTrackId = it.parentId;
      let groupKey = SCROLLING_TRACK_GROUP;

      if (parentTrackId !== null) {
        const maybeGroupKey = parentIdToGroupKey.get(parentTrackId);
        if (maybeGroupKey === undefined) {
          groupKey = uuidv4();
          parentIdToGroupKey.set(parentTrackId, groupKey);

          const parentName = getTrackName({name: rawParentName, kind});
          this.addTrackGroupActions.push(
            Actions.addTrackGroup({
              name: parentName,
              key: groupKey,
              collapsed: true,
            }),
          );
        } else {
          groupKey = maybeGroupKey;
        }
      }

      const track: AddTrackArgs = {
        uri: `perfetto.AsyncSlices#${rawName}.${it.parentId}`,
        trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
        trackGroup: groupKey,
        name,
      };

      this.tracksToAdd.push(track);
    }
  }

  async addGpuFreqTracks(engine: Engine): Promise<void> {
    const numGpus = globals.traceContext.gpuCount;
    for (let gpu = 0; gpu < numGpus; gpu++) {
      // Only add a gpu freq track if we have
      // gpu freq data.
      const freqExistsResult = await engine.query(`
        select *
        from gpu_counter_track
        join _counter_track_summary using (id)
        where name = 'gpufreq' and gpu_id = ${gpu}
        limit 1;
      `);
      if (freqExistsResult.numRows() > 0) {
        this.tracksToAdd.push({
          uri: `perfetto.Counter#gpu_freq${gpu}`,
          name: `Gpu ${gpu} Frequency`,
          trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
          trackGroup: SCROLLING_TRACK_GROUP,
        });
      }
    }
  }

  async addCpuFreqLimitCounterTracks(engine: Engine): Promise<void> {
    const cpuFreqLimitCounterTracksSql = `
      select name, id
      from cpu_counter_track
      join _counter_track_summary using (id)
      where name glob "Cpu * Freq Limit"
      order by name asc
    `;

    this.addCpuCounterTracks(engine, cpuFreqLimitCounterTracksSql);
  }

  async addCpuPerfCounterTracks(engine: Engine): Promise<void> {
    // Perf counter tracks are bound to CPUs, follow the scheduling and
    // frequency track naming convention ("Cpu N ...").
    // Note: we might not have a track for a given cpu if no data was seen from
    // it. This might look surprising in the UI, but placeholder tracks are
    // wasteful as there's no way of collapsing global counter tracks at the
    // moment.
    const addCpuPerfCounterTracksSql = `
      select printf("Cpu %u %s", cpu, name) as name, id
      from perf_counter_track as pct
      join _counter_track_summary using (id)
      order by perf_session_id asc, pct.name asc, cpu asc
    `;
    this.addCpuCounterTracks(engine, addCpuPerfCounterTracksSql);
  }

  async addCpuCounterTracks(engine: Engine, sql: string): Promise<void> {
    const result = await engine.query(sql);

    const it = result.iter({
      name: STR,
      id: NUM,
    });

    for (; it.valid(); it.next()) {
      const name = it.name;
      const trackId = it.id;
      this.tracksToAdd.push({
        uri: `perfetto.Counter#cpu${trackId}`,
        name,
        trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
        trackGroup: SCROLLING_TRACK_GROUP,
      });
    }
  }

  async groupGlobalIonTracks(): Promise<void> {
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

  async groupGlobalIostatTracks(tag: string, group: string): Promise<void> {
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

  async groupGlobalBuddyInfoTracks(): Promise<void> {
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

  async groupFrequencyTracks(groupName: string): Promise<void> {
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

  async groupMiscNonAllowlistedTracks(groupName: string): Promise<void> {
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

  async groupTracksByRegex(regex: RegExp, groupName: string): Promise<void> {
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

  async addAnnotationTracks(engine: Engine): Promise<void> {
    const sliceResult = await engine.query(`
      select id, name, upid, group_name
      from annotation_slice_track
      order by name
    `);

    const sliceIt = sliceResult.iter({
      id: NUM,
      name: STR,
      upid: NUM,
      group_name: STR_NULL,
    });

    interface GroupIds {
      id: string;
      summaryTrackKey: string;
    }

    const groupNameToKeys = new Map<string, GroupIds>();

    for (; sliceIt.valid(); sliceIt.next()) {
      const id = sliceIt.id;
      const name = sliceIt.name;
      const upid = sliceIt.upid;
      const groupName = sliceIt.group_name;

      let summaryTrackKey = undefined;
      let trackGroupId =
        upid === 0 ? SCROLLING_TRACK_GROUP : this.upidToUuid.get(upid);

      if (groupName) {
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
        uri: `perfetto.Annotation#${id}`,
        key: summaryTrackKey,
        name,
        trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
        trackGroup: trackGroupId,
      });
    }

    for (const [groupName, groupKeys] of groupNameToKeys) {
      const addGroup = Actions.addTrackGroup({
        summaryTrackKey: groupKeys.summaryTrackKey,
        name: groupName,
        key: groupKeys.id,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }

    const counterResult = await engine.query(`
      SELECT id, name, upid FROM annotation_counter_track
    `);

    const counterIt = counterResult.iter({
      id: NUM,
      name: STR,
      upid: NUM,
    });

    for (; counterIt.valid(); counterIt.next()) {
      const id = counterIt.id;
      const name = counterIt.name;
      const upid = counterIt.upid;
      this.tracksToAdd.push({
        uri: `perfetto.Annotation#counter${id}`,
        name,
        trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
        trackGroup:
          upid === 0 ? SCROLLING_TRACK_GROUP : this.upidToUuid.get(upid),
      });
    }
  }

  async addThreadStateTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      select
        utid,
        upid,
        tid,
        thread.name as threadName
      from thread
      join _sched_summary using (utid)
    `);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const upid = it.upid;
      const threadName = it.threadName;
      const uuid = this.getUuidUnchecked(utid, upid);
      if (uuid === undefined) {
        // If a thread has no scheduling activity (i.e. the sched table has zero
        // rows for that uid) no track group will be created and we want to skip
        // the track creation as well.
        continue;
      }

      const priority = InThreadTrackSortKey.THREAD_SCHEDULING_STATE_TRACK;
      const name = getTrackName({
        utid,
        tid,
        threadName,
        kind: THREAD_STATE_TRACK_KIND,
      });

      this.tracksToAdd.push({
        uri: `perfetto.ThreadState#${utid}`,
        name,
        trackGroup: uuid,
        trackSortKey: {
          utid,
          priority,
        },
      });
    }
  }

  async addThreadCpuSampleTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      with thread_cpu_sample as (
        select distinct utid
        from cpu_profile_stack_sample
        where utid != 0
      )
      select
        utid,
        tid,
        upid,
        thread.name as threadName
      from thread_cpu_sample
      join thread using(utid)`);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const upid = it.upid;
      const threadName = it.threadName;
      const uuid = this.getUuid(utid, upid);
      this.tracksToAdd.push({
        uri: `perfetto.CpuProfile#${utid}`,
        trackSortKey: {
          utid,
          priority: InThreadTrackSortKey.CPU_STACK_SAMPLES_TRACK,
        },
        name: `${threadName} (CPU Stack Samples)`,
        trackGroup: uuid,
      });
    }
  }

  async addThreadCounterTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      select
        thread_counter_track.name as trackName,
        utid,
        upid,
        tid,
        thread.name as threadName,
        thread_counter_track.id as trackId
      from thread_counter_track
      join _counter_track_summary using (id)
      join thread using (utid)
      where thread_counter_track.name != 'thread_time'
  `);

    const it = result.iter({
      trackName: STR_NULL,
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
      trackId: NUM,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const upid = it.upid;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const threadName = it.threadName;
      const uuid = this.getUuid(utid, upid);
      const name = getTrackName({
        name: trackName,
        utid,
        tid,
        kind: COUNTER_TRACK_KIND,
        threadName,
        threadTrack: true,
      });
      this.tracksToAdd.push({
        uri: `perfetto.Counter#thread${trackId}`,
        name,
        trackSortKey: {
          utid,
          priority: InThreadTrackSortKey.ORDINARY,
        },
        trackGroup: uuid,
      });
    }
  }

  async addProcessAsyncSliceTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      select
        upid,
        t.name as trackName,
        t.track_ids as trackIds,
        process.name as processName,
        process.pid as pid
      from _process_track_summary_by_upid_and_name t
      join process using(upid)
      where t.name is null or t.name not glob "* Timeline"
    `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const processName = it.processName;
      const pid = it.pid;

      const uuid = this.getUuid(null, upid);
      const name = getTrackName({
        name: trackName,
        upid,
        pid,
        processName,
        kind: ASYNC_SLICE_TRACK_KIND,
      });

      this.tracksToAdd.push({
        uri: `perfetto.AsyncSlices#process.${pid}${rawTrackIds}`,
        name,
        trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
        trackGroup: uuid,
      });
    }
  }

  async addUserAsyncSliceTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      with grouped_packages as materialized (
        select
          uid,
          group_concat(package_name, ',') as package_name,
          count() as cnt
        from package_list
        group by uid
      )
      select
        t.name as name,
        t.uid as uid,
        iif(g.cnt = 1, g.package_name, 'UID ' || g.uid) as packageName
      from _uid_track_track_summary_by_uid_and_name t
      left join grouped_packages g using (uid)
    `);

    const it = result.iter({
      name: STR_NULL,
      uid: NUM_NULL,
      packageName: STR_NULL,
    });

    // Map From [name] -> [uuid, key]
    const groupMap = new Map<string, string>();

    for (; it.valid(); it.next()) {
      if (it.name == null || it.uid == null) {
        continue;
      }
      const rawName = it.name;
      const uid = it.uid === null ? undefined : it.uid;
      const userName = it.packageName === null ? `UID ${uid}` : it.packageName;

      const groupUuid = `uid-track-group${rawName}`;
      if (groupMap.get(rawName) === undefined) {
        groupMap.set(rawName, groupUuid);
      }

      this.tracksToAdd.push({
        uri: `perfetto.AsyncSlices#${rawName}.${uid}`,
        name: userName,
        trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
        trackGroup: groupUuid,
      });
    }

    for (const [name, groupUuid] of groupMap) {
      const addGroup = Actions.addTrackGroup({
        name: name,
        key: groupUuid,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }
  }

  async addActualFramesTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      select
        upid,
        t.name as trackName,
        process.name as processName,
        process.pid as pid
      from _process_track_summary_by_upid_and_name t
      join process using(upid)
      where t.name = "Actual Timeline"
    `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      processName: STR_NULL,
      pid: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const processName = it.processName;
      const pid = it.pid;

      const uuid = this.getUuid(null, upid);
      const kind = ACTUAL_FRAMES_SLICE_TRACK_KIND;
      const name = getTrackName({
        name: trackName,
        upid,
        pid,
        processName,
        kind,
      });

      this.tracksToAdd.push({
        uri: `perfetto.ActualFrames#${upid}`,
        name,
        trackSortKey: PrimaryTrackSortKey.ACTUAL_FRAMES_SLICE_TRACK,
        trackGroup: uuid,
      });
    }
  }

  async addExpectedFramesTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      select
        upid,
        t.name as trackName,
        process.name as processName,
        process.pid as pid
      from _process_track_summary_by_upid_and_name t
      join process using(upid)
      where t.name = "Expected Timeline"
    `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      processName: STR_NULL,
      pid: NUM_NULL,
    });

    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const processName = it.processName;
      const pid = it.pid;

      const uuid = this.getUuid(null, upid);
      const kind = EXPECTED_FRAMES_SLICE_TRACK_KIND;
      const name = getTrackName({
        name: trackName,
        upid,
        pid,
        processName,
        kind,
      });

      this.tracksToAdd.push({
        uri: `perfetto.ExpectedFrames#${upid}`,
        name,
        trackSortKey: PrimaryTrackSortKey.EXPECTED_FRAMES_SLICE_TRACK,
        trackGroup: uuid,
      });
    }
  }

  async addThreadSliceTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      select
        thread_track.utid as utid,
        thread_track.id as trackId,
        thread_track.name as trackName,
        EXTRACT_ARG(thread_track.source_arg_set_id,
                    'is_root_in_scope') as isDefaultTrackForScope,
        tid,
        thread.name as threadName,
        thread.upid as upid
      from thread_track
      join _slice_track_summary using (id)
      join thread using(utid)
  `);

    const it = result.iter({
      utid: NUM,
      trackId: NUM,
      trackName: STR_NULL,
      isDefaultTrackForScope: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
      upid: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const trackId = it.trackId;
      const trackName = it.trackName;
      // Note that !!null === false.
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      const isDefaultTrackForScope = !!it.isDefaultTrackForScope;
      const tid = it.tid;
      const threadName = it.threadName;
      const upid = it.upid;

      const uuid = this.getUuid(utid, upid);

      const kind = THREAD_SLICE_TRACK_KIND;
      const name = getTrackName({name: trackName, utid, tid, threadName, kind});

      this.tracksToAdd.push({
        uri: `perfetto.ThreadSlices#${trackId}`,
        name,
        trackGroup: uuid,
        trackSortKey: {
          utid,
          priority: isDefaultTrackForScope
            ? InThreadTrackSortKey.DEFAULT_TRACK
            : InThreadTrackSortKey.ORDINARY,
        },
      });
    }
  }

  async addProcessCounterTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      select
        process_counter_track.id as trackId,
        process_counter_track.name as trackName,
        upid,
        process.pid,
        process.name as processName
      from process_counter_track
      join _counter_track_summary using (id)
      join process using(upid);
  `);
    const it = result.iter({
      trackId: NUM,
      trackName: STR_NULL,
      upid: NUM,
      pid: NUM_NULL,
      processName: STR_NULL,
    });
    for (let i = 0; it.valid(); ++i, it.next()) {
      const pid = it.pid;
      const upid = it.upid;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const processName = it.processName;
      const uuid = this.getUuid(null, upid);
      const name = getTrackName({
        name: trackName,
        upid,
        pid,
        kind: COUNTER_TRACK_KIND,
        processName,
      });
      this.tracksToAdd.push({
        uri: `perfetto.Counter#process${trackId}`,
        name,
        trackSortKey: await this.resolveTrackSortKeyForProcessCounterTrack(
          upid,
          trackName || undefined,
        ),
        trackGroup: uuid,
      });
    }
  }

  async addProcessHeapProfileTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      select upid
      from _process_available_info_summary
      where allocation_count > 0 or graph_object_count > 0
  `);
    for (const it = result.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const uuid = this.getUuid(null, upid);
      this.tracksToAdd.push({
        uri: `perfetto.HeapProfile#${upid}`,
        trackSortKey: PrimaryTrackSortKey.HEAP_PROFILE_TRACK,
        name: `Heap Profile`,
        trackGroup: uuid,
      });
    }
  }

  async addProcessPerfSamplesTracks(engine: Engine): Promise<void> {
    const result = await engine.query(`
      select upid, pid
      from _process_available_info_summary
      join process using (upid)
      where perf_sample_count > 0
  `);
    for (const it = result.iter({upid: NUM, pid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const pid = it.pid;
      const uuid = this.getUuid(null, upid);
      this.tracksToAdd.push({
        uri: `perfetto.PerfSamplesProfile#${upid}`,
        trackSortKey: PrimaryTrackSortKey.PERF_SAMPLES_PROFILE_TRACK,
        name: `Callstacks ${pid}`,
        trackGroup: uuid,
      });
    }
  }

  getUuidUnchecked(utid: number | null, upid: number | null) {
    return upid === null
      ? this.utidToUuid.get(utid!)
      : this.upidToUuid.get(upid);
  }

  getUuid(utid: number | null, upid: number | null) {
    return assertExists(this.getUuidUnchecked(utid, upid));
  }

  getOrCreateUuid(utid: number | null, upid: number | null) {
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

  setUuidForUpid(upid: number, uuid: string) {
    this.upidToUuid.set(upid, uuid);
  }

  async addKernelThreadGrouping(engine: Engine): Promise<void> {
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
      uri: 'perfetto.ProcessSummary#kernel',
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

  async addProcessTrackGroups(engine: Engine): Promise<void> {
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
      chromeProcessLabels: STR,
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
      const type = hasSched ? 'schedule' : 'summary';
      const uri = `perfetto.ProcessScheduling#${upid}.${utid}.${type}`;

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
        labels: it.chromeProcessLabels.split(','),
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
        name,
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

  addPluginTracks(): void {
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

      const key = uuidv4();

      this.tracksToAdd.push({
        uri: info.uri,
        name: info.displayName,
        key,
        // TODO(hjd): Fix how sorting works. Plugins should expose
        // 'sort keys' which the user can use to choose a sort order.
        trackSortKey: info.sortKey ?? PrimaryTrackSortKey.ORDINARY_TRACK,
        trackGroup: groupUuid,
      });

      if (info.isPinned) {
        this.tracksToPin.push(key);
      }
    }
  }

  async addScrollJankPluginTracks(): Promise<void> {
    if (ENABLE_SCROLL_JANK_PLUGIN_V2.get()) {
      const result = await getScrollJankTracks(this.engine);
      this.tracksToAdd = this.tracksToAdd.concat(result.tracks.tracksToAdd);
      this.addTrackGroupActions.push(result.addTrackGroup);
    }
  }

  async decideTracks(): Promise<DeferredAction[]> {
    {
      const result = screenshotDecideTracks(this.engine);
      if (result !== null) {
        const {tracksToAdd} = await result;
        this.tracksToAdd.push(...tracksToAdd);
      }
    }

    // Add first the global tracks that don't require per-process track groups.
    await this.addScrollJankPluginTracks();
    await this.addCpuSchedulingTracks();
    await this.addCpuFreqTracks(
      this.engine.getProxy('TrackDecider::addCpuFreqTracks'),
    );
    await this.addGlobalAsyncTracks(
      this.engine.getProxy('TrackDecider::addGlobalAsyncTracks'),
    );
    await this.addGpuFreqTracks(
      this.engine.getProxy('TrackDecider::addGpuFreqTracks'),
    );
    await this.addCpuFreqLimitCounterTracks(
      this.engine.getProxy('TrackDecider::addCpuFreqLimitCounterTracks'),
    );
    await this.addCpuPerfCounterTracks(
      this.engine.getProxy('TrackDecider::addCpuPerfCounterTracks'),
    );
    this.addPluginTracks();
    await this.addAnnotationTracks(
      this.engine.getProxy('TrackDecider::addAnnotationTracks'),
    );
    await this.groupGlobalIonTracks();
    await this.groupGlobalIostatTracks(F2FS_IOSTAT_TAG, F2FS_IOSTAT_GROUP_NAME);
    await this.groupGlobalIostatTracks(
      F2FS_IOSTAT_LAT_TAG,
      F2FS_IOSTAT_LAT_GROUP_NAME,
    );
    await this.groupGlobalIostatTracks(DISK_IOSTAT_TAG, DISK_IOSTAT_GROUP_NAME);
    await this.groupTracksByRegex(UFS_CMD_TAG_REGEX, UFS_CMD_TAG_GROUP);
    await this.groupGlobalBuddyInfoTracks();
    await this.groupTracksByRegex(KERNEL_WAKELOCK_REGEX, KERNEL_WAKELOCK_GROUP);
    await this.groupTracksByRegex(NETWORK_TRACK_REGEX, NETWORK_TRACK_GROUP);
    await this.groupTracksByRegex(
      ENTITY_RESIDENCY_REGEX,
      ENTITY_RESIDENCY_GROUP,
    );
    await this.groupTracksByRegex(UCLAMP_REGEX, UCLAMP_GROUP);
    await this.groupFrequencyTracks(FREQUENCY_GROUP);
    await this.groupTracksByRegex(POWER_RAILS_REGEX, POWER_RAILS_GROUP);
    await this.groupTracksByRegex(TEMPERATURE_REGEX, TEMPERATURE_GROUP);
    await this.groupTracksByRegex(IRQ_REGEX, IRQ_GROUP);
    await this.groupTracksByRegex(CHROME_TRACK_REGEX, CHROME_TRACK_GROUP);
    await this.groupMiscNonAllowlistedTracks(MISC_GROUP);

    // Add user slice tracks before listing the processes. These tracks will
    // be listed with their user/package name only, and they will be grouped
    // under on their original shared track names. E.g. "GPU Work Period"
    await this.addUserAsyncSliceTracks(
      this.engine.getProxy('TrackDecider::addUserAsyncSliceTracks'),
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

    await this.addProcessHeapProfileTracks(
      this.engine.getProxy('TrackDecider::addProcessHeapProfileTracks'),
    );
    if (PERF_SAMPLE_FLAG.get()) {
      await this.addProcessPerfSamplesTracks(
        this.engine.getProxy('TrackDecider::addProcessPerfSamplesTracks'),
      );
    }
    await this.addProcessCounterTracks(
      this.engine.getProxy('TrackDecider::addProcessCounterTracks'),
    );
    await this.addProcessAsyncSliceTracks(
      this.engine.getProxy('TrackDecider::addProcessAsyncSliceTracks'),
    );
    await this.addActualFramesTracks(
      this.engine.getProxy('TrackDecider::addActualFramesTracks'),
    );
    await this.addExpectedFramesTracks(
      this.engine.getProxy('TrackDecider::addExpectedFramesTracks'),
    );
    await this.addThreadCounterTracks(
      this.engine.getProxy('TrackDecider::addThreadCounterTracks'),
    );
    await this.addThreadStateTracks(
      this.engine.getProxy('TrackDecider::addThreadStateTracks'),
    );
    await this.addThreadSliceTracks(
      this.engine.getProxy('TrackDecider::addThreadSliceTracks'),
    );
    await this.addThreadCpuSampleTracks(
      this.engine.getProxy('TrackDecider::addThreadCpuSampleTracks'),
    );

    // TODO(hjd): Move into plugin API.
    {
      const result = scrollJankDecideTracks(this.engine, (utid, upid) => {
        return this.getUuid(utid, upid);
      });
      if (result !== null) {
        const {tracksToAdd} = await result;
        this.tracksToAdd.push(...tracksToAdd);
      }
    }

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

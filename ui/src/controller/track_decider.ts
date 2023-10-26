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
import {
  Actions,
  AddTrackArgs,
  DeferredAction,
} from '../common/actions';
import {Engine, EngineProxy} from '../common/engine';
import {featureFlags, PERF_SAMPLE_FLAG} from '../common/feature_flags';
import {pluginManager} from '../common/plugins';
import {
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../common/query_result';
import {
  InThreadTrackSortKey,
  SCROLLING_TRACK_GROUP,
  TrackSortKey,
  UtidToTrackSortKey,
} from '../common/state';
import {PrimaryTrackSortKey} from '../public';
import {getTrackName} from '../public/utils';
import {ACTUAL_FRAMES_SLICE_TRACK_KIND} from '../tracks/actual_frames';
import {ASYNC_SLICE_TRACK_KIND} from '../tracks/async_slices';
import {
  ENABLE_SCROLL_JANK_PLUGIN_V2,
  getScrollJankTracks,
} from '../tracks/chrome_scroll_jank';
import {
  decideTracks as scrollJankDecideTracks,
} from '../tracks/chrome_scroll_jank/chrome_tasks_scroll_jank_track';
import {SLICE_TRACK_KIND} from '../tracks/chrome_slices';
import {COUNTER_TRACK_KIND} from '../tracks/counter';
import {EXPECTED_FRAMES_SLICE_TRACK_KIND} from '../tracks/expected_frames';
import {NULL_TRACK_URI} from '../tracks/null_track';
import {
  decideTracks as screenshotDecideTracks,
} from '../tracks/screenshots';
import {THREAD_STATE_TRACK_KIND} from '../tracks/thread_state';

const TRACKS_V2_FLAG = featureFlags.register({
  id: 'tracksV2.1',
  name: 'Tracks V2',
  description: 'Show tracks built on top of the Track V2 API.',
  defaultValue: false,
});

const TRACKS_V2_COMPARE_FLAG = featureFlags.register({
  id: 'tracksV2Compare',
  name: 'Tracks V2: Also show V1 tracks',
  description:
      'Show V1 tracks side by side with V2 tracks. Does nothing if TracksV2 is not enabled.',
  defaultValue: false,
});

function showV2(): boolean {
  return TRACKS_V2_FLAG.get();
}

function showV1(): boolean {
  return !showV2() || (showV2() && TRACKS_V2_COMPARE_FLAG.get());
}

const MEM_DMA_COUNTER_NAME = 'mem.dma_heap';
const MEM_DMA = 'mem.dma_buffer';
const MEM_ION = 'mem.ion';
const F2FS_IOSTAT_TAG = 'f2fs_iostat.';
const F2FS_IOSTAT_GROUP_NAME = 'f2fs_iostat';
const F2FS_IOSTAT_LAT_TAG = 'f2fs_iostat_latency.';
const F2FS_IOSTAT_LAT_GROUP_NAME = 'f2fs_iostat_latency';
const DISK_IOSTAT_TAG = 'diskstat.';
const DISK_IOSTAT_GROUP_NAME = 'diskstat';
const UFS_CMD_TAG = 'io.ufs.command.tag';
const UFS_CMD_TAG_GROUP_NAME = 'io.ufs.command.tags';
const BUDDY_INFO_TAG = 'mem.buddyinfo';
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
const IRQ_REGEX = new RegExp('^Irq Cpu.*');
const CHROME_TRACK_REGEX = new RegExp('^Chrome.*|^InputLatency::.*');
const CHROME_TRACK_GROUP = 'Chrome Global Tracks';
const MISC_GROUP = 'Misc Global Tracks';

export async function decideTracks(engine: Engine): Promise<DeferredAction[]> {
  return (new TrackDecider(engine)).decideTracks();
}

class TrackDecider {
  private engine: Engine;
  private upidToUuid = new Map<number, string>();
  private utidToUuid = new Map<number, string>();
  private tracksToAdd: AddTrackArgs[] = [];
  private addTrackGroupActions: DeferredAction[] = [];

  constructor(engine: Engine) {
    this.engine = engine;
  }

  async guessCpuSizes(): Promise<Map<number, string>> {
    const cpuToSize = new Map<number, string>();
    await this.engine.query(`
      INCLUDE PERFETTO MODULE common.cpus;
    `);
    const result = await this.engine.query(`
      SELECT cpu, GUESS_CPU_SIZE(cpu) as size FROM cpu_counter_track;
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
    const cpus = await this.engine.getCpus();
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

  async addCpuFreqTracks(engine: EngineProxy): Promise<void> {
    const cpus = await this.engine.getCpus();

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

  async addGlobalAsyncTracks(engine: EngineProxy): Promise<void> {
    const rawGlobalAsyncTracks = await engine.query(`
      with tracks_with_slices as materialized (
        select distinct track_id
        from slice
      ),
      global_tracks as (
        select
          track.parent_id as parent_id,
          track.id as track_id,
          track.name as name
        from track
        join tracks_with_slices on tracks_with_slices.track_id = track.id
        where
          track.type = "track"
          or track.type = "gpu_track"
          or track.type = "cpu_track"
      ),
      global_tracks_grouped as (
        select
          parent_id,
          name,
          group_concat(track_id) as trackIds,
          count(track_id) as trackCount
        from global_tracks track
        group by parent_id, name
      )
      select
        t.parent_id as parentId,
        p.name as parentName,
        t.name as name,
        t.trackIds as trackIds,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from global_tracks_grouped AS t
      left join track p on (t.parent_id = p.id)
      order by p.name, t.name;
    `);
    const it = rawGlobalAsyncTracks.iter({
      name: STR_NULL,
      parentName: STR_NULL,
      parentId: NUM_NULL,
      maxDepth: NUM_NULL,
    });

    const parentIdToGroupId = new Map<number, string>();

    for (; it.valid(); it.next()) {
      const kind = ASYNC_SLICE_TRACK_KIND;
      const rawName = it.name === null ? undefined : it.name;
      const rawParentName = it.parentName === null ? undefined : it.parentName;
      const name = getTrackName({name: rawName, kind});
      const parentTrackId = it.parentId;
      const maxDepth = it.maxDepth;
      let trackGroup = SCROLLING_TRACK_GROUP;

      // If there are no slices in this track, skip it.
      if (maxDepth === null) {
        continue;
      }

      if (parentTrackId !== null) {
        const groupId = parentIdToGroupId.get(parentTrackId);
        if (groupId === undefined) {
          trackGroup = uuidv4();
          parentIdToGroupId.set(parentTrackId, trackGroup);

          const parentName = getTrackName({name: rawParentName, kind});

          const summaryTrackKey = uuidv4();
          this.tracksToAdd.push({
            uri: NULL_TRACK_URI,
            key: summaryTrackKey,
            trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
            trackGroup: undefined,
            name: parentName,
          });

          this.addTrackGroupActions.push(Actions.addTrackGroup({
            summaryTrackKey: summaryTrackKey,
            name: parentName,
            id: trackGroup,
            collapsed: true,
          }));
        } else {
          trackGroup = groupId;
        }
      }

      const track: AddTrackArgs = {
        uri: `perfetto.AsyncSlices#${rawName}`,
        trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
        trackGroup,
        name,
      };

      this.tracksToAdd.push(track);
    }
  }

  async addGpuFreqTracks(engine: EngineProxy): Promise<void> {
    const numGpus = await this.engine.getNumberOfGpus();
    for (let gpu = 0; gpu < numGpus; gpu++) {
      // Only add a gpu freq track if we have
      // gpu freq data.
      const freqExistsResult = await engine.query(`
      select *
      from gpu_counter_track
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

  async addCpuFreqLimitCounterTracks(engine: EngineProxy): Promise<void> {
    const cpuFreqLimitCounterTracksSql = `
      select name, id
      from cpu_counter_track
      where name glob "Cpu * Freq Limit"
      order by name asc
    `;

    this.addCpuCounterTracks(engine, cpuFreqLimitCounterTracksSql);
  }

  async addCpuPerfCounterTracks(engine: EngineProxy): Promise<void> {
    // Perf counter tracks are bound to CPUs, follow the scheduling and
    // frequency track naming convention ("Cpu N ...").
    // Note: we might not have a track for a given cpu if no data was seen from
    // it. This might look surprising in the UI, but placeholder tracks are
    // wasteful as there's no way of collapsing global counter tracks at the
    // moment.
    const addCpuPerfCounterTracksSql = `
      select printf("Cpu %u %s", cpu, name) as name, id
      from perf_counter_track as pct
      order by perf_session_id asc, pct.name asc, cpu asc
    `;
    this.addCpuCounterTracks(engine, addCpuPerfCounterTracksSql);
  }

  async addCpuCounterTracks(engine: EngineProxy, sql: string): Promise<void> {
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

    const id = uuidv4();
    const summaryTrackKey = uuidv4();
    let foundSummary = false;

    for (const track of ionTracks) {
      if (!foundSummary &&
          [MEM_DMA_COUNTER_NAME, MEM_ION].includes(track.name)) {
        foundSummary = true;
        track.key = summaryTrackKey;
        track.trackGroup = undefined;
      } else {
        track.trackGroup = id;
      }
    }

    const addGroup = Actions.addTrackGroup({
      summaryTrackKey,
      name: MEM_DMA_COUNTER_NAME,
      id,
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
      const summaryTrackKey = uuidv4();

      this.tracksToAdd.push({
        uri: NULL_TRACK_URI,
        key: summaryTrackKey,
        trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
        name: groupName,
        trackGroup: undefined,
      });

      const addGroup = Actions.addTrackGroup({
        summaryTrackKey,
        name: groupName,
        id: value,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }
  }

  async groupGlobalUfsCmdTagTracks(tag: string, group: string): Promise<void> {
    const ufsCmdTagTracks: AddTrackArgs[] = [];

    for (const track of this.tracksToAdd) {
      if (track.name.startsWith(tag)) {
        ufsCmdTagTracks.push(track);
      }
    }

    if (ufsCmdTagTracks.length === 0) {
      return;
    }

    const id = uuidv4();
    const summaryTrackKey = uuidv4();
    ufsCmdTagTracks[0].key = summaryTrackKey;
    for (const track of ufsCmdTagTracks) {
      track.trackGroup = id;
    }

    const addGroup = Actions.addTrackGroup({
      summaryTrackKey,
      name: group,
      id,
      collapsed: true,
    });
    this.addTrackGroupActions.push(addGroup);
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
      track.name = 'Size: ' + size;
      track.trackGroup = devMap.get(groupName);
    }

    for (const [key, value] of devMap) {
      const groupName = key;
      const summaryTrackKey = uuidv4();

      this.tracksToAdd.push({
        uri: NULL_TRACK_URI,
        key: summaryTrackKey,
        trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
        name: groupName,
        trackGroup: undefined,
      });

      const addGroup = Actions.addTrackGroup({
        summaryTrackKey,
        name: groupName,
        id: value,
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
      if (track.name.endsWith('Frequency') && !track.name.startsWith('Cpu') &&
          !track.name.startsWith('Gpu')) {
        if (track.trackGroup !== undefined &&
            track.trackGroup !== SCROLLING_TRACK_GROUP) {
          continue;
        }
        if (track.uri === NULL_TRACK_URI) {
          continue;
        }
        if (groupUuid === undefined) {
          groupUuid = uuidv4();
        }
        track.trackGroup = groupUuid;
      }
    }

    if (groupUuid !== undefined) {
      const summaryTrackKey = uuidv4();
      this.tracksToAdd.push({
        uri: NULL_TRACK_URI,
        key: summaryTrackKey,
        trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
        name: groupName,
        trackGroup: undefined,
      });

      const addGroup = Actions.addTrackGroup({
        summaryTrackKey,
        name: groupName,
        id: groupUuid,
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
    ];

    let groupUuid = undefined;
    for (const track of this.tracksToAdd) {
      if (track.trackGroup !== undefined &&
          track.trackGroup !== SCROLLING_TRACK_GROUP) {
        continue;
      }
      if (track.uri === NULL_TRACK_URI) {
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
      const summaryTrackKey = uuidv4();
      this.tracksToAdd.push({
        uri: NULL_TRACK_URI,
        key: summaryTrackKey,
        trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
        name: groupName,
        trackGroup: undefined,
      });

      const addGroup = Actions.addTrackGroup({
        summaryTrackKey,
        name: groupName,
        id: groupUuid,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }
  }

  async groupTracksByRegex(regex: RegExp, groupName: string): Promise<void> {
    let groupUuid = undefined;

    for (const track of this.tracksToAdd) {
      if (regex.test(track.name)) {
        if (track.trackGroup !== undefined &&
            track.trackGroup !== SCROLLING_TRACK_GROUP) {
          continue;
        }
        if (track.uri === NULL_TRACK_URI) {
          continue;
        }
        if (groupUuid === undefined) {
          groupUuid = uuidv4();
        }
        track.trackGroup = groupUuid;
      }
    }

    if (groupUuid !== undefined) {
      const summaryTrackKey = uuidv4();
      this.tracksToAdd.push({
        uri: NULL_TRACK_URI,
        key: summaryTrackKey,
        trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
        name: groupName,
        trackGroup: undefined,
      });

      const addGroup = Actions.addTrackGroup({
        summaryTrackKey: summaryTrackKey,
        name: groupName,
        id: groupUuid,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }
  }

  async addLogsTrack(engine: EngineProxy): Promise<void> {
    const result =
        await engine.query(`select count(1) as cnt from android_logs`);
    const count = result.firstRow({cnt: NUM}).cnt;

    if (count > 0) {
      this.tracksToAdd.push({
        uri: 'perfetto.AndroidLog',
        name: 'Android logs',
        trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
        trackGroup: SCROLLING_TRACK_GROUP,
      });
    }
  }

  async addFtraceTrack(engine: EngineProxy): Promise<void> {
    const query = 'select distinct cpu from ftrace_event';

    const result = await engine.query(query);
    const it = result.iter({cpu: NUM});

    let groupUuid = undefined;
    let summaryTrackKey = undefined;

    // use the first one as the summary track
    for (let row = 0; it.valid(); it.next(), row++) {
      if (groupUuid === undefined) {
        groupUuid = 'ftrace-track-group';
        summaryTrackKey = uuidv4();
        this.tracksToAdd.push({
          uri: NULL_TRACK_URI,
          trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
          name: `Ftrace Events`,
          trackGroup: undefined,
          key: summaryTrackKey,
        });
      }
      this.tracksToAdd.push({
        uri: `perfetto.FtraceRaw#cpu${it.cpu}`,
        trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
        name: `Ftrace Events Cpu ${it.cpu}`,
        trackGroup: groupUuid,
      });
    }

    if (groupUuid !== undefined && summaryTrackKey !== undefined) {
      const addGroup = Actions.addTrackGroup({
        name: 'Ftrace Events',
        id: groupUuid,
        collapsed: true,
        summaryTrackKey,
      });
      this.addTrackGroupActions.push(addGroup);
    }
  }

  async addAnnotationTracks(engine: EngineProxy): Promise<void> {
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

    const groupNameToIds = new Map<string, GroupIds>();

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
        const groupIds = groupNameToIds.get(groupName);
        if (groupIds) {
          trackGroupId = groupIds.id;
        } else {
          trackGroupId = uuidv4();
          summaryTrackKey = uuidv4();
          groupNameToIds.set(groupName, {
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

    for (const [groupName, groupIds] of groupNameToIds) {
      const addGroup = Actions.addTrackGroup({
        summaryTrackKey: groupIds.summaryTrackKey,
        name: groupName,
        id: groupIds.id,
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
        trackGroup: upid === 0 ? SCROLLING_TRACK_GROUP :
                                 this.upidToUuid.get(upid),
      });
    }
  }

  async addThreadStateTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
      select
        utid,
        tid,
        upid,
        pid,
        thread.name as threadName
      from
        thread_state
        left join thread using(utid)
        left join process using(upid)
      where utid != 0
      group by utid`);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      pid: NUM_NULL,
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

      if (showV1()) {
        const kind = THREAD_STATE_TRACK_KIND;
        this.tracksToAdd.push({
          uri: `perfetto.ThreadState#${upid}.${utid}`,
          name: getTrackName({utid, tid, threadName, kind}),
          trackGroup: uuid,
          trackSortKey: {
            utid,
            priority,
          },
        });
      }

      if (showV2()) {
        this.tracksToAdd.push({
          uri: `perfetto.ThreadState#${utid}.v2`,
          name:
              getTrackName({utid, tid, threadName, kind: 'ThreadStateTrackV2'}),
          trackGroup: uuid,
          trackSortKey: {
            utid,
            priority,
          },
        });
      }
    }
  }

  async addThreadCpuSampleTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
      select
        utid,
        tid,
        upid,
        thread.name as threadName
      from
        thread
        join (select utid
            from cpu_profile_stack_sample group by utid
        ) using(utid)
        left join process using(upid)
      where utid != 0
      group by utid`);

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

  async addThreadCounterTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
    select
      thread_counter_track.name as trackName,
      utid,
      upid,
      tid,
      thread.name as threadName,
      thread_counter_track.id as trackId
    from thread_counter_track
    join thread using(utid)
    left join process using(upid)
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

  async addProcessAsyncSliceTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
      with process_async_tracks as materialized (
        select
          process_track.upid as upid,
          process_track.name as trackName,
          process.name as processName,
          process.pid as pid,
          group_concat(process_track.id) as trackIds,
          count(1) as trackCount
        from process_track
        left join process using(upid)
        where
            process_track.name is null or
            process_track.name not like "% Timeline"
        group by
          process_track.upid,
          process_track.name
      )
      select
        t.*,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from process_async_tracks t;
    `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      if (maxDepth === null) {
        // If there are no slices in this track, skip it.
        continue;
      }

      const uuid = this.getUuid(0, upid);
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

  async addActualFramesTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
      with process_async_tracks as materialized (
        select
          process_track.upid as upid,
          process_track.name as trackName,
          process.name as processName,
          process.pid as pid,
          group_concat(process_track.id) as trackIds,
          count(1) as trackCount
        from process_track
        left join process using(upid)
        where process_track.name = "Actual Timeline"
        group by
          process_track.upid,
          process_track.name
      )
      select
        t.*,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from process_async_tracks t;
  `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      if (maxDepth === null) {
        // If there are no slices in this track, skip it.
        continue;
      }

      const uuid = this.getUuid(0, upid);

      const kind = ACTUAL_FRAMES_SLICE_TRACK_KIND;
      const name =
          getTrackName({name: trackName, upid, pid, processName, kind});
      this.tracksToAdd.push({
        uri: `perfetto.ActualFrames#${upid}`,
        name,
        trackSortKey: PrimaryTrackSortKey.ACTUAL_FRAMES_SLICE_TRACK,
        trackGroup: uuid,
      });
    }
  }

  async addExpectedFramesTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
      with process_async_tracks as materialized (
        select
          process_track.upid as upid,
          process_track.name as trackName,
          process.name as processName,
          process.pid as pid,
          group_concat(process_track.id) as trackIds,
          count(1) as trackCount
        from process_track
        left join process using(upid)
        where process_track.name = "Expected Timeline"
        group by
          process_track.upid,
          process_track.name
      )
      select
        t.*,
        max_layout_depth(t.trackCount, t.trackIds) as maxDepth
      from process_async_tracks t;
  `);

    const it = result.iter({
      upid: NUM,
      trackName: STR_NULL,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM_NULL,
    });

    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      if (maxDepth === null) {
        // If there are no slices in this track, skip it.
        continue;
      }

      const uuid = this.getUuid(0, upid);

      const kind = EXPECTED_FRAMES_SLICE_TRACK_KIND;
      const name =
          getTrackName({name: trackName, upid, pid, processName, kind});
      this.tracksToAdd.push({
        uri: `perfetto.ExpectedFrames#${upid}`,
        name,
        trackSortKey: PrimaryTrackSortKey.EXPECTED_FRAMES_SLICE_TRACK,
        trackGroup: uuid,
      });
    }
  }

  async addThreadSliceTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
        select
          thread_track.utid as utid,
          thread_track.id as trackId,
          thread_track.name as trackName,
          EXTRACT_ARG(thread_track.source_arg_set_id,
                      'is_root_in_scope') as isDefaultTrackForScope,
          tid,
          thread.name as threadName,
          process.upid as upid
        from slice
        join thread_track on slice.track_id = thread_track.id
        join thread using(utid)
        left join process using(upid)
        group by thread_track.id
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
      const isDefaultTrackForScope = !!it.isDefaultTrackForScope;
      const tid = it.tid;
      const threadName = it.threadName;
      const upid = it.upid;

      const uuid = this.getUuid(utid, upid);

      const kind = SLICE_TRACK_KIND;
      const name = getTrackName({name: trackName, utid, tid, threadName, kind});
      if (showV1()) {
        this.tracksToAdd.push({
          uri: `perfetto.ChromeSlices#${trackId}`,
          name,
          trackGroup: uuid,
          trackSortKey: {
            utid,
            priority: isDefaultTrackForScope ?
                InThreadTrackSortKey.DEFAULT_TRACK :
                InThreadTrackSortKey.ORDINARY,
          },
        });
      }

      if (showV2()) {
        this.tracksToAdd.push({
          uri: `perfetto.ChromeSlices#${trackId}.v2`,
          name,
          trackGroup: uuid,
          trackSortKey: {
            utid,
            priority: isDefaultTrackForScope ?
                InThreadTrackSortKey.DEFAULT_TRACK :
                InThreadTrackSortKey.ORDINARY,
          },
        });
      }
    }
  }

  async addProcessCounterTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
    select
      process_counter_track.id as trackId,
      process_counter_track.name as trackName,
      upid,
      process.pid,
      process.name as processName
    from process_counter_track
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
      const uuid = this.getUuid(0, upid);
      const name = getTrackName(
          {name: trackName, upid, pid, kind: COUNTER_TRACK_KIND, processName});
      this.tracksToAdd.push({
        uri: `perfetto.Counter#process${trackId}`,
        name,
        trackSortKey: await this.resolveTrackSortKeyForProcessCounterTrack(
            upid, trackName || undefined),
        trackGroup: uuid,
      });
    }
  }

  async addProcessHeapProfileTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
    select distinct(upid) from heap_profile_allocation
    union
    select distinct(upid) from heap_graph_object
  `);
    for (const it = result.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const uuid = this.getUuid(0, upid);
      this.tracksToAdd.push({
        uri: `perfetto.HeapProfile#${upid}`,
        trackSortKey: PrimaryTrackSortKey.HEAP_PROFILE_TRACK,
        name: `Heap Profile`,
        trackGroup: uuid,
      });
    }
  }

  async addProcessPerfSamplesTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
      select distinct upid, pid
      from perf_sample join thread using (utid) join process using (upid)
      where callsite_id is not null
  `);
    for (const it = result.iter({upid: NUM, pid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const pid = it.pid;
      const uuid = this.getUuid(0, upid);
      this.tracksToAdd.push({
        uri: `perfetto.PerfSamplesProfile#${upid}`,
        trackSortKey: PrimaryTrackSortKey.PERF_SAMPLES_PROFILE_TRACK,
        name: `Callstacks ${pid}`,
        trackGroup: uuid,
      });
    }
  }

  getUuidUnchecked(utid: number, upid: number|null) {
    return upid === null ? this.utidToUuid.get(utid) :
                           this.upidToUuid.get(upid);
  }

  getUuid(utid: number, upid: number|null) {
    return assertExists(this.getUuidUnchecked(utid, upid));
  }

  getOrCreateUuid(utid: number, upid: number|null) {
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

  setUuidForUpid(upid: number, uuid: string) {
    this.upidToUuid.set(upid, uuid);
  }

  async addKernelThreadGrouping(engine: EngineProxy): Promise<void> {
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
      id: kthreadGroupUuid,
      collapsed: true,
    });
    this.addTrackGroupActions.push(addTrackGroup);

    // Set the group for all kernel threads (including kthreadd itself).
    for (; it.valid(); it.next()) {
      this.setUuidForUpid(it.upid, kthreadGroupUuid);
    }
  }

  async addProcessTrackGroups(engine: EngineProxy): Promise<void> {
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
      chromeProcessLabels: STR,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const upid = it.upid;
      const pid = it.pid;
      const threadName = it.threadName;
      const processName = it.processName;
      const hasSched = !!it.hasSched;
      const hasHeapProfiles = !!it.hasHeapProfiles;

      // Group by upid if present else by utid.
      let pUuid =
          upid === null ? this.utidToUuid.get(utid) : this.upidToUuid.get(upid);
      // These should only happen once for each track group.
      if (pUuid === undefined) {
        pUuid = this.getOrCreateUuid(utid, upid);
        const summaryTrackKey = uuidv4();
        const type = hasSched ? 'schedule' : 'summary';
        const uri = `perfetto.ProcessScheduling#${upid}.${utid}.${type}`;

        this.tracksToAdd.push({
          uri,
          key: summaryTrackKey,
          trackSortKey: hasSched ?
              PrimaryTrackSortKey.PROCESS_SCHEDULING_TRACK :
              PrimaryTrackSortKey.PROCESS_SUMMARY_TRACK,
          name: `${upid === null ? tid : pid} summary`,
          labels: it.chromeProcessLabels.split(','),
        });

        const name =
            getTrackName({utid, processName, pid, threadName, tid, upid});
        const addTrackGroup = Actions.addTrackGroup({
          summaryTrackKey,
          name,
          id: pUuid,
          // Perf profiling tracks remain collapsed, otherwise we would have too
          // many expanded process tracks for some perf traces, leading to
          // jankyness.
          collapsed: !hasHeapProfiles,
        });

        this.addTrackGroupActions.push(addTrackGroup);
      }
    }
  }

  private async computeThreadOrderingMetadata(): Promise<UtidToTrackSortKey> {
    const result = await this.engine.query(`
    select
      utid,
      tid,
      pid,
      thread.name as threadName
    from thread
    left join process using(upid)`);

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
    const tracks = pluginManager.findPotentialTracks();
    for (const info of tracks) {
      this.tracksToAdd.push({
        uri: info.uri,
        name: info.displayName,
        // TODO(hjd): Fix how sorting works. Plugins should expose
        // 'sort keys' which the user can use to choose a sort order.
        trackSortKey: info.sortKey ?? PrimaryTrackSortKey.ORDINARY_TRACK,
        trackGroup: SCROLLING_TRACK_GROUP,
      });
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
    await this.addFtraceTrack(
        this.engine.getProxy('TrackDecider::addFtraceTrack'));
    await this.addCpuFreqTracks(
        this.engine.getProxy('TrackDecider::addCpuFreqTracks'));
    await this.addGlobalAsyncTracks(
        this.engine.getProxy('TrackDecider::addGlobalAsyncTracks'));
    await this.addGpuFreqTracks(
        this.engine.getProxy('TrackDecider::addGpuFreqTracks'));
    await this.addCpuFreqLimitCounterTracks(
          this.engine.getProxy('TrackDecider::addCpuFreqLimitCounterTracks'));
    await this.addCpuPerfCounterTracks(
        this.engine.getProxy('TrackDecider::addCpuPerfCounterTracks'));
    this.addPluginTracks();
    await this.addAnnotationTracks(
        this.engine.getProxy('TrackDecider::addAnnotationTracks'));
    await this.groupGlobalIonTracks();
    await this.groupGlobalIostatTracks(F2FS_IOSTAT_TAG, F2FS_IOSTAT_GROUP_NAME);
    await this.groupGlobalIostatTracks(
        F2FS_IOSTAT_LAT_TAG, F2FS_IOSTAT_LAT_GROUP_NAME);
    await this.groupGlobalIostatTracks(DISK_IOSTAT_TAG, DISK_IOSTAT_GROUP_NAME);
    await this.groupGlobalUfsCmdTagTracks(UFS_CMD_TAG, UFS_CMD_TAG_GROUP_NAME);
    await this.groupGlobalBuddyInfoTracks();
    await this.groupTracksByRegex(KERNEL_WAKELOCK_REGEX, KERNEL_WAKELOCK_GROUP);
    await this.groupTracksByRegex(NETWORK_TRACK_REGEX, NETWORK_TRACK_GROUP);
    await this.groupTracksByRegex(
        ENTITY_RESIDENCY_REGEX, ENTITY_RESIDENCY_GROUP);
    await this.groupTracksByRegex(UCLAMP_REGEX, UCLAMP_GROUP);
    await this.groupFrequencyTracks(FREQUENCY_GROUP);
    await this.groupTracksByRegex(POWER_RAILS_REGEX, POWER_RAILS_GROUP);
    await this.groupTracksByRegex(TEMPERATURE_REGEX, TEMPERATURE_GROUP);
    await this.groupTracksByRegex(IRQ_REGEX, IRQ_GROUP);
    await this.groupTracksByRegex(CHROME_TRACK_REGEX, CHROME_TRACK_GROUP);
    await this.groupMiscNonAllowlistedTracks(MISC_GROUP);

    // Pre-group all kernel "threads" (actually processes) if this is a linux
    // system trace. Below, addProcessTrackGroups will skip them due to an
    // existing group uuid, and addThreadStateTracks will fill in the
    // per-thread tracks. Quirk: since all threads will appear to be
    // TrackKindPriority.MAIN_THREAD, any process-level tracks will end up
    // pushed to the bottom of the group in the UI.
    await this.addKernelThreadGrouping(
        this.engine.getProxy('TrackDecider::addKernelThreadGrouping'));

    // Create the per-process track groups. Note that this won't necessarily
    // create a track per process. If a process has been completely idle and has
    // no sched events, no track group will be emitted.
    // Will populate this.addTrackGroupActions
    await this.addProcessTrackGroups(
        this.engine.getProxy('TrackDecider::addProcessTrackGroups'));

    await this.addProcessHeapProfileTracks(
        this.engine.getProxy('TrackDecider::addProcessHeapProfileTracks'));
    if (PERF_SAMPLE_FLAG.get()) {
      await this.addProcessPerfSamplesTracks(
          this.engine.getProxy('TrackDecider::addProcessPerfSamplesTracks'));
    }
    await this.addProcessCounterTracks(
        this.engine.getProxy('TrackDecider::addProcessCounterTracks'));
    await this.addProcessAsyncSliceTracks(
        this.engine.getProxy('TrackDecider::addProcessAsyncSliceTrack'));
    await this.addActualFramesTracks(
        this.engine.getProxy('TrackDecider::addActualFramesTracks'));
    await this.addExpectedFramesTracks(
        this.engine.getProxy('TrackDecider::addExpectedFramesTracks'));
    await this.addThreadCounterTracks(
        this.engine.getProxy('TrackDecider::addThreadCounterTracks'));
    await this.addThreadStateTracks(
        this.engine.getProxy('TrackDecider::addThreadStateTracks'));
    await this.addThreadSliceTracks(
        this.engine.getProxy('TrackDecider::addThreadSliceTracks'));
    await this.addThreadCpuSampleTracks(
        this.engine.getProxy('TrackDecider::addThreadCpuSampleTracks'));
    await this.addLogsTrack(this.engine.getProxy('TrackDecider::addLogsTrack'));

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
        Actions.addTracks({tracks: this.tracksToAdd}));

    const threadOrderingMetadata = await this.computeThreadOrderingMetadata();
    this.addTrackGroupActions.push(
        Actions.setUtidToTrackSortKey({threadOrderingMetadata}));

    return this.addTrackGroupActions;
  }

  // Some process counter tracks are tied to specific threads based on their
  // name.
  private async resolveTrackSortKeyForProcessCounterTrack(
      upid: number, threadName?: string): Promise<TrackSortKey> {
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
    for (; it; it.next()) {
      return {
        utid: it.utid,
        priority: InThreadTrackSortKey.THREAD_COUNTER_TRACK,
      };
    }
    return PrimaryTrackSortKey.COUNTER_TRACK;
  }

  private static getThreadSortKey(
      threadName?: string|null, tid?: number|null,
      pid?: number|null): PrimaryTrackSortKey {
    if (pid !== undefined && pid !== null && pid === tid) {
      return PrimaryTrackSortKey.MAIN_THREAD;
    }
    if (threadName === undefined || threadName === null) {
      return PrimaryTrackSortKey.ORDINARY_THREAD;
    }

    // Chrome main threads should always come first within their process.
    if (threadName === 'CrBrowserMain' || threadName === 'CrRendererMain' ||
        threadName === 'CrGpuMain') {
      return PrimaryTrackSortKey.MAIN_THREAD;
    }

    // Chrome IO threads should always come immediately after the main thread.
    if (threadName === 'Chrome_ChildIOThread' ||
        threadName === 'Chrome_IOThread') {
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

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
import {Engine} from '../common/engine';
import {featureFlags, PERF_SAMPLE_FLAG} from '../common/feature_flags';
import {
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../common/query_result';
import {
  InThreadTrackSortKey,
  PrimaryTrackSortKey,
  SCROLLING_TRACK_GROUP,
  TrackSortKey,
  UtidToTrackSortKey,
} from '../common/state';
import {ACTUAL_FRAMES_SLICE_TRACK_KIND} from '../tracks/actual_frames';
import {ANDROID_LOGS_TRACK_KIND} from '../tracks/android_log';
import {ASYNC_SLICE_TRACK_KIND} from '../tracks/async_slices';
import {SLICE_TRACK_KIND} from '../tracks/chrome_slices';
import {COUNTER_TRACK_KIND} from '../tracks/counter';
import {CPU_FREQ_TRACK_KIND} from '../tracks/cpu_freq';
import {CPU_PROFILE_TRACK_KIND} from '../tracks/cpu_profile';
import {CPU_SLICE_TRACK_KIND} from '../tracks/cpu_slices';
import {
  EXPECTED_FRAMES_SLICE_TRACK_KIND,
} from '../tracks/expected_frames';
import {HEAP_PROFILE_TRACK_KIND} from '../tracks/heap_profile';
import {NULL_TRACK_KIND} from '../tracks/null_track';
import {
  PERF_SAMPLES_PROFILE_TRACK_KIND,
} from '../tracks/perf_samples_profile';
import {
  PROCESS_SCHEDULING_TRACK_KIND,
} from '../tracks/process_scheduling';
import {PROCESS_SUMMARY_TRACK} from '../tracks/process_summary';
import {THREAD_STATE_TRACK_KIND} from '../tracks/thread_state';

const NULL_TRACKS_FLAG = featureFlags.register({
  id: 'nullTracks',
  name: 'Null tracks',
  description: 'Display some empty tracks.',
  defaultValue: false,
});

const TRACKS_V2_FLAG = featureFlags.register({
  id: 'tracksV2',
  name: 'Tracks V2',
  description: 'Show tracks built on top of the Track V2 API.',
  defaultValue: false,
});

const MEM_DMA_COUNTER_NAME = 'mem.dma_heap';
const MEM_DMA = 'mem.dma_buffer';
const MEM_ION = 'mem.ion';
const F2FS_IOSTAT_TAG = 'f2fs_iostat.';
const F2FS_IOSTAT_GROUP_NAME = 'f2fs_iostat';
const F2FS_IOSTAT_LAT_TAG = 'f2fs_iostat_latency.';
const F2FS_IOSTAT_LAT_GROUP_NAME = 'f2fs_iostat_latency';
const UFS_CMD_TAG = 'io.ufs.command.tag';
const UFS_CMD_TAG_GROUP_NAME = 'io.ufs.command.tags';

export async function decideTracks(
    engineId: string, engine: Engine): Promise<DeferredAction[]> {
  return (new TrackDecider(engineId, engine)).decideTracks();
}

class TrackDecider {
  private engineId: string;
  private engine: Engine;
  private upidToUuid = new Map<number, string>();
  private utidToUuid = new Map<number, string>();
  private tracksToAdd: AddTrackArgs[] = [];
  private addTrackGroupActions: DeferredAction[] = [];

  constructor(engineId: string, engine: Engine) {
    this.engineId = engineId;
    this.engine = engine;
  }

  static getTrackName(args: Partial<{
    name: string | null,
    utid: number,
    processName: string|null,
    pid: number|null,
    threadName: string|null,
    tid: number|null,
    upid: number|null,
    kind: string,
    threadTrack: boolean
  }>) {
    const {
      name,
      upid,
      utid,
      processName,
      threadName,
      pid,
      tid,
      kind,
      threadTrack,
    } = args;

    const hasName = name !== undefined && name !== null && name !== '[NULL]';
    const hasUpid = upid !== undefined && upid !== null;
    const hasUtid = utid !== undefined && utid !== null;
    const hasProcessName = processName !== undefined && processName !== null;
    const hasThreadName = threadName !== undefined && threadName !== null;
    const hasTid = tid !== undefined && tid !== null;
    const hasPid = pid !== undefined && pid !== null;
    const hasKind = kind !== undefined;
    const isThreadTrack = threadTrack !== undefined && threadTrack;

    // If we don't have any useful information (better than
    // upid/utid) we show the track kind to help with tracking
    // down where this is coming from.
    const kindSuffix = hasKind ? ` (${kind})` : '';

    if (isThreadTrack && hasName && hasTid) {
      return `${name} (${tid})`;
    } else if (hasName) {
      return `${name}`;
    } else if (hasUpid && hasPid && hasProcessName) {
      return `${processName} ${pid}`;
    } else if (hasUpid && hasPid) {
      return `Process ${pid}`;
    } else if (hasThreadName && hasTid) {
      return `${threadName} ${tid}`;
    } else if (hasTid) {
      return `Thread ${tid}`;
    } else if (hasUpid) {
      return `upid: ${upid}${kindSuffix}`;
    } else if (hasUtid) {
      return `utid: ${utid}${kindSuffix}`;
    } else if (hasKind) {
      return `Unnamed ${kind}`;
    }
    return 'Unknown';
  }

  addNullTracks(): void {
    this.tracksToAdd.push({
      engineId: this.engineId,
      kind: NULL_TRACK_KIND,
      trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
      name: `Null track foo`,
      trackGroup: SCROLLING_TRACK_GROUP,
      config: {},
    });

    this.tracksToAdd.push({
      engineId: this.engineId,
      kind: NULL_TRACK_KIND,
      trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
      name: `Null track bar`,
      trackGroup: SCROLLING_TRACK_GROUP,
      config: {},
    });
  }

  async addCpuSchedulingTracks(): Promise<void> {
    const cpus = await this.engine.getCpus();
    for (const cpu of cpus) {
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: CPU_SLICE_TRACK_KIND,
        trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
        name: `Cpu ${cpu}`,
        trackGroup: SCROLLING_TRACK_GROUP,
        config: {
          cpu,
        },
      });
    }
  }

  async addCpuFreqTracks(): Promise<void> {
    const cpus = await this.engine.getCpus();

    const maxCpuFreqResult = await this.engine.query(`
    select ifnull(max(value), 0) as freq
    from counter c
    inner join cpu_counter_track t on c.track_id = t.id
    where name = 'cpufreq';
  `);
    const maxCpuFreq = maxCpuFreqResult.firstRow({freq: NUM}).freq;

    for (const cpu of cpus) {
      // Only add a cpu freq track if we have
      // cpu freq data.
      // TODO(hjd): Find a way to display cpu idle
      // events even if there are no cpu freq events.
      const cpuFreqIdleResult = await this.engine.query(`
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
        const row = cpuFreqIdleResult.firstRow({
          cpuFreqId: NUM,
          cpuIdleId: NUM_NULL,
        });
        const freqTrackId = row.cpuFreqId;
        const idleTrackId = row.cpuIdleId === null ? undefined : row.cpuIdleId;

        this.tracksToAdd.push({
          engineId: this.engineId,
          kind: CPU_FREQ_TRACK_KIND,
          trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
          name: `Cpu ${cpu} Frequency`,
          trackGroup: SCROLLING_TRACK_GROUP,
          config: {
            cpu,
            maximumValue: maxCpuFreq,
            freqTrackId,
            idleTrackId,
          },
        });
      }
    }
  }

  async addGlobalAsyncTracks(): Promise<void> {
    const rawGlobalAsyncTracks = await this.engine.query(`
    SELECT
      t.name as name,
      t.track_ids as trackIds,
      MAX(experimental_slice_layout.layout_depth) as maxDepth
    FROM (
      SELECT name, GROUP_CONCAT(track.id) AS track_ids
      FROM track
      WHERE track.type = "track" or track.type = "gpu_track"
      GROUP BY name
    ) AS t CROSS JOIN experimental_slice_layout
    WHERE t.track_ids = experimental_slice_layout.filter_track_ids
    GROUP BY t.track_ids
    ORDER BY t.name;
  `);
    const it = rawGlobalAsyncTracks.iter({
      name: STR_NULL,
      trackIds: STR,
      maxDepth: NUM,
    });

    for (; it.valid(); it.next()) {
      const name = it.name === null ? undefined : it.name;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;
      const kind = ASYNC_SLICE_TRACK_KIND;
      const track = {
        engineId: this.engineId,
        kind,
        trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
        trackGroup: SCROLLING_TRACK_GROUP,
        name: TrackDecider.getTrackName({name, kind}),
        config: {
          maxDepth,
          trackIds,
        },
      };
      this.tracksToAdd.push(track);
    }
  }

  async addGpuFreqTracks(): Promise<void> {
    const numGpus = await this.engine.getNumberOfGpus();
    const maxGpuFreqResult = await this.engine.query(`
    select ifnull(max(value), 0) as maximumValue
    from counter c
    inner join gpu_counter_track t on c.track_id = t.id
    where name = 'gpufreq';
  `);
    const maximumValue =
        maxGpuFreqResult.firstRow({maximumValue: NUM}).maximumValue;

    for (let gpu = 0; gpu < numGpus; gpu++) {
      // Only add a gpu freq track if we have
      // gpu freq data.
      const freqExistsResult = await this.engine.query(`
      select id
      from gpu_counter_track
      where name = 'gpufreq' and gpu_id = ${gpu}
      limit 1;
    `);
      if (freqExistsResult.numRows() > 0) {
        const trackId = freqExistsResult.firstRow({id: NUM}).id;
        this.tracksToAdd.push({
          engineId: this.engineId,
          kind: COUNTER_TRACK_KIND,
          name: `Gpu ${gpu} Frequency`,
          trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
          trackGroup: SCROLLING_TRACK_GROUP,
          config: {
            trackId,
            maximumValue,
          },
        });
      }
    }
  }

  async addGlobalCounterTracks(): Promise<void> {
    // Add global or GPU counter tracks that are not bound to any pid/tid.
    const globalCounters = await this.engine.query(`
    select name, id
    from (
      select name, id
      from counter_track
      where type = 'counter_track'
      union
      select name, id
      from gpu_counter_track
      where name != 'gpufreq'
    )
    order by name
  `);

    const it = globalCounters.iter({
      name: STR,
      id: NUM,
    });

    for (; it.valid(); it.next()) {
      const name = it.name;
      const trackId = it.id;
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: COUNTER_TRACK_KIND,
        name,
        trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
        trackGroup: SCROLLING_TRACK_GROUP,
        config: {
          name,
          trackId,
        },
      });
    }
  }

  async addCpuPerfCounterTracks(): Promise<void> {
    // Perf counter tracks are bound to CPUs, follow the scheduling and
    // frequency track naming convention ("Cpu N ...").
    // Note: we might not have a track for a given cpu if no data was seen from
    // it. This might look surprising in the UI, but placeholder tracks are
    // wasteful as there's no way of collapsing global counter tracks at the
    // moment.
    const result = await this.engine.query(`
      select printf("Cpu %u %s", cpu, name) as name, id
      from perf_counter_track as pct
      order by perf_session_id asc, pct.name asc, cpu asc
  `);

    const it = result.iter({
      name: STR,
      id: NUM,
    });

    for (; it.valid(); it.next()) {
      const name = it.name;
      const trackId = it.id;
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: COUNTER_TRACK_KIND,
        name,
        trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
        trackGroup: SCROLLING_TRACK_GROUP,
        config: {
          name,
          trackId,
        },
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
    const summaryTrackId = uuidv4();
    let foundSummary = false;

    for (const track of ionTracks) {
      if (!foundSummary &&
          [MEM_DMA_COUNTER_NAME, MEM_ION].includes(track.name)) {
        foundSummary = true;
        track.id = summaryTrackId;
        track.trackGroup = undefined;
      } else {
        track.trackGroup = id;
      }
    }

    const addGroup = Actions.addTrackGroup({
      engineId: this.engineId,
      summaryTrackId,
      name: MEM_DMA_COUNTER_NAME,
      id,
      collapsed: true,
    });
    this.addTrackGroupActions.push(addGroup);
  }

  async groupGlobalF2fsIostatTracks(tag: string, group: string): Promise<void> {
    const f2fsIostatTracks: AddTrackArgs[] = [];
    const devMap = new Map<string, string>();

    for (const track of this.tracksToAdd) {
      if (track.name.startsWith(tag)) {
        f2fsIostatTracks.push(track);
      }
    }

    if (f2fsIostatTracks.length === 0) {
      return;
    }

    for (const track of f2fsIostatTracks) {
      const name = track.name.split('.', 3);

      if (!devMap.has(name[1])) {
        devMap.set(name[1], uuidv4());
      }
      track.name = name[2];
      track.trackGroup = devMap.get(name[1]);
    }

    for (const [key, value] of devMap) {
      const groupName = group + key;
      const summaryTrackId = uuidv4();

      this.tracksToAdd.push({
        id: summaryTrackId,
        engineId: this.engineId,
        kind: NULL_TRACK_KIND,
        trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
        name: groupName,
        trackGroup: undefined,
        config: {},
      });

      const addGroup = Actions.addTrackGroup({
        engineId: this.engineId,
        summaryTrackId,
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
    const summaryTrackId = uuidv4();
    ufsCmdTagTracks[0].id = summaryTrackId;
    for (const track of ufsCmdTagTracks) {
      track.trackGroup = id;
    }

    const addGroup = Actions.addTrackGroup({
      engineId: this.engineId,
      summaryTrackId,
      name: group,
      id,
      collapsed: true,
    });
    this.addTrackGroupActions.push(addGroup);
  }

  async addLogsTrack(): Promise<void> {
    const result =
        await this.engine.query(`select count(1) as cnt from android_logs`);
    const count = result.firstRow({cnt: NUM}).cnt;

    if (count > 0) {
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: ANDROID_LOGS_TRACK_KIND,
        name: 'Android logs',
        trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
        trackGroup: SCROLLING_TRACK_GROUP,
        config: {},
      });
    }
  }

  async addAnnotationTracks(): Promise<void> {
    const sliceResult = await this.engine.query(`
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
      summaryTrackId: string;
    }

    const groupNameToIds = new Map<string, GroupIds>();

    for (; sliceIt.valid(); sliceIt.next()) {
      const id = sliceIt.id;
      const name = sliceIt.name;
      const upid = sliceIt.upid;
      const groupName = sliceIt.group_name;

      let summaryTrackId = undefined;
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
          summaryTrackId = uuidv4();
          groupNameToIds.set(groupName, {
            id: trackGroupId,
            summaryTrackId,
          });
        }
      }

      this.tracksToAdd.push({
        id: summaryTrackId,
        engineId: this.engineId,
        kind: SLICE_TRACK_KIND,
        name,
        trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
        trackGroup: trackGroupId,
        config: {
          maxDepth: 0,
          namespace: 'annotation',
          trackId: id,
        },
      });
    }

    for (const [groupName, groupIds] of groupNameToIds) {
      const addGroup = Actions.addTrackGroup({
        engineId: this.engineId,
        summaryTrackId: groupIds.summaryTrackId,
        name: groupName,
        id: groupIds.id,
        collapsed: true,
      });
      this.addTrackGroupActions.push(addGroup);
    }

    const counterResult = await this.engine.query(`
    SELECT
      id,
      name,
      upid,
      min_value as minValue,
      max_value as maxValue
    FROM annotation_counter_track`);

    const counterIt = counterResult.iter({
      id: NUM,
      name: STR,
      upid: NUM,
      minValue: NUM_NULL,
      maxValue: NUM_NULL,
    });

    for (; counterIt.valid(); counterIt.next()) {
      const id = counterIt.id;
      const name = counterIt.name;
      const upid = counterIt.upid;
      const minimumValue =
          counterIt.minValue === null ? undefined : counterIt.minValue;
      const maximumValue =
          counterIt.maxValue === null ? undefined : counterIt.maxValue;
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: 'CounterTrack',
        name,
        trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
        trackGroup: upid === 0 ? SCROLLING_TRACK_GROUP :
                                 this.upidToUuid.get(upid),
        config: {
          name,
          namespace: 'annotation',
          trackId: id,
          minimumValue,
          maximumValue,
        },
      });
    }
  }

  async addThreadStateTracks(): Promise<void> {
    const result = await this.engine.query(`
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
      const kind = THREAD_STATE_TRACK_KIND;
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name: TrackDecider.getTrackName({utid, tid, threadName, kind}),
        trackGroup: uuid,
        trackSortKey: {
          utid,
          priority: InThreadTrackSortKey.THREAD_SCHEDULING_STATE_TRACK,
        },
        config: {utid, tid},
      });
    }
  }

  async addThreadCpuSampleTracks(): Promise<void> {
    const result = await this.engine.query(`
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
        engineId: this.engineId,
        kind: CPU_PROFILE_TRACK_KIND,
        trackSortKey: {
          utid,
          priority: InThreadTrackSortKey.CPU_STACK_SAMPLES_TRACK,
        },
        name: `${threadName} (CPU Stack Samples)`,
        trackGroup: uuid,
        config: {utid},
      });
    }
  }

  async addThreadCounterTracks(): Promise<void> {
    const result = await this.engine.query(`
    select
      thread_counter_track.name as trackName,
      utid,
      upid,
      tid,
      thread.name as threadName,
      thread_counter_track.id as trackId,
      thread.start_ts as startTs,
      thread.end_ts as endTs
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
      startTs: NUM_NULL,
      trackId: NUM,
      endTs: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const upid = it.upid;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const threadName = it.threadName;
      const uuid = this.getUuid(utid, upid);
      const startTs = it.startTs === null ? undefined : it.startTs;
      const endTs = it.endTs === null ? undefined : it.endTs;
      const kind = COUNTER_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, utid, tid, kind, threadName, threadTrack: true});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackSortKey: {
          utid,
          priority: InThreadTrackSortKey.ORDINARY,
        },
        trackGroup: uuid,
        config: {name, trackId, startTs, endTs, tid},
      });
    }
  }

  async addProcessAsyncSliceTracks(): Promise<void> {
    const result = await this.engine.query(`
        select
          process_track.upid as upid,
          process_track.name as trackName,
          group_concat(process_track.id) as trackIds,
          process.name as processName,
          process.pid as pid
        from process_track
        left join process using(upid)
        where
            process_track.name is null or
            process_track.name not like "% Timeline"
        group by
          process_track.upid,
          process_track.name
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
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;

      const uuid = this.getUuid(0, upid);

      // TODO(hjd): 1+N queries are bad in the track_decider
      const depthResult = await this.engine.query(`
      SELECT IFNULL(MAX(layout_depth), 0) as depth
      FROM experimental_slice_layout('${rawTrackIds}');
    `);
      const maxDepth = depthResult.firstRow({depth: NUM}).depth;

      const kind = ASYNC_SLICE_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, upid, pid, processName, kind});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
        trackGroup: uuid,
        config: {
          trackIds,
          maxDepth,
        },
      });
    }
  }

  async addActualFramesTracks(): Promise<void> {
    const result = await this.engine.query(`
        select
          upid,
          trackName,
          trackIds,
          process.name as processName,
          process.pid as pid
        from (
          select
            process_track.upid as upid,
            process_track.name as trackName,
            group_concat(process_track.id) as trackIds
          from process_track
          where process_track.name like "Actual Timeline"
          group by
            process_track.upid,
            process_track.name
        ) left join process using(upid)
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
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;

      const uuid = this.getUuid(0, upid);

      // TODO(hjd): 1+N queries are bad in the track_decider
      const depthResult = await this.engine.query(`
      SELECT IFNULL(MAX(layout_depth), 0) as depth
      FROM experimental_slice_layout('${rawTrackIds}');
    `);
      const maxDepth = depthResult.firstRow({depth: NUM}).depth;

      const kind = ACTUAL_FRAMES_SLICE_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, upid, pid, processName, kind});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackSortKey: PrimaryTrackSortKey.ACTUAL_FRAMES_SLICE_TRACK,
        trackGroup: uuid,
        config: {
          trackIds,
          maxDepth,
        },
      });
    }
  }

  async addExpectedFramesTracks(): Promise<void> {
    const result = await this.engine.query(`
        select
          upid,
          trackName,
          trackIds,
          process.name as processName,
          process.pid as pid
        from (
          select
            process_track.upid as upid,
            process_track.name as trackName,
            group_concat(process_track.id) as trackIds
          from process_track
          where process_track.name like "Expected Timeline"
          group by
            process_track.upid,
            process_track.name
        ) left join process using(upid)
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
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;

      const uuid = this.getUuid(0, upid);

      // TODO(hjd): 1+N queries are bad in the track_decider
      const depthResult = await this.engine.query(`
      SELECT IFNULL(MAX(layout_depth), 0) as depth
      FROM experimental_slice_layout('${rawTrackIds}');
    `);
      const maxDepth = depthResult.firstRow({depth: NUM}).depth;

      const kind = EXPECTED_FRAMES_SLICE_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, upid, pid, processName, kind});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackSortKey: PrimaryTrackSortKey.EXPECTED_FRAMES_SLICE_TRACK,
        trackGroup: uuid,
        config: {
          trackIds,
          maxDepth,
        },
      });
    }
  }

  async addThreadSliceTracks(): Promise<void> {
    const result = await this.engine.query(`
        select
          thread_track.utid as utid,
          thread_track.id as trackId,
          thread_track.name as trackName,
          EXTRACT_ARG(thread_track.source_arg_set_id,
                      'is_root_in_scope') as isDefaultTrackForScope,
          tid,
          thread.name as threadName,
          max(slice.depth) as maxDepth,
          (count(thread_slice.id) = count(slice.id)) as onlyThreadSlice,
          process.upid as upid
        from slice
        join thread_track on slice.track_id = thread_track.id
        join thread using(utid)
        left join process using(upid)
        left join thread_slice on slice.id = thread_slice.id
        group by thread_track.id
  `);

    const it = result.iter({
      utid: NUM,
      trackId: NUM,
      trackName: STR_NULL,
      isDefaultTrackForScope: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
      maxDepth: NUM,
      upid: NUM_NULL,
      onlyThreadSlice: NUM,
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
      const maxDepth = it.maxDepth;
      const onlyThreadSlice = it.onlyThreadSlice;

      const uuid = this.getUuid(utid, upid);

      const kind = SLICE_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, utid, tid, threadName, kind});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackGroup: uuid,
        trackSortKey: {
          utid,
          priority: isDefaultTrackForScope ?
              InThreadTrackSortKey.DEFAULT_TRACK :
              InThreadTrackSortKey.ORDINARY,
        },
        config: {
          trackId,
          maxDepth,
          tid,
          isThreadSlice: onlyThreadSlice === 1,
        },
      });

      if (TRACKS_V2_FLAG.get()) {
        this.tracksToAdd.push({
          engineId: this.engineId,
          kind: 'GenericSliceTrack',
          name,
          trackGroup: uuid,
          trackSortKey: {
            utid,
            priority: isDefaultTrackForScope ?
                InThreadTrackSortKey.DEFAULT_TRACK :
                InThreadTrackSortKey.ORDINARY,
          },
          config: {sqlTrackId: trackId},
        });
      }
    }
  }

  async addProcessCounterTracks(): Promise<void> {
    const result = await this.engine.query(`
    select
      process_counter_track.id as trackId,
      process_counter_track.name as trackName,
      upid,
      process.pid,
      process.name as processName,
      process.start_ts as startTs,
      process.end_ts as endTs
    from process_counter_track
    join process using(upid);
  `);
    const it = result.iter({
      trackId: NUM,
      trackName: STR_NULL,
      upid: NUM,
      pid: NUM_NULL,
      processName: STR_NULL,
      startTs: NUM_NULL,
      endTs: NUM_NULL,
    });
    for (let i = 0; it.valid(); ++i, it.next()) {
      const pid = it.pid;
      const upid = it.upid;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const processName = it.processName;
      const uuid = this.getUuid(0, upid);
      const startTs = it.startTs === null ? undefined : it.startTs;
      const endTs = it.endTs === null ? undefined : it.endTs;
      const kind = COUNTER_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, upid, pid, kind, processName});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackSortKey: await this.resolveTrackSortKeyForProcessCounterTrack(
            upid, trackName || undefined),
        trackGroup: uuid,
        config: {
          name,
          trackId,
          startTs,
          endTs,
        },
      });
    }
  }

  async addProcessHeapProfileTracks(): Promise<void> {
    const result = await this.engine.query(`
    select distinct(upid) from heap_profile_allocation
    union
    select distinct(upid) from heap_graph_object
  `);
    for (const it = result.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const uuid = this.getUuid(0, upid);
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: HEAP_PROFILE_TRACK_KIND,
        trackSortKey: PrimaryTrackSortKey.HEAP_PROFILE_TRACK,
        name: `Heap Profile`,
        trackGroup: uuid,
        config: {upid},
      });
    }
  }

  async addProcessPerfSamplesTracks(): Promise<void> {
    const result = await this.engine.query(`
      select distinct upid, pid
      from perf_sample join thread using (utid) join process using (upid)
      where callsite_id is not null
  `);
    for (const it = result.iter({upid: NUM, pid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const pid = it.pid;
      const uuid = this.getUuid(0, upid);
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: PERF_SAMPLES_PROFILE_TRACK_KIND,
        trackSortKey: PrimaryTrackSortKey.PERF_SAMPLES_PROFILE_TRACK,
        name: `Callstacks ${pid}`,
        trackGroup: uuid,
        config: {upid},
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

  async addKernelThreadGrouping(): Promise<void> {
    // Identify kernel threads if this is a linux system trace, and sufficient
    // process information is available. Kernel threads are identified by being
    // children of kthreadd (always pid 2).
    // The query will return the kthreadd process row first, which must exist
    // for any other kthreads to be returned by the query.
    // TODO(rsavitski): figure out how to handle the idle process (swapper),
    // which has pid 0 but appears as a distinct process (with its own comm) on
    // each cpu. It'd make sense to exclude its thread state track, but still
    // put process-scoped tracks in this group.
    const result = await this.engine.query(`
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
    const summaryTrackId = uuidv4();
    this.tracksToAdd.push({
      id: summaryTrackId,
      engineId: this.engineId,
      kind: PROCESS_SUMMARY_TRACK,
      trackSortKey: PrimaryTrackSortKey.MAIN_THREAD,
      name: `Kernel thread summary`,
      config: {pidForColor: 2, upid: it.upid, utid: it.utid},
    });
    const addTrackGroup = Actions.addTrackGroup({
      engineId: this.engineId,
      summaryTrackId,
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

  async addProcessTrackGroups(): Promise<void> {
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
    const result = await this.engine.query(`
    select
      the_tracks.upid,
      the_tracks.utid,
      total_dur as hasSched,
      hasHeapProfiles,
      process.pid as pid,
      thread.tid as tid,
      process.name as processName,
      thread.name as threadName,
      process.arg_set_id as argSetId,
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
      argSetId: NUM_NULL,
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

      const labels = [];
      if (it.argSetId !== null) {
        const result = await this.engine.query(`
          select string_value as label
          from args
          where arg_set_id = ${it.argSetId}
        `);
        const argIt = result.iter({label: STR_NULL});
        for (; argIt.valid(); argIt.next()) {
          if (argIt.label !== null) {
            labels.push(argIt.label);
          }
        }
      }

      // Group by upid if present else by utid.
      let pUuid =
          upid === null ? this.utidToUuid.get(utid) : this.upidToUuid.get(upid);
      // These should only happen once for each track group.
      if (pUuid === undefined) {
        pUuid = this.getOrCreateUuid(utid, upid);
        const summaryTrackId = uuidv4();

        const pidForColor = pid || tid || upid || utid || 0;
        const kind =
            hasSched ? PROCESS_SCHEDULING_TRACK_KIND : PROCESS_SUMMARY_TRACK;

        this.tracksToAdd.push({
          id: summaryTrackId,
          engineId: this.engineId,
          kind,
          trackSortKey: hasSched ?
              PrimaryTrackSortKey.PROCESS_SCHEDULING_TRACK :
              PrimaryTrackSortKey.PROCESS_SUMMARY_TRACK,
          name: `${upid === null ? tid : pid} summary`,
          config: {pidForColor, upid, utid, tid},
          labels,
        });

        const name = TrackDecider.getTrackName(
            {utid, processName, pid, threadName, tid, upid});
        const addTrackGroup = Actions.addTrackGroup({
          engineId: this.engineId,
          summaryTrackId,
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

  async decideTracks(): Promise<DeferredAction[]> {
    // Add first the global tracks that don't require per-process track groups.
    if (NULL_TRACKS_FLAG.get()) {
      await this.addNullTracks();
    }
    await this.addCpuSchedulingTracks();
    await this.addCpuFreqTracks();
    await this.addGlobalAsyncTracks();
    await this.addGpuFreqTracks();
    await this.addGlobalCounterTracks();
    await this.addCpuPerfCounterTracks();
    await this.addAnnotationTracks();
    await this.groupGlobalIonTracks();
    await this.groupGlobalF2fsIostatTracks(
        F2FS_IOSTAT_TAG, F2FS_IOSTAT_GROUP_NAME);
    await this.groupGlobalF2fsIostatTracks(
        F2FS_IOSTAT_LAT_TAG, F2FS_IOSTAT_LAT_GROUP_NAME);
    await this.groupGlobalUfsCmdTagTracks(
        UFS_CMD_TAG, UFS_CMD_TAG_GROUP_NAME);

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
    await this.addProcessTrackGroups();

    await this.addProcessHeapProfileTracks();
    if (PERF_SAMPLE_FLAG.get()) {
      await this.addProcessPerfSamplesTracks();
    }
    await this.addProcessCounterTracks();
    await this.addProcessAsyncSliceTracks();
    await this.addActualFramesTracks();
    await this.addExpectedFramesTracks();
    await this.addThreadCounterTracks();
    await this.addThreadStateTracks();
    await this.addThreadSliceTracks();
    await this.addThreadCpuSampleTracks();
    await this.addLogsTrack();

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

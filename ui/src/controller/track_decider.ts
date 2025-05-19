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
  AddTrackGroupArgs,
  AddTrackLikeArgs,
  DeferredAction,
} from '../common/actions';
import {Engine, EngineProxy} from '../common/engine';
import {featureFlags, PERF_SAMPLE_FLAG} from '../common/feature_flags';
import {pluginManager} from '../common/plugins';
import {
  LONG_NULL,
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
import {
  decideTracks as scrollJankDecideTracks,
} from '../tracks/chrome_scroll_jank';
import {SLICE_TRACK_KIND} from '../tracks/chrome_slices';
import {COUNTER_TRACK_KIND, CounterScaleOptions} from '../tracks/counter';
import {CPU_FREQ_TRACK_KIND} from '../tracks/cpu_freq';
import {CPU_PROFILE_TRACK_KIND} from '../tracks/cpu_profile';
import {CPU_SLICE_TRACK_KIND} from '../tracks/cpu_slices';
import {
  EXPECTED_FRAMES_SLICE_TRACK_KIND,
} from '../tracks/expected_frames';
import {FTRACE_RAW_TRACK_KIND} from '../tracks/ftrace';
import {HEAP_PROFILE_TRACK_KIND} from '../tracks/heap_profile';
import {NULL_TRACK_KIND} from '../tracks/null_track';
import {
  PERF_SAMPLES_PROFILE_TRACK_KIND,
} from '../tracks/perf_samples_profile';
import {
  PROCESS_SCHEDULING_TRACK_KIND,
} from '../tracks/process_scheduling';
import {PROCESS_SUMMARY_TRACK} from '../tracks/process_summary';
import {
  ENABLE_SCROLL_JANK_PLUGIN_V2,
  INPUT_LATENCY_TRACK,
} from '../tracks/scroll_jank';
import {addLatenciesTrack} from '../tracks/scroll_jank/event_latency_track';
import {addTopLevelScrollTrack} from '../tracks/scroll_jank/scroll_track';
import {THREAD_STATE_TRACK_KIND} from '../tracks/thread_state';
import {shouldCreateTrack, shouldCreateTrackGroup} from './track_filter';
import {TrackInfo} from '../common/plugin_api';
import {globals} from '../frontend/globals';
import {METRIC_NAMES} from './trace_controller';

const TRACKS_V2_FLAG = featureFlags.register({
  id: 'tracksV2.1',
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
// Sets the default 'scale' for counter tracks. If the regex matches
// then the paired mode is used. Entries are in priority order so the
// first match wins.
const COUNTER_REGEX: [RegExp, CounterScaleOptions][] = [
  // Power counters make more sense in rate mode since you're typically
  // interested in the slope of the graph rather than the absolute
  // value.
  [new RegExp('^power\..*$'), 'RATE'],
  // Same for network counters.
  [NETWORK_TRACK_REGEX, 'RATE'],
  // Entity residency
  [ENTITY_RESIDENCY_REGEX, 'RATE'],
];

function getCounterScale(name: string): CounterScaleOptions|undefined {
  for (const [re, scale] of COUNTER_REGEX) {
    if (name.match(re)) {
      return scale;
    }
  }
  return undefined;
}

export async function decideTracks(
    engineId: string, engine: Engine,
    filterTracks = false): Promise<DeferredAction[]> {
  return (new TrackDecider(engineId, engine)).decideTracks(filterTracks);
}

type LazyTrackGroupArgs = Partial<AddTrackGroupArgs & {
  lazyParentGroup: () => string;
}>;

// Type of a function that produces new IDs on demand.
type LazyIdProvider = (() => string) & {
  // Whether the group exists, yet
  exists(): boolean;
  // Revoke an ID previously provided, undoing whatever
  // side-effects its provision entailed
  revoke(): void;
};

// A data structure for keeping track (no pun intended)
// of lazily-created track groups in their unique positions
// in the group hierarchy, accounting for duplication of
// names in different parent groups. When the group is created
// it is stored in its tree node.
type LazyTrackGroupTree = {
  name: string;
  id: LazyIdProvider;
  group?: AddTrackGroupArgs;
  parent?: LazyTrackGroupTree;
  children: LazyTrackGroupTree[];
}

// A process or thread is considered "idle" that uses less than 0.1%
// as much CPU as its parent context (trace or process, respectively).
const IDLE_FACTOR = 1000;

// Return either the |single| or |plural| form of a noun or verb
// according to the number |n| of objects.
function pluralize(n: number, single: string, plural = single + 's'): string {
  return n === 1 ?
    `${single}` :
    `${plural}`;
}

// Count a number |n| of nouns or verbs using either the
// |single| or |plural| form as appropriate. Unlike the
// |pluralize| function, the resulting string includes
// the |n| count.
function count(n: number, single: string, plural?: string): string {
  return `${n} ${pluralize(n, single, plural)}`;
}

// User-friendly titles for tracks.
const TRACK_TITLES: {[key: string]: string} = {
  'batt.capacity_pct': 'Capacity (%)',
  'batt.charge_uah': 'Charge (μAh)',
  'batt.current_ua': 'Current (μA)',
};

class TrackDecider {
  private engineId: string;
  private engine: Engine;
  private upidToUuid = new Map<number, string>();
  private utidToUuid = new Map<number, string>();
  private tracksToAdd: AddTrackArgs[] = [];
  private trackGroupsToAdd: AddTrackGroupArgs[] = [];

  // A tree of lazily created track groups. Each node
  // represents a group that may be created when needed
  // with a function providing its ID when created
  private lazyTrackGroups: LazyTrackGroupTree = {
    name: '',
    id: (() => SCROLLING_TRACK_GROUP) as LazyIdProvider,
    children: [],
  };

  private hasCpuUsage = true;

  // Set of |upid| from the |process| table recording
  // processes that are idle (< 0.1% CPU)
  private idleUpids = new Set<number>();

  // Map of |upid| process identifier from the |thread| table
  // to |utid| from the |thread| table recording threads
  // that are idle (< 0.1% of their process's CPU usage)
  private idleUtids = new Map<number, Set<number>>();

  // Map of |upid| process identifier to count of how many
  // of its threads were in existence but recorded no data
  // in the trace and so are not presented at all in the UI
  private nullThreadCounts = new Map<number, number>();

  // Map of |upid| process identifier to UUID of the thread
  // group (used for |AddTrackGroupArgs::id|), if any, that
  // collects its idle threads (< 0.1% CPU)
  private idleThreadGroups = new Map<number, string>();

  // Top-level "CPU" group for many things CPU-related.
  private cpuGroup: LazyIdProvider = this.lazyTrackGroup('CPU',
    {collapsed: false});

  // Top-level "GPU" group for all things GPU-related.
  private gpuGroup: LazyIdProvider = this.lazyTrackGroup('GPU',
    {collapsed: false});

  // Top-level "SurfaceFlinger Events" group.
  private sfEventsGroup: LazyIdProvider = this.lazyTrackGroup('SurfaceFlinger Events');

  // Top-level "Processes" group process groups containing
  // the process/thread tracks.
  private processesGroup: LazyIdProvider = this.lazyTrackGroup('Processes',
    {collapsed: false, description: 'Track groups for each active process.'});

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

  // Decorate an optional track description with additional contextual
  // information. The base |description| itself may originate in the
  // trace database or hard-coded in software.
  static decorateTrackDescription(description: string | undefined,
      args: Partial<{
        upid: number|null,
        processName: string|null,
        totalThreads: number|null,
        idleThreads: number|null,
        nullThreads: number|null,
      }>): string | undefined {
    const {upid, processName, totalThreads, idleThreads, nullThreads} = args;

    const hasProcessName = !!processName && upid !== undefined && upid !== null;
    const hasTotalThreads = totalThreads !== undefined && totalThreads !== null;
    const hasIdleThreads = idleThreads !== undefined && idleThreads !== null &&
      idleThreads > 0;
    const hasNullThreads = nullThreads !== undefined && nullThreads !== null &&
      nullThreads > 0;

    let suffix = '';
    if (hasProcessName) {
      suffix = `Process: ${processName} [${upid}]`;
    } else if (hasIdleThreads && hasTotalThreads) {
      const allIdle = idleThreads + (nullThreads ?? 0);
      suffix = `${count(totalThreads, 'thread')} of which ${count(allIdle, 'is', 'are')} idle.`;
    } else if (hasNullThreads && hasTotalThreads) {
      suffix = `${count(totalThreads, 'thread')} of which ${count(nullThreads, 'is', 'are')} not shown, having no data.`;
    } else if (hasTotalThreads) {
      suffix = `${count(totalThreads, 'thread')}.`;
    }

    const hasDescription = !!description;
    const hasSuffix = !!suffix;

    if (hasDescription && hasSuffix) {
      return `${description}\n\n${suffix}`;
    } else if (hasSuffix) {
      return suffix;
    } else {
      return description;
    }
  }

  // Infer a human-readable label (name) for a process counter track, from
  // information including the track name (as recorded in the trace database).
  static labelProcessCounter(args: Partial<{
        trackName: string|null,
      }>): string {
    const {trackName} = args;

    const hasTrackName = !!trackName;

    if (hasTrackName) {
      switch (trackName.toLowerCase()) {
        case 'mem.rss': return 'Resident Set Size';
        case 'mem.rss.anon': return 'Resident Anonymous';
        case 'mem.rss.file': return 'Resident File-backed';
        case 'mem.rss.shmem': return 'Resident Shared';
        case 'mem.rss.watermark': return 'Resident High Water Mark';
        case 'mem.locked': return 'Locked';
        case 'mem.swap': return 'Swapped Out';
        case 'mem.virt': return 'Virtual Memory Size';
        case 'oom_score_adj': return 'Out-of-memory Badness Adjustment';
        default: return trackName;
      }
    }

    return trackName ?? '';
  }

  // Infer a human-readable description for a process counter track, from
  // information including the track name (as recorded in the trace database).
  static describeProcessCounter(args: Partial<{
        trackName: string|null,
      }>): string | undefined {
    const {trackName} = args;

    const hasTrackName = !!trackName;

    if (hasTrackName) {
      switch (trackName.toLowerCase()) {
        case 'mem.rss': return 'Resident Set Size: total amount of resident memory for the process. This is the sum of anonymous, file-backed, and shared resident memory.';
        case 'mem.rss.anon': return 'Size of resident anonymous memory for the process.';
        case 'mem.rss.file': return 'Size of resident file mappings for the process.';
        case 'mem.rss.shmem': return 'Size of resident shared memory for the process, including System V shared memory, mappings from tmpfs, and shared anonymous mappings.';
        case 'mem.rss.watermark': return 'Peak resident set size for the process. As an historical maximum, it never decreases.';
        case 'mem.locked': return 'Size of physical pages that are locked into memory.';
        case 'mem.swap': return 'Size of virtual memory that is swapped out, not including shared-memory swap usage.';
        case 'mem.virt': return 'Total allocated virtual memory for the process.';
        case 'oom_score_adj': return 'Adjustment to the badness heuristic of the process for out-of-memory conditions. This value is added to the calculated badness of a process before determining which processes to kill. Scores range from 0 (never kill) to 1000 (always kill) and the adjustment recorded in this counter may vary from -1000 to 1000.';
        default: return undefined;
      }
    }

    return undefined;
  }

  // Get the trace-processor database track ID for a |track| to be created,
  // if it has one.
  static getTrackId(track: AddTrackArgs): number|undefined {
    return ('trackIds' in track.config && Array.isArray(track.config.trackIds)) ?
      track.config.trackIds[0] :
      undefined;
  }

  async guessCpuSizes(): Promise<Map<number, string>> {
    const cpuToSize = new Map<number, string>();
    await this.engine.query(`
      SELECT IMPORT('common.cpus');
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
    const groupId = this.lazyTrackGroup('CPU Usage',
      {collapsed: false, lazyParentGroup: this.cpuGroup});

    for (const cpu of cpus) {
      const size = cpuToSize.get(cpu);
      const name = size === undefined ? `Cpu ${cpu}` : `Cpu ${cpu} (${size})`;
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: CPU_SLICE_TRACK_KIND,
        trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
        name,
        trackGroup: groupId(),
        config: {
          cpu,
        },
      });
    }
  }

  // Group global counter tracks by the name of their parent track, if any, with
  // some exceptions:
  // - tracks that are already grouped by the time of this call are not re-grouped
  // - the 'Power' parent track induces a group named 'Battery'
  // - the 'Memory' parent track induces a group named 'Memory Usage'
  // - all tracks of |gpu_counter_track| type are grouped in 'GPU Counters'
  // - global counter tracks that don't have a parent track are grouped in
  //   an 'Other Counters' group
  async groupCounterTracks(): Promise<void> {
    type GroupDetails = Parameters<TrackDecider['lazyTrackGroup']>[1];
    const groupNameToGroupDetails = new Map<string, GroupDetails>();
    groupNameToGroupDetails.set('GPU Counters', {lazyParentGroup: this.gpuGroup});

    const groupingResult = await this.engine.query(`
      select track.id as track_id, track.name as track_name,
        (case when parent.name = 'Power' then 'Battery'
              when parent.name = 'Memory' then 'Memory Usage'
              else parent.name
         end) as group_name
      from track
      left join track as parent on track.parent_id = parent.id
      where track.type = 'counter_track' and track.name is not null
      union
      select track.id as track_id, track.name as track_name,
        'GPU Counters' as group_name
      from track
      where track.type = 'gpu_counter_track' and track.name is not null
    `);
    const trackNameToGroupName = new Map<string, string|null>();
    const iter = groupingResult.iter({track_name: STR, group_name: STR_NULL});
    for (; iter.valid(); iter.next()) {
      trackNameToGroupName.set(iter.track_name, iter.group_name);
    }

    for (const track of this.tracksToAdd) {
      if (track.kind !== COUNTER_TRACK_KIND ||
        (track.trackGroup && track.trackGroup !== SCROLLING_TRACK_GROUP)) {
        continue;
      }
      const groupName = trackNameToGroupName.get(track.name) ?? 'Other Counters';
      const groupIdProvider = this.lazyTrackGroup(groupName,
          groupNameToGroupDetails.get(groupName) ?? {});
      track.trackGroup = groupIdProvider();
    }
  }

  async addScrollJankTracks(engine: Engine): Promise<void> {
    const topLevelScrolls = addTopLevelScrollTrack(engine);
    const topLevelScrollsResult = await topLevelScrolls;
    let originalLength = this.tracksToAdd.length;
    this.tracksToAdd.length += topLevelScrollsResult.tracksToAdd.length;
    for (let i = 0; i < topLevelScrollsResult.tracksToAdd.length; ++i) {
      this.tracksToAdd[i + originalLength] =
          topLevelScrollsResult.tracksToAdd[i];
    }

    originalLength = this.tracksToAdd.length;
    const eventLatencies = addLatenciesTrack(engine);
    const eventLatencyResult = await eventLatencies;
    this.tracksToAdd.length += eventLatencyResult.tracksToAdd.length;
    for (let i = 0; i < eventLatencyResult.tracksToAdd.length; ++i) {
      this.tracksToAdd[i + originalLength] = eventLatencyResult.tracksToAdd[i];
    }
  }

  async addCpuFreqTracks(engine: EngineProxy): Promise<void> {
    const cpus = await this.engine.getCpus();

    const maxCpuFreqResult = await engine.query(`
    select ifnull(max(value), 0) as freq
    from counter c
    inner join cpu_counter_track t on c.track_id = t.id
    where name = 'cpufreq';
  `);
    const maxCpuFreq = maxCpuFreqResult.firstRow({freq: NUM}).freq;
    const groupId = this.lazyTrackGroup('CPU Frequencies',
      {lazyParentGroup: this.cpuGroup});

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
        ) as cpuIdleId,
        description
      from cpu_counter_track
      where name = 'cpufreq' and cpu = ${cpu}
      limit 1;
    `);

      if (cpuFreqIdleResult.numRows() > 0) {
        const row = cpuFreqIdleResult.firstRow({
          cpuFreqId: NUM,
          cpuIdleId: NUM_NULL,
          description: STR_NULL,
        });
        const freqTrackId = row.cpuFreqId;
        const idleTrackId = row.cpuIdleId === null ? undefined : row.cpuIdleId;
        const description = row.description ?? 'Values of the cpufreq counter for the CPU, along with CPU idle events.';

        this.tracksToAdd.push({
          engineId: this.engineId,
          kind: CPU_FREQ_TRACK_KIND,
          trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
          name: `Cpu ${cpu} Frequency`,
          description,
          trackGroup: groupId(),
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
      trackIds: STR,
      maxDepth: NUM,
    });

    const parentIdToGroupId = new Map<number, string>();
    let scrollJankRendered = false;

    for (; it.valid(); it.next()) {
      const kind = ASYNC_SLICE_TRACK_KIND;
      const rawName = it.name === null ? undefined : it.name;
      const rawParentName = it.parentName === null ? undefined : it.parentName;
      const name = TrackDecider.getTrackName({name: rawName, kind});
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const parentTrackId = it.parentId;
      const maxDepth = it.maxDepth;
      let trackGroup = SCROLLING_TRACK_GROUP;

      if (parentTrackId !== null) {
        const groupId = parentIdToGroupId.get(parentTrackId);
        if (groupId === undefined) {
          trackGroup = uuidv4();
          parentIdToGroupId.set(parentTrackId, trackGroup);

          const parentName =
              TrackDecider.getTrackName({name: rawParentName, kind});

          const summaryTrackId = uuidv4();
          this.tracksToAdd.push({
            id: summaryTrackId,
            engineId: this.engineId,
            kind: NULL_TRACK_KIND,
            trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
            trackGroup: undefined,
            name: parentName,
            config: {},
          });

          this.trackGroupsToAdd.push({
            engineId: this.engineId,
            summaryTrackId,
            name: parentName,
            id: trackGroup,
            collapsed: true,
          });
        } else {
          trackGroup = groupId;
        }
      }

      if (ENABLE_SCROLL_JANK_PLUGIN_V2.get() && !scrollJankRendered &&
          name.includes(INPUT_LATENCY_TRACK)) {
        // This ensures that the scroll jank tracks render above the tracks
        // for GestureScrollUpdate.
        await this.addScrollJankTracks(this.engine);
        scrollJankRendered = true;
      }
      const track = {
        engineId: this.engineId,
        kind,
        trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
        trackGroup,
        name,
        config: {
          maxDepth,
          trackIds,
        },
      };

      this.tracksToAdd.push(track);
    }
  }

  async addGpuFreqTracks(engine: EngineProxy): Promise<void> {
    const numGpus = await this.engine.getNumberOfGpus();
    const maxGpuFreqResult = await engine.query(`
    select ifnull(max(value), 0) as maximumValue
    from counter c
    inner join gpu_counter_track t on c.track_id = t.id
    where name = 'gpufreq';
  `);
    const maximumValue =
        maxGpuFreqResult.firstRow({maximumValue: NUM}).maximumValue;
    const groupId = this.lazyTrackGroup('GPU Frequencies',
      {lazyParentGroup: this.lazyTrackGroup('GPU', {collapsed: false})});

    for (let gpu = 0; gpu < numGpus; gpu++) {
      // Only add a gpu freq track if we have
      // gpu freq data.
      const freqExistsResult = await engine.query(`
      select id, description
      from gpu_counter_track
      where name = 'gpufreq' and gpu_id = ${gpu}
      limit 1;
    `);
      if (freqExistsResult.numRows() > 0) {
        const {id: trackId, description} = freqExistsResult.firstRow(
          {id: NUM, description: STR_NULL});
        this.tracksToAdd.push({
          engineId: this.engineId,
          kind: COUNTER_TRACK_KIND,
          name: `Gpu ${gpu} Frequency`,
          description: description ?? 'Values of the gpufreq counter.',
          trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
          trackGroup: groupId(),
          config: {
            trackId,
            maximumValue,
          },
        });
      }
    }
  }

  async addCpuPerfCounterTracks(engine: EngineProxy): Promise<void> {
    // Perf counter tracks are bound to CPUs, follow the scheduling and
    // frequency track naming convention ("Cpu N ...").
    // Note: we might not have a track for a given cpu if no data was seen from
    // it. This might look surprising in the UI, but placeholder tracks are
    // wasteful as there's no way of collapsing global counter tracks at the
    // moment.
    const result = await engine.query(`
      select printf("Cpu %u %s", cpu, name) as name, id, description
      from perf_counter_track as pct
      order by perf_session_id asc, pct.name asc, cpu asc
  `);

    const it = result.iter({
      name: STR,
      id: NUM,
      description: STR_NULL,
    });

    for (; it.valid(); it.next()) {
      const name = it.name;
      const trackId = it.id;
      const description = it.description ?? 'Values of counter samples for the CPU, from the traced_perf profiler.';
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: COUNTER_TRACK_KIND,
        name,
        description,
        trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
        trackGroup: SCROLLING_TRACK_GROUP,
        config: {
          name,
          trackId,
          scale: getCounterScale(name),
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

    this.trackGroupsToAdd.push({
      engineId: this.engineId,
      summaryTrackId,
      name: MEM_DMA_COUNTER_NAME,
      id,
      collapsed: true,
    });
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

      this.trackGroupsToAdd.push({
        engineId: this.engineId,
        summaryTrackId,
        name: groupName,
        id: value,
        collapsed: true,
      });
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

    this.trackGroupsToAdd.push({
      engineId: this.engineId,
      summaryTrackId,
      name: group,
      id,
      collapsed: true,
    });
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

      this.trackGroupsToAdd.push({
        engineId: this.engineId,
        summaryTrackId,
        name: groupName,
        id: value,
        collapsed: true,
      });
    }
  }

  async groupTracksByRegex(regex: RegExp, groupName: string): Promise<void> {
    let groupUuid = undefined;

    for (const track of this.tracksToAdd) {
      if (regex.test(track.name)) {
        if (groupUuid === undefined) {
          groupUuid = uuidv4();
        }
        track.trackGroup = groupUuid;
      }
    }

    if (groupUuid !== undefined) {
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

      this.trackGroupsToAdd.push({
        engineId: this.engineId,
        summaryTrackId,
        name: groupName,
        id: groupUuid,
        collapsed: true,
      });
    }
  }

  applyDefaultCounterScale(): void {
    for (const track of this.tracksToAdd) {
      if (track.kind === COUNTER_TRACK_KIND) {
        const scaleConfig = {
          scale: getCounterScale(track.name),
        };
        track.config = Object.assign({}, track.config, scaleConfig);
      }
    }
  }

  async addLogsTrack(engine: EngineProxy): Promise<void> {
    const result =
        await engine.query(`select count(1) as cnt from android_logs`);
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

  async addFtraceTrack(engine: EngineProxy): Promise<void> {
    const query = `select distinct cpu
          from ftrace_event
          where cpu + 1 > 1 or utid + 1 > 1`;

    const result = await engine.query(query);
    const it = result.iter({cpu: NUM});

    let groupUuid = undefined;
    let summaryTrackId = undefined;

    // use the first one as the summary track
    for (let row = 0; it.valid(); it.next(), row++) {
      if (groupUuid === undefined) {
        groupUuid = 'ftrace-track-group';
        summaryTrackId = uuidv4();
        this.tracksToAdd.push({
          engineId: this.engineId,
          kind: NULL_TRACK_KIND,
          trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
          name: `Ftrace Events`,
          trackGroup: undefined,
          config: {},
          id: summaryTrackId,
        });
      }
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: FTRACE_RAW_TRACK_KIND,
        trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
        name: `Ftrace Events Cpu ${it.cpu}`,
        trackGroup: groupUuid,
        config: {cpu: it.cpu},
      });
    }

    if (groupUuid !== undefined && summaryTrackId !== undefined) {
      this.trackGroupsToAdd.push({
        engineId: this.engineId,
        name: 'Ftrace Events',
        id: groupUuid,
        collapsed: true,
        summaryTrackId,
      });
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
      this.trackGroupsToAdd.push({
        engineId: this.engineId,
        summaryTrackId: groupIds.summaryTrackId,
        name: groupName,
        id: groupIds.id,
        collapsed: true,
      });
    }

    const counterResult = await engine.query(`
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

  async addThreadStateTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
      select
        utid,
        tid,
        upid,
        pid,
        process.name as processName,
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
      processName: STR_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const upid = it.upid;
      const processName = it.processName;
      const threadName = it.threadName;
      const uuid = this.getThreadProcessGroupUnchecked(utid, upid);
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
        description: TrackDecider.decorateTrackDescription(
          'Scheduling state of the thread.',
          {processName, upid}),
        trackGroup: uuid,
        trackSortKey: {
          utid,
          priority: InThreadTrackSortKey.THREAD_SCHEDULING_STATE_TRACK,
        },
        config: {utid, tid},
      });
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
      const group = this.getThreadProcessGroup(utid, upid);
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: CPU_PROFILE_TRACK_KIND,
        trackSortKey: {
          utid,
          priority: InThreadTrackSortKey.CPU_STACK_SAMPLES_TRACK,
        },
        name: `${threadName} (CPU Stack Samples)`,
        trackGroup: group,
        config: {utid},
      });
    }
  }

  async addThreadCounterTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
    select
      thread_counter_track.name as trackName,
      thread_counter_track.description as description,
      utid,
      upid,
      process.name as processName,
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
      description: STR_NULL,
      utid: NUM,
      upid: NUM_NULL,
      processName: STR_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
      startTs: LONG_NULL,
      trackId: NUM,
      endTs: LONG_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const upid = it.upid;
      const processName = it.processName;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const description = it.description?.trim() ?? undefined;
      const threadName = it.threadName;
      const group = this.getThreadProcessGroup(utid, upid);
      const startTs = it.startTs === null ? undefined : it.startTs;
      const endTs = it.endTs === null ? undefined : it.endTs;
      const kind = COUNTER_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, utid, tid, kind, threadName, threadTrack: true});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        description: TrackDecider.decorateTrackDescription(
          description,
          {processName, upid}),
        trackSortKey: {
          utid,
          priority: InThreadTrackSortKey.ORDINARY,
        },
        trackGroup: group,
        config: {
          name,
          trackId,
          startTs,
          endTs,
          tid,
        },
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
      maxDepth: NUM,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      const uuid = this.getUuid(0, upid);

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
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM,
    });
    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      const uuid = this.getUuid(0, upid);

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
      trackIds: STR,
      processName: STR_NULL,
      pid: NUM_NULL,
      maxDepth: NUM,
    });

    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const trackName = it.trackName;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const processName = it.processName;
      const pid = it.pid;
      const maxDepth = it.maxDepth;

      const uuid = this.getUuid(0, upid);

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
          max(slice.depth) as maxDepth,
          process.upid as upid,
          process.name as processName
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
      maxDepth: NUM,
      upid: NUM_NULL,
      processName: STR_NULL,
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
      const processName = it.processName;
      const maxDepth = it.maxDepth;

      const group = this.getThreadProcessGroup(utid, upid);

      const kind = SLICE_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, utid, tid, threadName, kind});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        description: TrackDecider.decorateTrackDescription(
          'Slices from userspace that explain what the thread was doing during the trace',
          {processName, upid}),
        trackGroup: group,
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
        },
      });

      if (TRACKS_V2_FLAG.get()) {
        this.tracksToAdd.push({
          engineId: this.engineId,
          kind: 'GenericSliceTrack',
          name,
          trackGroup: group,
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

  async addProcessCounterTracks(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
    select
      process_counter_track.id as trackId,
      process_counter_track.name as trackName,
      process_counter_track.description as description,
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
      description: STR_NULL,
      upid: NUM,
      pid: NUM_NULL,
      processName: STR_NULL,
      startTs: LONG_NULL,
      endTs: LONG_NULL,
    });

    const subgroupsByProcess: Record<string, Record<string, string>> = {};
    const createSubgroup = (kind: string, name: string,
        parentGroup: string) => {
      const subgroupId = this.lazyTrackGroup(name, {parentGroup})();
      const byProcess = subgroupsByProcess[kind] ?? {};
      subgroupsByProcess[kind] = byProcess;
      byProcess[parentGroup] = subgroupId;

      return subgroupId;
    };

    for (let i = 0; it.valid(); ++i, it.next()) {
      const pid = it.pid;
      const upid = it.upid;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const description = it.description?.trim() ?? undefined;
      const processName = it.processName;
      const uuid = this.getUuid(0, upid);
      const startTs = it.startTs === null ? undefined : it.startTs;
      const endTs = it.endTs === null ? undefined : it.endTs;
      const kind = COUNTER_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, upid, pid, kind, processName});

      // Lazily initialize the "Memory Usage" and Process Counters" subgroups
      // for this process

      const trackGroup = trackName?.startsWith('mem.') || trackName=== 'oom_score_adj' ?
        subgroupsByProcess['memUsage']?.[uuid] ??
          createSubgroup('memUsage', 'Memory Usage', uuid) :
        subgroupsByProcess['procCounters']?.[uuid] ??
          createSubgroup('procCounters', 'Process Counters', uuid);
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name: TrackDecider.labelProcessCounter({trackName}),
        description: TrackDecider.decorateTrackDescription(
          description ?? TrackDecider.describeProcessCounter({trackName}),
          {processName, upid}),
        trackSortKey: await this.resolveTrackSortKeyForProcessCounterTrack(
            upid, trackName || undefined),
        trackGroup,
        config: {
          name,
          trackId,
          startTs,
          endTs,
        },
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
        engineId: this.engineId,
        kind: HEAP_PROFILE_TRACK_KIND,
        trackSortKey: PrimaryTrackSortKey.HEAP_PROFILE_TRACK,
        name: `Heap Profile`,
        trackGroup: uuid,
        config: {upid},
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

  getThreadProcessGroupUnchecked(utid: number,
      upid: number|null): string|undefined {
    // Don't need the Idle Threads group in a process that is idle
    // because all of its threads would redundantly be in that group.
    // And don't create a group for just one idle thread.
    // And finally, idle has no meaning without CPU usage information
    const idle = !this.hasCpuUsage || upid === null ?
      undefined :
      this.idleUtids.get(upid);
    const isIdleProcess = upid !== null && this.idleUpids.has(upid);
    return idle !== undefined && !isIdleProcess &&
        (idle.has(utid) && idle.size > 1) ?
      this.getIdleThreadsGroup(utid, upid) :
      this.getUuidUnchecked(utid, upid);
  }

  getThreadProcessGroup(utid: number, upid: number|null): string {
    return assertExists(this.getThreadProcessGroupUnchecked(utid, upid));
  }

  getIdleThreadsGroup(utid: number, upid: number|null): string {
    const key = upid ?? 0;
    let result = this.idleThreadGroups.get(key);
    if (!result) {
      let name = 'Idle Threads (< 0.1%)';
      const idleThreads = this.idleUtids.get(key)?.size ?? 0;
      if (idleThreads > 0) {
        name = `${count(idleThreads, 'Idle thread')} (< 0.1%)`;
      }
      const nullThreads = this.nullThreadCounts.get(key) ?? 0;
      let description = 'An idle thread accounts for less than 0.1% of its process\'s total CPU time.';
      if (nullThreads > 0) {
        description = `${description}\n${count(nullThreads, 'additional idle thread')} ${pluralize(nullThreads, 'is', 'are')} not shown because ${pluralize(nullThreads, 'it has', 'they have')} no data.`;
      }

      // The group for the process that has this idle thread.
      const processGroup = this.getUuid(utid, upid);
      result = this.lazyTrackGroup(name,
        {description, collapsed: true, parentGroup: processGroup})();
      this.idleThreadGroups.set(key, result);
    }
    return result;
  }

  getTrackGroup(uuid: string): AddTrackGroupArgs|undefined {
    return this.trackGroupsToAdd.find((group) => group.id === uuid);
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
    const summaryTrackId = uuidv4();
    this.tracksToAdd.push({
      id: summaryTrackId,
      engineId: this.engineId,
      kind: PROCESS_SUMMARY_TRACK,
      trackSortKey: PrimaryTrackSortKey.PROCESS_SUMMARY_TRACK,
      name: `Kernel thread summary`,
      config: {pidForColor: 2, upid: it.upid, utid: it.utid},
    });
    this.trackGroupsToAdd.push({
      engineId: this.engineId,
      summaryTrackId,
      name: `Kernel threads`,
      id: kthreadGroupUuid,
      collapsed: true,
    });

    // Set the group for all kernel threads (including kthreadd itself).
    for (; it.valid(); it.next()) {
      this.setUuidForUpid(it.upid, kthreadGroupUuid);
    }
  }

  async addProcessTrackGroups(engine: EngineProxy): Promise<void> {
    // Map of process upid to its track group descriptor
    const processTrackGroups = new Map<string, AddTrackGroupArgs>();

    // Map of idle process upid to its track group for descriptor. But if we
    // didn't capture CPU information then this concept has no meaning
    const idleProcessTrackGroups = this.hasCpuUsage ?
      new Map<string, AddTrackGroupArgs>() :
      undefined;

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
      total_dur,
      thread_dur,
      hasHeapProfiles,
      process.pid as pid,
      thread.tid as tid,
      process.name as processName,
      thread.name as threadName,
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
      select utid, sum(dur) as thread_dur
      from sched where dur != -1 and utid != 0
      group by utid
    ) using(utid)
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
      total_dur: NUM_NULL,
      thread_dur: NUM_NULL,
      hasHeapProfiles: NUM_NULL,
      chromeProcessLabels: STR,
    });

   const idleProcessesGroupId = idleProcessTrackGroups ?
      this.lazyTrackGroup('Idle Processes (< 0.1%)',
        {collapsed: true,
          description: 'CPU usage of an idle process accounts for less than 0.1% of the total trace duration.',
          lazyParentGroup: this.processesGroup}) :
      undefined;

    // An "idle process" is measured against the duration of the trace.
    // The threshold is 0.1%, or one one-thousandth, of the trace time.
    const traceTime = globals.state.traceTime;
    const traceDuration = traceTime.end - traceTime.start;
    const idleProcessThreshold = Number(traceDuration) / IDLE_FACTOR;

    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const upid = it.upid;
      const pid = it.pid;
      const threadName = it.threadName;
      const processName = it.processName;
      const hasSched = !!it.total_dur;
      const hasHeapProfiles = !!it.hasHeapProfiles;

      const idleProcess = (upid !== null) && (
        (it.total_dur === null) || (it.total_dur < idleProcessThreshold));
      if (idleProcess) {
        // Track the process that is idle but will show a track (idle processes
        // that have no data at all will not show a track)
        this.idleUpids.add(upid);
      } else if (upid !== null) {
        // In case a previous query result row had it idle
        this.idleUpids.delete(upid);
      }

      // An "idle thread" is measured against its process's total CPU time
      // not the duration of the trace
      const idleThreadThreshold = (it.total_dur ?? 0) / IDLE_FACTOR;
      // If the total duration is NULL, that means we found no slices for the
      // thread, so it is manifestly idle. We do not distinguish here between
      // "null threads" (no track created) and "idle threads" (having a track)
      // because that is done in the grouping of idle threads elsewhere.
      // NOTE: the faked `0` utid must not be counted amongst the idle threads
      //       because it doesn't exist
      const idleThread = (utid !== 0) &&
        (it.thread_dur === null || it.thread_dur < idleThreadThreshold);
      if (idleThread) {
        const key = upid ?? 0;
        let mostlyIdleUtids = this.idleUtids.get(key);
        if (mostlyIdleUtids === undefined) {
          mostlyIdleUtids = new Set();
          this.idleUtids.set(key, mostlyIdleUtids);
        }
        mostlyIdleUtids.add(utid);
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
          labels: it.chromeProcessLabels.split(','),
        });

        const name = TrackDecider.getTrackName(
            {utid, processName, pid, threadName, tid, upid});
        const trackGroup: AddTrackGroupArgs = {
          engineId: this.engineId,
          summaryTrackId,
          name,
          description: name,
          id: pUuid,
          // Perf profiling tracks remain collapsed, otherwise we would have too
          // many expanded process tracks for some perf traces, leading to
          // jankyness.
          collapsed: !hasHeapProfiles,
          parentGroup: !idleProcess || !idleProcessesGroupId ?
            this.processesGroup() :
            idleProcessesGroupId(),
        };
        this.trackGroupsToAdd.push(trackGroup);
        processTrackGroups.set(pUuid, trackGroup);
        if (idleProcess && idleProcessTrackGroups) {
          idleProcessTrackGroups.set(pUuid, trackGroup);
        }
      }
    }

    // Count threads per process
    const threadsPerProcess = await engine.query(`
    select
      upid, total_threads, null_threads
    from
      (select upid, count(*) as total_threads
       from process join thread using (upid)
       where upid != 0 and utid != 0
       group by upid)
      left outer join
      (select upid, count(*) as null_threads
       from thread left outer join sched using (utid)
       where upid != 0 and sched.utid is null
       group by upid) using (upid)
    `);

    const totalProcessCount = threadsPerProcess.numRows();
    const shownProcessCount = processTrackGroups.size;
    const idleProcessCount = idleProcessTrackGroups?.size ?? 0;
    const nullProcessCount = totalProcessCount - shownProcessCount;

    if (shownProcessCount > 0) {
      const processesGroup = this.getTrackGroup(this.processesGroup());
      if (processesGroup) {
        processesGroup.name = `Processes (${shownProcessCount})`;
        const idleOrNull = idleProcessCount + nullProcessCount;
        processesGroup.description = `There ${pluralize(totalProcessCount, 'is', 'are')} ${count(totalProcessCount, 'process', 'processes')} in total.`;
        if (idleOrNull > 0) {
          processesGroup.description = `${processesGroup.description}\nOf these, ${count(idleOrNull, 'process is', 'processes are')} idle.`;
        }
        if (idleProcessCount <= 0 && nullProcessCount > 0) {
          processesGroup.description = `${processesGroup.description}\nNo idle processes are shown because they recorded no data.`;
        }
      }
    }
    if (idleProcessCount > 0 && idleProcessesGroupId) {
      // There are idle processes actually showing tracks
      const idleProcessesGroup = this.getTrackGroup(idleProcessesGroupId());
      if (idleProcessesGroup) {
        if (idleProcessCount === 1) {
          // Don't create a group for just a single member
          idleProcessesGroupId.revoke();
        } else {
          idleProcessesGroup.name = `${idleProcessCount} Idle Processes (< 0.1%)`;
          if (nullProcessCount > 0) {
          const nullProcessesMessage = `${nullProcessCount} additional idle ${pluralize(nullProcessCount, 'process is', 'processes are')} not shown, having no data at all.`;
            idleProcessesGroup.description = `${idleProcessesGroup.description}\n${nullProcessesMessage}`;
          }
        }
      }
    }

    // Update process group descriptions with thread counts
    const tppIt = threadsPerProcess.iter({
      upid: NUM,
      total_threads: NUM,
      null_threads: NUM_NULL,
    });
    for (; tppIt.valid(); tppIt.next()) {
      const upid = tppIt.upid;
      const totalThreads = tppIt.total_threads;
      const idleThreads = this.idleUtids.get(upid)?.size ?? 0;
      const nullThreads = tppIt.null_threads;

      if (nullThreads !== null) {
        this.nullThreadCounts.set(upid, nullThreads);
      }

      const uuid = this.upidToUuid.get(upid);
      const trackGroup = processTrackGroups.get(uuid ?? '');
      if (trackGroup && trackGroup.id === uuid) {
        trackGroup.description = TrackDecider.decorateTrackDescription(
          undefined,
          {totalThreads, idleThreads, nullThreads});
      }
    }
  }

  async addSurfaceFlingerTrackGroups(engine: EngineProxy): Promise<void> {
    const result = await engine.query(`
    select distinct gpu_track.id as trackId, frame_slice.layer_name as layerName
    from frame_slice join gpu_track on (frame_slice.track_id = gpu_track.id)
    where
      gpu_track.scope = 'graphics_frame_event'
      and gpu_track.name is not null
      and frame_slice.layer_name is not null
    `);

    const it = result.iter({
      trackId: NUM,
      layerName: STR,
    });

    // Layer names by track ID from the trace DB
    const layersByTrack = new Map<number, string>();
    for (; it.valid(); it.next()) {
      const trackId = it.trackId;
      const layerName = it.layerName;
      layersByTrack.set(trackId, layerName);
    }

    const layerGroup = (layerName: string) => this.lazyTrackGroup(
      `Layer - ${layerName}`, {lazyParentGroup: this.sfEventsGroup});
    const layerSubgroup = (layerName: string, subgroup: string) =>
      this.lazyTrackGroup(subgroup, {lazyParentGroup: layerGroup(layerName)});

    for (const track of this.tracksToAdd) {
      if (track.trackGroup === SCROLLING_TRACK_GROUP) {
        const trackId = TrackDecider.getTrackId(track);
        const layerName = trackId !== undefined ?
          layersByTrack.get(trackId) :
          undefined;
        if (layerName) {
          const subgroupName = track.name.startsWith('Buffer:') ? 'Buffers' : undefined;
          if (subgroupName) {
            // Group the track
            track.trackGroup = layerSubgroup(layerName, subgroupName)();
            // And rename it
            const bufferMatch = /^Buffer: (\d+)?/.exec(track.name);
            if (bufferMatch) {
              track.description = track.description ?? track.name;
              track.name = `Buffer ${bufferMatch[1]}`;
            }
          } else {
            track.trackGroup = layerGroup(layerName)();

            // Rename the track, if applicable
            const bufferMatch = /^(SF|APP|GPU|Display)_(\d+)?/
              .exec(track.name);
            if (bufferMatch) {
              switch (bufferMatch[1]) {
                case 'APP':
                  track.name = `Application - Buffer ${bufferMatch[2]}`;
                  track.description = track.description ?? 'The time from when the buffer was dequeued by the app to when it was enqueued back.';
                  break;
                case 'GPU':
                  track.name = `Wait for GPU - Buffer ${bufferMatch[2]}`;
                  track.description = track.description ?? 'The duration the buffer was owned by the GPU. This is the time from when the buffer was sent to the GPU to when the GPU finished its work on the buffer. This does not indicate that the GPU was working only on this buffer during this time.';
                  break;
                case 'SF':
                  track.name = `Composition - Buffer ${bufferMatch[2]}`;
                  track.description = track.description ?? 'The time from when SurfaceFlinger latched on to the buffer and sent for composition to when it was sent to the display.';
                  break;
                case 'Display':
                  track.name = 'On Display';
                  track.description = track.description ?? 'The duration the frame was displayed on screen.';
                  break;
              }
            }
          }
        }
      }
    }
  }

  async groupGpuTracks(engine: EngineProxy): Promise<void> {
    const groupsToCollect: AddTrackGroupArgs[] = [];
    for (const group of this.trackGroupsToAdd) {
      if (group.name.startsWith('GPU ') && !group.parentGroup) {
        groupsToCollect.push(group);
      }
    }
    groupsToCollect.forEach((group) => group.parentGroup = this.gpuGroup());

    // Collect some tracks, too
    const result = await engine.query(`
    select id, scope
    from gpu_track
    where scope is not null;
    `);
    if (result.numRows() === 0) {
      // No GPU tracks to group
      return;
    }

    const gpuQueueTrackIds = new Set<number>();
    const it = result.iter({
      id: NUM,
      scope: STR,
    });
    for (; it.valid(); it.next()) {
      switch (it.scope) {
        case 'gpu_render_stage':
          gpuQueueTrackIds.add(it.id);
          break;
        case 'vulkan_events':
          gpuQueueTrackIds.add(it.id);
          break;
        default:
          break; // Group TBD
      }
    }

    const gpuQueuesGroup = this.lazyTrackGroup('GPU Queues',
      {lazyParentGroup: this.gpuGroup});
    for (const track of this.tracksToAdd) {
      if (track.trackGroup === SCROLLING_TRACK_GROUP) {
        const trackId = TrackDecider.getTrackId(track);
        if (trackId === undefined) {
          continue;
        }
        if (gpuQueueTrackIds.has(trackId)) {
          track.trackGroup = gpuQueuesGroup();
        }
      }
    }
  }

  async groupMetricTracks(engine: EngineProxy): Promise<void> {
    if (!globals.state.metrics.availableMetrics?.length) {
      return;
    }

    const metricTableNames = globals.state.metrics.availableMetrics
      .map((metric) => `${metric}_event`)
      .map(sqliteString)
      .join(',');
    const metricTableNamesThatExist = new Set<string>();
    const result = await engine.query(`
      select name from sqlite_master
      where type in ('table','view') and
      name in (${metricTableNames})
    `);
    const it = result.iter({name: STR});
    for (; it.valid(); it.next()) {
      metricTableNamesThatExist.add(it.name);
    }

    const metricTrackGroupings = new Map<string, Set<string>>();
    for (const metric of globals.state.metrics.availableMetrics) {
      const tableName = `${metric}_event`;
      if (!metricTableNamesThatExist.has(tableName)) {
        continue;
      }

      const result = await engine.query(`
        select distinct track_name from ${tableName};
      `);
      if (!result.numRows()) {
        continue;
      }

      const it = result.iter({track_name: STR});
      const tracks = new Set<string>();
      metricTrackGroupings.set(METRIC_NAMES[metric], tracks);

      for (; it.valid(); it.next()) {
        tracks.add(it.track_name);
      }
    }

    for (const [groupName, tracks] of metricTrackGroupings) {
      const groupId = this.lazyTrackGroup(groupName,
        {description: `Results from calculation of the ${groupName} metric.`});
      // Don't create a group of just one track but do add
      // to a group if it already exists
      if (tracks.size < 2 && !groupId.exists()) {
        continue;
      }
      for (const track of this.tracksToAdd) {
        if (track.trackGroup === SCROLLING_TRACK_GROUP &&
              tracks.has(track.name)) {
            track.trackGroup = groupId();
        }
      }
    }
  }

  // Assign titles for tracks that needs user-friendly names in the UI.
  // Don't change the track name because it may originate in the trace
  // database and so be used for correlation purposes.
  setTrackTitles(): void {
    this.tracksToAdd.forEach((track) => track.title = TRACK_TITLES[track.name]);
  }

  sortTopTrackGroups(): void {
    // Must create parent groups before subgroups.
    const topGroups: AddTrackGroupArgs[] = [];
    for (let i = 0; i < this.trackGroupsToAdd.length; i++) {
      const group = this.trackGroupsToAdd[i];
      if (!group.parentGroup) {
        topGroups.push(group);
        this.trackGroupsToAdd.splice(i, 1);
        i--;
      }
    }

    // Sort the top-level Processes group to the bottom
    const processesRegex = /Processes \(\d+\)/;
    const comparator = (g1: AddTrackGroupArgs, g2: AddTrackGroupArgs) => {
      if (g1 === g2) {
        return 0;
      }
      if (g1.name.match(processesRegex)) {
        return +1; // Last
      }
      if (g2.name.match(processesRegex)) {
        return -1; // Last
      }
      return g1.name.localeCompare(g2.name);
    };

    topGroups.sort(comparator);

    // And put SurfaceFlinger Events (if exists) after GPU (if exists)
    const sfEventsIndex = this.sfEventsGroup.exists() ?
      topGroups.findIndex((group) => group.id === this.sfEventsGroup()) :
      -1;
    const gpuGroupIndex = this.gpuGroup.exists() ?
      topGroups.findIndex((group) => group.id === this.gpuGroup()) :
      -1;
    if (sfEventsIndex >= 0 && gpuGroupIndex >= 0) {
      const move = topGroups.splice(sfEventsIndex, 1);
      topGroups.splice(gpuGroupIndex + 1, 0, ...move);
    }
    this.trackGroupsToAdd.unshift(...topGroups);
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

  private async defineMaxLayoutDepthSqlFunction(): Promise<void> {
    await this.engine.query(`
      select create_function(
        'max_layout_depth(track_count INT, track_ids STRING)',
        'INT',
        '
          select ifnull(iif(
            $track_count = 1,
            (
              select max(depth)
              from slice
              where track_id = cast($track_ids AS int)
            ),
            (
              select max(layout_depth)
              from experimental_slice_layout($track_ids)
            )
          ), 0);
        '
      );
    `);
  }

  async addPluginTracks(): Promise<void> {
    const promises = pluginManager.findPotentialTracks(this.engine);
    const results = await Promise.all(promises);

    const grouperator = (track: TrackInfo): string => {
      if (track.group) {
        return this.lazyTrackGroup(track.group)();
      }
      if (track.groups) {
        let parentGroupIdProvider: LazyIdProvider | undefined = undefined;
        for (const group of track.groups) {
          parentGroupIdProvider = this.lazyTrackGroup(group, {lazyParentGroup: parentGroupIdProvider});
        }
        return parentGroupIdProvider!();
      }

      return SCROLLING_TRACK_GROUP;
    };

    for (const infos of results) {
      for (const info of infos) {
        this.tracksToAdd.push({
          engineId: this.engineId,
          kind: info.trackKind,
          name: info.name,
          description: info.description,
          // TODO(hjd): Fix how sorting works. Plugins should expose
          // 'sort keys' which the user can use to choose a sort order.
          trackSortKey: PrimaryTrackSortKey.COUNTER_TRACK,
          trackGroup: grouperator(info),
          config: info.config,
        });
      }
    }
  }

  async decideTracks(filterTracks = false): Promise<DeferredAction[]> {
    await this.defineMaxLayoutDepthSqlFunction();

    const cpus = await this.engine.getCpus();
    this.hasCpuUsage = cpus.length > 0;

    // Add first the global tracks that don't require per-process track groups.
    await this.addCpuSchedulingTracks();
    await this.addFtraceTrack(
        this.engine.getProxy('TrackDecider::addFtraceTrack'));
    await this.addCpuFreqTracks(
        this.engine.getProxy('TrackDecider::addCpuFreqTracks'));
    await this.addGlobalAsyncTracks(
        this.engine.getProxy('TrackDecider::addGlobalAsyncTracks'));
    await this.addGpuFreqTracks(
        this.engine.getProxy('TrackDecider::addGpuFreqTracks'));
    await this.addCpuPerfCounterTracks(
        this.engine.getProxy('TrackDecider::addCpuPerfCounterTracks'));
    await this.addPluginTracks();
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

    await this.groupMetricTracks(
        this.engine.getProxy('TrackDecider::groupMetricTracks'));

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
    // Will populate this.trackGroupsToAdd
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

    await this.groupGpuTracks(
      this.engine.getProxy('TrackDecider::groupGpuTracks'));
    await this.addSurfaceFlingerTrackGroups(
      this.engine.getProxy('TrackDecider::addSurfaceflingerTrackGroups'));
    await this.groupCounterTracks();

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

    this.setTrackTitles();

    this.sortTopTrackGroups();

    const actions: DeferredAction[] = [];
    if (filterTracks) {
      const rejected = this.filterTracks();
      actions.push(Actions.setFilteredTracks({
        filteredTracks: rejected,
      }));
    }

    const topLvlTracks = this.tracksToAdd.filter((track, index)=> {
      if (!track.trackGroup || track.trackGroup === SCROLLING_TRACK_GROUP) {
        this.tracksToAdd.splice(index, 1);
        return true;
      }
      return false;
    });
    actions.push(Actions.addTracks({tracks: topLvlTracks}));
    actions.push(Actions.addTrackGroups({trackGroups: this.trackGroupsToAdd}));
    actions.push(Actions.addTracks({tracks: this.tracksToAdd}));

    const threadOrderingMetadata = await this.computeThreadOrderingMetadata();
    actions.push(Actions.setUtidToTrackSortKey({threadOrderingMetadata}));
    const idleGroups: string[] = [];
    for (const group of Object.values(this.trackGroupsToAdd)) {
      if (group.name.search(/\bIdle\b/) >= 0) {
        idleGroups.push(group.id);
      }
    }
    actions.push(Actions.moveTrackGroupsToBottom({ids: idleGroups}));

    this.applyDefaultCounterScale();

    return actions;
  }

  /**
   * Filter tracks and track groups. Any tracks belonging to an excluded
   * group are themselves excluded regardless of track-specific filters.
   * Any tracks not otherwise excluded that belong to an included group
   * are implicitly included.
   *
   * @return {AddTrackLikeArgs[]} the tracks and track groups rejected
   *   by the filter
   */
  private filterTracks(): AddTrackLikeArgs[] {
    const rejected: AddTrackLikeArgs[] = [];

    const includedTrackGroupIds = new Set<string>();
    const excludedTrackGroupIds = new Set<string>();
    const includedTrackGroupSummaryTrackIds = new Set<string>();
    const excludedTrackGroupSummaryTrackIds = new Set<string>();

    this.trackGroupsToAdd = this.trackGroupsToAdd.filter((group) => {
      const included = shouldCreateTrackGroup(group);
      if (included) {
        includedTrackGroupIds.add(group.id);
        includedTrackGroupSummaryTrackIds.add(group.summaryTrackId);
      } else {
        rejected.push(group);
        excludedTrackGroupIds.add(group.id);
        excludedTrackGroupSummaryTrackIds.add(group.summaryTrackId);
      }
      return included;
    });

    const hasTrackGroup =
    (track: AddTrackArgs): track is AddTrackArgs & {trackGroup: string} =>
        track.trackGroup !== undefined &&
        track.trackGroup !== SCROLLING_TRACK_GROUP;
    const hasId = (track: AddTrackArgs): track is AddTrackArgs & {id: string} =>
        track.id !== undefined;
    const includedByGroup = (track: AddTrackArgs) =>
    (hasTrackGroup(track) && includedTrackGroupIds.has(track.trackGroup)) ||
        (hasId(track) && includedTrackGroupSummaryTrackIds.has(track.id));
    const excludedByGroup = (track: AddTrackArgs) =>
    (hasTrackGroup(track) && excludedTrackGroupIds.has(track.trackGroup)) ||
        (hasId(track) && excludedTrackGroupSummaryTrackIds.has(track.id));
    const trackFilter = (track: AddTrackArgs) => {
      const included =
      shouldCreateTrack(track,
        (track)=>{
          if (hasTrackGroup(track)) {
            return includedByGroup(track);
          } else {
            return true;
          }
      }) && !excludedByGroup(track);
      if (!included) {
        rejected.push(track);
      }
      return included;
    };
    this.tracksToAdd = this.tracksToAdd.filter(trackFilter);

    return rejected;
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

  // Obtain a function that will create a group of the given |name| only when
  // it is actually needed to get the containing group ID for some track.
  // When invoked, the returned function returns the ID of the group created
  // at that moment or earlier and cached.
  lazyTrackGroup(name: string,
      details?: LazyTrackGroupArgs): LazyIdProvider {
    const parent = details?.lazyParentGroup ?
      (details.lazyParentGroup as LazyIdProvider & {node: LazyTrackGroupTree})
        .node :
      details?.parentGroup ?
        this.getLazyTrackGroupTree(details.parentGroup) :
        this.lazyTrackGroups;
    let result = parent.children.find((child) => child.name === name);
    if (!result) {
      const path = this.getGroupPath(parent);
      path.push(name);

      const node: LazyTrackGroupTree = {
        name,
        parent,
        children: [],
        group: this.findGroup(path),
        id: ((): string => {
          // First, try to find a group that exists
          if (!node.group) {
            node.group = this.findGroup(path);
          }
          // Otherwise, create it
          if (!node.group) {
            node.group = this.createPureTrackGroup(
                uuidv4(), name, details);
          }
          return node.group.id;
        }) as LazyIdProvider,
      };
      parent.children.push(node);

      Object.assign(node.id, {
        node,
        exists: () => !!node.group,
        revoke: () => {
          if (node.group) {
            this.removeTrackGroup(node.group.id);
          }
        },
      });
      result = node;
    }
    return result.id;
  }

  // Find a group previously added to the |trackGroupsToAdd| property
  // by the |path| of group names from the top of the UI.
  private findGroup(path: string[]): AddTrackGroupArgs | undefined {
    if (path.length === 0) {
      return undefined;
    }

    const name = path[path.length - 1];
    const parentPath = path.slice(0, -1);
    for (const group of this.trackGroupsToAdd) {
      if (group.name === name) {
        // Candidate. Does it not have a parent?
        if (parentPath.length === 0 &&
            (!group.parentGroup ||
              group.parentGroup === SCROLLING_TRACK_GROUP)) {
          return group;
        }

        // Otherwise, does its parent match?
        const parent = this.findGroup(parentPath);
        if (parent?.id && (parent?.id === group.parentGroup)) {
          return group;
        }
      }
    }

    return undefined;
  }

  // Get the tree node book-keeping the lazily created group of the
  // given ID.
  //
  // Preconditions: the |groupId| is the ID of a group that actually has
  //     been added to the |trackGroupsToAdd| property. i.e., if it is
  //     a lazily-created group then that lazy creation has occurred
  private getLazyTrackGroupTree(groupId: string): LazyTrackGroupTree {
    const group = assertExists(
      this.trackGroupsToAdd.find((group) => group.id === groupId));
    const name = group.name;
    let result: LazyIdProvider;
    if (!group.parentGroup || group.parentGroup === SCROLLING_TRACK_GROUP) {
      result = this.lazyTrackGroup(name);
    } else {
      const lazyParentGroup = this.getLazyTrackGroupTree(group.parentGroup).id;
      result = this.lazyTrackGroup(name, {lazyParentGroup});
    }
    return (result as LazyIdProvider & {node: LazyTrackGroupTree}).node;
  }

  // Get the path from the root of the UI to the group represented by the
  // given lazy-track-group tree |node|.
  private getGroupPath(node: LazyTrackGroupTree): string[] {
    if (node === this.lazyTrackGroups) {
      return [];
    }
    const result = this.getGroupPath(node.parent ?? this.lazyTrackGroups);
    result.push(node.name);
    return result;
  }

  createPureTrackGroup(id: string, name: string,
      details: LazyTrackGroupArgs = {}): AddTrackGroupArgs {
    const {lazyParentGroup, ...staticDetails} = details;
    const summaryTrackId = id+':summary';
    const result: AddTrackGroupArgs = {
      id,
      engineId: this.engineId,
      name,
      summaryTrackId, // Group needs a summary track, even if it's blank
      collapsed: true,
      ...staticDetails,
    };
    if (lazyParentGroup) {
      result.parentGroup = lazyParentGroup();
    }

    this.trackGroupsToAdd.push(result);
    this.tracksToAdd.push(this.blankSummaryTrack(summaryTrackId));
    return result;
  }

  blankSummaryTrack(id: string): AddTrackArgs {
    return {
      engineId: this.engineId,
      id: id,
      kind: NULL_TRACK_KIND,
      name: id,
      trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
      trackGroup: undefined,
      config: {},
    };
  }

  protected removeTrackGroup(id: string): void {
    const groupIndex = this.trackGroupsToAdd.findIndex(
      (group) => group.id === id);
    if (groupIndex < 0) {
      return; // Nothing to remove
    }
    const [group] = this.trackGroupsToAdd.splice(groupIndex, 1);
    const parentGroup = group.parentGroup;

    // remove the summary track
    const summary = this.tracksToAdd.findIndex(
      (track) => track.id === group?.summaryTrackId);
    if (summary >= 0) {
      this.tracksToAdd.splice(summary, 1);
    }

    // And re-group all members
    this.tracksToAdd.forEach((track) => {
      if (track.trackGroup === id) {
        track.trackGroup = parentGroup ?? SCROLLING_TRACK_GROUP;
      }
    });
    this.trackGroupsToAdd.forEach((trackGroup) => {
      if (trackGroup.parentGroup === id) {
        trackGroup.parentGroup = parentGroup;
      }
    });
  }
}

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

import * as uuidv4 from 'uuid/v4';

import {
  Actions,
  AddTrackArgs,
  DeferredAction,
} from '../common/actions';
import {Engine} from '../common/engine';
import {
  iter,
  NUM,
  NUM_NULL,
  slowlyCountRows,
  STR,
  STR_NULL,
} from '../common/query_iterator';
import {SCROLLING_TRACK_GROUP} from '../common/state';
import {ACTUAL_FRAMES_SLICE_TRACK_KIND} from '../tracks/actual_frames/common';
import {ANDROID_LOGS_TRACK_KIND} from '../tracks/android_log/common';
import {ASYNC_SLICE_TRACK_KIND} from '../tracks/async_slices/common';
import {SLICE_TRACK_KIND} from '../tracks/chrome_slices/common';
import {COUNTER_TRACK_KIND} from '../tracks/counter/common';
import {CPU_FREQ_TRACK_KIND} from '../tracks/cpu_freq/common';
import {CPU_PROFILE_TRACK_KIND} from '../tracks/cpu_profile/common';
import {CPU_SLICE_TRACK_KIND} from '../tracks/cpu_slices/common';
import {
  EXPECTED_FRAMES_SLICE_TRACK_KIND
} from '../tracks/expected_frames/common';
import {HEAP_PROFILE_TRACK_KIND} from '../tracks/heap_profile/common';
import {
  PROCESS_SCHEDULING_TRACK_KIND
} from '../tracks/process_scheduling/common';
import {PROCESS_SUMMARY_TRACK} from '../tracks/process_summary/common';
import {THREAD_STATE_TRACK_KIND} from '../tracks/thread_state/common';

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
      threadTrack
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
      return `${kind}`;
    }
    return 'Unknown';
  }

  async addCpuSchedulingTracks(): Promise<void> {
    const cpus = await this.engine.getCpus();
    for (const cpu of cpus) {
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: CPU_SLICE_TRACK_KIND,
        name: `Cpu ${cpu}`,
        trackGroup: SCROLLING_TRACK_GROUP,
        config: {
          cpu,
        }
      });
    }
  }

  async addCpuFreqTracks(): Promise<void> {
    const cpus = await this.engine.getCpus();

    const maxCpuFreq = await this.engine.query(`
    select max(value)
    from counter c
    inner join cpu_counter_track t on c.track_id = t.id
    where name = 'cpufreq';
  `);

    for (const cpu of cpus) {
      // Only add a cpu freq track if we have
      // cpu freq data.
      // TODO(taylori): Find a way to display cpu idle
      // events even if there are no cpu freq events.
      const cpuFreqIdle = await this.engine.query(`
      select
        id as cpu_freq_id,
        (
          select id
          from cpu_counter_track
          where name = 'cpuidle'
          and cpu = ${cpu}
          limit 1
        ) as cpu_idle_id
      from cpu_counter_track
      where name = 'cpufreq' and cpu = ${cpu}
      limit 1;
    `);
      if (slowlyCountRows(cpuFreqIdle) > 0) {
        const freqTrackId = +cpuFreqIdle.columns[0].longValues![0];

        const idleTrackExists: boolean = !cpuFreqIdle.columns[1].isNulls![0];
        const idleTrackId = idleTrackExists ?
            +cpuFreqIdle.columns[1].longValues![0] :
            undefined;

        this.tracksToAdd.push({
          engineId: this.engineId,
          kind: CPU_FREQ_TRACK_KIND,
          name: `Cpu ${cpu} Frequency`,
          trackGroup: SCROLLING_TRACK_GROUP,
          config: {
            cpu,
            maximumValue: +maxCpuFreq.columns[0].doubleValues![0],
            freqTrackId,
            idleTrackId,
          }
        });
      }
    }
  }

  async addGlobalAsyncTracks(): Promise<void> {
    const rawGlobalAsyncTracks = await this.engine.query(`
    SELECT
      t.name,
      t.track_ids,
      MAX(experimental_slice_layout.layout_depth) as max_depth
    FROM (
      SELECT name, GROUP_CONCAT(track.id) AS track_ids
      FROM track
      WHERE track.type = "track"
      GROUP BY name
    ) AS t CROSS JOIN experimental_slice_layout
    WHERE t.track_ids = experimental_slice_layout.filter_track_ids
    GROUP BY t.track_ids;
  `);
    for (let i = 0; i < slowlyCountRows(rawGlobalAsyncTracks); i++) {
      const name = rawGlobalAsyncTracks.columns[0].stringValues![i];
      const rawTrackIds = rawGlobalAsyncTracks.columns[1].stringValues![i];
      const trackIds = rawTrackIds.split(',').map(v => Number(v));
      const maxDepth = +rawGlobalAsyncTracks.columns[2].longValues![i];
      const track = {
        engineId: this.engineId,
        kind: ASYNC_SLICE_TRACK_KIND,
        trackGroup: SCROLLING_TRACK_GROUP,
        name,
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
    const maxGpuFreq = await this.engine.query(`
    select max(value)
    from counter c
    inner join gpu_counter_track t on c.track_id = t.id
    where name = 'gpufreq';
  `);

    for (let gpu = 0; gpu < numGpus; gpu++) {
      // Only add a gpu freq track if we have
      // gpu freq data.
      const freqExists = await this.engine.query(`
      select id
      from gpu_counter_track
      where name = 'gpufreq' and gpu_id = ${gpu}
      limit 1;
    `);
      if (slowlyCountRows(freqExists) > 0) {
        this.tracksToAdd.push({
          engineId: this.engineId,
          kind: COUNTER_TRACK_KIND,
          name: `Gpu ${gpu} Frequency`,
          trackGroup: SCROLLING_TRACK_GROUP,
          config: {
            trackId: +freqExists.columns[0].longValues![0],
            maximumValue: +maxGpuFreq.columns[0].doubleValues![0],
          }
        });
      }
    }
  }

  async addGlobalCounterTracks(): Promise<void> {
    // Add global or GPU counter tracks that are not bound to any pid/tid.
    const globalCounters = await this.engine.query(`
    select name, id
    from counter_track
    where type = 'counter_track'
    union
    select name, id
    from gpu_counter_track
    where name != 'gpufreq'
  `);
    for (let i = 0; i < slowlyCountRows(globalCounters); i++) {
      const name = globalCounters.columns[0].stringValues![i];
      const trackId = +globalCounters.columns[1].longValues![i];
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: COUNTER_TRACK_KIND,
        name,
        trackGroup: SCROLLING_TRACK_GROUP,
        config: {
          name,
          trackId,
        }
      });
    }
  }

  async addLogsTrack(): Promise<void> {
    const logCount =
        await this.engine.query(`select count(1) from android_logs`);
    if (logCount.columns[0].longValues![0] > 0) {
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: ANDROID_LOGS_TRACK_KIND,
        name: 'Android logs',
        trackGroup: SCROLLING_TRACK_GROUP,
        config: {}
      });
    }
  }

  async addAnnotationTracks(): Promise<void> {
    const annotationSliceRows = await this.engine.query(`
    SELECT id, name, upid FROM annotation_slice_track`);
    for (let i = 0; i < slowlyCountRows(annotationSliceRows); i++) {
      const id = annotationSliceRows.columns[0].longValues![i];
      const name = annotationSliceRows.columns[1].stringValues![i];
      const upid = annotationSliceRows.columns[2].longValues![i];
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: SLICE_TRACK_KIND,
        name,
        trackGroup: upid === 0 ? SCROLLING_TRACK_GROUP :
                                 this.upidToUuid.get(upid),
        config: {
          maxDepth: 0,
          namespace: 'annotation',
          trackId: id,
        },
      });
    }

    const annotationCounterRows = await this.engine.query(`
    SELECT id, name, upid, min_value, max_value
    FROM annotation_counter_track`);
    for (let i = 0; i < slowlyCountRows(annotationCounterRows); i++) {
      const id = annotationCounterRows.columns[0].longValues![i];
      const name = annotationCounterRows.columns[1].stringValues![i];
      const upid = annotationCounterRows.columns[2].longValues![i];
      const minimumValue = annotationCounterRows.columns[3].isNulls![i] ?
          undefined :
          annotationCounterRows.columns[3].doubleValues![i];
      const maximumValue = annotationCounterRows.columns[4].isNulls![i] ?
          undefined :
          annotationCounterRows.columns[4].doubleValues![i];
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: 'CounterTrack',
        name,
        trackGroup: upid === 0 ? SCROLLING_TRACK_GROUP :
                                 this.upidToUuid.get(upid),
        config: {
          name,
          namespace: 'annotation',
          trackId: id,
          minimumValue,
          maximumValue,
        }
      });
    }
  }

  async addThreadStateTracks(): Promise<void> {
    const query = await this.engine.query(`
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

    const it = iter(
        {
          utid: NUM,
          upid: NUM_NULL,
          tid: NUM_NULL,
          pid: NUM_NULL,
          threadName: STR_NULL,
        },
        query);
    for (let i = 0; it.valid(); ++i, it.next()) {
      const row = it.row;
      const utid = row.utid;
      const tid = row.tid;
      const upid = row.upid;
      const pid = row.pid;
      const threadName = row.threadName;
      const isMainThread = tid === pid;
      const uuid = this.getUuid(utid, upid);
      const kind = THREAD_STATE_TRACK_KIND;
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name: TrackDecider.getTrackName({utid, tid, threadName, kind}),
        trackGroup: uuid,
        isMainThread,
        config: {utid}
      });
    }
  }

  async addThreadCpuSampleTracks(): Promise<void> {
    const query = await this.engine.query(`
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

    const it = iter(
        {
          utid: NUM,
          upid: NUM_NULL,
          tid: NUM_NULL,
          threadName: STR_NULL,
        },
        query);
    for (let i = 0; it.valid(); ++i, it.next()) {
      const row = it.row;
      const utid = row.utid;
      const upid = row.upid;
      const threadName = row.threadName;
      const uuid = this.getUuid(utid, upid);
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: CPU_PROFILE_TRACK_KIND,
        // TODO(hjd): The threadName can be null, use  instead.
        name: `${threadName} (CPU Stack Samples)`,
        trackGroup: uuid,
        config: {utid},
      });
    }
  }

  async addThreadCounterTracks(): Promise<void> {
    const query = await this.engine.query(`
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
    where thread_counter_track.name not in ('time_in_state', 'thread_time')
  `);

    const it = iter(
        {
          trackName: STR_NULL,
          utid: NUM,
          upid: NUM_NULL,
          tid: NUM_NULL,
          threadName: STR_NULL,
          startTs: NUM_NULL,
          trackId: NUM,
          endTs: NUM_NULL,
        },
        query);
    for (let i = 0; it.valid(); ++i, it.next()) {
      const row = it.row;
      const utid = row.utid;
      const tid = row.tid;
      const upid = row.upid;
      const trackId = row.trackId;
      const trackName = row.trackName;
      const threadName = row.threadName;
      const uuid = this.getUuid(utid, upid);
      const startTs = row.startTs === null ? undefined : row.startTs;
      const endTs = row.endTs === null ? undefined : row.endTs;
      const kind = COUNTER_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, utid, tid, kind, threadName, threadTrack: true});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackGroup: uuid,
        config: {
          name,
          trackId,
          startTs,
          endTs,
        }
      });
    }
  }

  async addProcessAsyncSliceTracks(): Promise<void> {
    const query = await this.engine.query(`
        select
          process_track.upid as upid,
          process_track.name as trackName,
          group_concat(process_track.id) as trackIds,
          process.name as processName,
          process.pid as pid
        from process_track
        left join process using(upid)
        where process_track.name not like "% Timeline"
        group by
          process_track.upid,
          process_track.name
  `);

    const it = iter(
        {
          upid: NUM,
          trackName: STR_NULL,
          trackIds: STR,
          processName: STR_NULL,
          pid: NUM_NULL,
        },
        query);
    for (let i = 0; it.valid(); ++i, it.next()) {
      const row = it.row;
      const upid = row.upid;
      const trackName = row.trackName;
      const rawTrackIds = row.trackIds;
      const trackIds = rawTrackIds.split(',').map(v => Number(v));
      const processName = row.processName;
      const pid = row.pid;

      const uuid = this.getUuid(0, upid);

      // TODO(hjd): 1+N queries are bad in the track_decider
      const depthResult = await this.engine.query(`
      SELECT MAX(layout_depth) as max_depth
      FROM experimental_slice_layout('${rawTrackIds}');
    `);
      const maxDepth = +depthResult.columns[0].longValues![0];

      const kind = ASYNC_SLICE_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, upid, pid, processName, kind});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackGroup: uuid,
        config: {
          trackIds,
          maxDepth,
        }
      });
    }
  }

  async addActualFramesTracks(): Promise<void> {
    const query = await this.engine.query(`
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

    const it = iter(
        {
          upid: NUM,
          trackName: STR_NULL,
          trackIds: STR,
          processName: STR_NULL,
          pid: NUM_NULL,
        },
        query);
    for (let i = 0; it.valid(); ++i, it.next()) {
      const row = it.row;
      const upid = row.upid;
      const trackName = row.trackName;
      const rawTrackIds = row.trackIds;
      const trackIds = rawTrackIds.split(',').map(v => Number(v));
      const processName = row.processName;
      const pid = row.pid;

      const uuid = this.getUuid(0, upid);

      // TODO(hjd): 1+N queries are bad in the track_decider
      const depthResult = await this.engine.query(`
      SELECT MAX(layout_depth) as max_depth
      FROM experimental_slice_layout('${rawTrackIds}');
    `);
      const maxDepth = +depthResult.columns[0].longValues![0];

      const kind = ACTUAL_FRAMES_SLICE_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, upid, pid, processName, kind});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackGroup: uuid,
        config: {
          trackIds,
          maxDepth,
        }
      });
    }
  }

  async addExpectedFramesTracks(): Promise<void> {
    const query = await this.engine.query(`
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

    const it = iter(
        {
          upid: NUM,
          trackName: STR_NULL,
          trackIds: STR,
          processName: STR_NULL,
          pid: NUM_NULL,
        },
        query);
    for (let i = 0; it.valid(); ++i, it.next()) {
      const row = it.row;
      const upid = row.upid;
      const trackName = row.trackName;
      const rawTrackIds = row.trackIds;
      const trackIds = rawTrackIds.split(',').map(v => Number(v));
      const processName = row.processName;
      const pid = row.pid;

      const uuid = this.getUuid(0, upid);

      // TODO(hjd): 1+N queries are bad in the track_decider
      const depthResult = await this.engine.query(`
      SELECT MAX(layout_depth) as max_depth
      FROM experimental_slice_layout('${rawTrackIds}');
    `);
      const maxDepth = +depthResult.columns[0].longValues![0];

      const kind = EXPECTED_FRAMES_SLICE_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, upid, pid, processName, kind});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackGroup: uuid,
        config: {
          trackIds,
          maxDepth,
        }
      });
    }
  }

  async addThreadSliceTracks(): Promise<void> {
    const query = await this.engine.query(`
        select
          thread_track.utid as utid,
          thread_track.id as trackId,
          thread_track.name as trackName,
          tid,
          thread.name as threadName,
          max(depth) as maxDepth,
          process.upid as upid,
          process.pid as pid
        from slice
        join thread_track on slice.track_id = thread_track.id
        join thread using(utid)
        left join process using(upid)
        group by thread_track.id
  `);

    const it = iter(
        {
          utid: NUM,
          trackId: NUM,
          trackName: STR_NULL,
          tid: NUM_NULL,
          threadName: STR_NULL,
          maxDepth: NUM,
          upid: NUM_NULL,
          pid: NUM_NULL,
        },
        query);
    for (let i = 0; it.valid(); ++i, it.next()) {
      const row = it.row;
      const utid = row.utid;
      const trackId = row.trackId;
      const trackName = row.trackName;
      const tid = row.tid;
      const threadName = row.threadName;
      const upid = row.upid;
      const pid = row.pid;
      const maxDepth = row.maxDepth;
      const isMainThread = tid === pid;

      const uuid = this.getUuid(utid, upid);

      const kind = SLICE_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, utid, tid, threadName, kind});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackGroup: uuid,
        isMainThread,
        config: {
          trackId,
          maxDepth,
        }
      });
    }
  }

  async addProcessCounterTracks(): Promise<void> {
    const query = await this.engine.query(`
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
    const it = iter(
        {
          trackId: NUM,
          trackName: STR_NULL,
          upid: NUM,
          pid: NUM_NULL,
          processName: STR_NULL,
          startTs: NUM_NULL,
          endTs: NUM_NULL,
        },
        query);
    for (let i = 0; it.valid(); ++i, it.next()) {
      const row = it.row;
      const pid = row.pid;
      const upid = row.upid;
      const trackId = row.trackId;
      const trackName = row.trackName;
      const processName = row.processName;
      const uuid = this.getUuid(0, upid);
      const startTs = row.startTs === null ? undefined : row.startTs;
      const endTs = row.endTs === null ? undefined : row.endTs;
      const kind = COUNTER_TRACK_KIND;
      const name = TrackDecider.getTrackName(
          {name: trackName, upid, pid, kind, processName});
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind,
        name,
        trackGroup: uuid,
        config: {
          name,
          trackId,
          startTs,
          endTs,
        }
      });
    }
  }

  async addProcessHeapProfileTracks(): Promise<void> {
    const query = await this.engine.query(`
    select distinct(upid) from heap_profile_allocation
    union
    select distinct(upid) from heap_graph_object
  `);
    const it = iter({upid: NUM}, query);
    for (let i = 0; it.valid(); ++i, it.next()) {
      const upid = it.row.upid;
      const uuid = this.getUuid(0, upid);
      this.tracksToAdd.push({
        engineId: this.engineId,
        kind: HEAP_PROFILE_TRACK_KIND,
        name: `Heap Profile`,
        trackGroup: uuid,
        config: {upid}
      });
    }
  }

  getUuid = (utid: number, upid: number|null) => {
    let uuid =
        upid === null ? this.utidToUuid.get(utid) : this.upidToUuid.get(upid);
    if (uuid === undefined) {
      uuid = uuidv4();
      if (upid === null) {
        this.utidToUuid.set(utid, uuid);
      } else {
        this.upidToUuid.set(upid, uuid);
      }
    }
    return uuid;
  };

  async addProcessTrackGroups(): Promise<void> {
    // We want to create groups of tracks in a specific order.
    // The tracks should be grouped:
    //    by upid
    //    or (if upid is null) by utid
    // the groups should be sorted by:
    //  has a heap profile or not
    //  total cpu time *for the whole parent process*
    //  upid
    //  utid
    const query = await this.engine.query(`
    select
      the_tracks.upid,
      the_tracks.utid,
      total_dur as hasSched,
      hasHeapProfiles,
      process.pid as pid,
      thread.tid as tid,
      process.name as processName,
      thread.name as threadName
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
      select upid, utid from (
        select distinct(utid) from cpu_profile_stack_sample
      ) join thread using(utid)
      union
      select distinct(upid) as upid, 0 as utid from heap_profile_allocation
      union
      select distinct(upid) as upid, 0 as utid from heap_graph_object
    ) the_tracks
    left join (select upid, sum(dur) as total_dur
      from sched join thread using(utid)
      group by upid
    ) using(upid)
    left join (select upid, sum(value) as total_cycles
      from android_thread_time_in_state_event
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
    left join thread using(utid)
    left join process using(upid)
    order by
      hasHeapProfiles,
      total_dur desc,
      total_cycles desc,
      the_tracks.upid,
      the_tracks.utid;
  `);

    const it = iter(
        {
          utid: NUM,
          upid: NUM_NULL,
          tid: NUM_NULL,
          pid: NUM_NULL,
          threadName: STR_NULL,
          processName: STR_NULL,
          hasSched: NUM_NULL,
          hasHeapProfiles: NUM_NULL,
        },
        query);
    for (let i = 0; it.valid(); ++i, it.next()) {
      const row = it.row;
      const utid = row.utid;
      const tid = row.tid;
      const upid = row.upid;
      const pid = row.pid;
      const threadName = row.threadName;
      const processName = row.processName;
      const hasSched = !!row.hasSched;
      const hasHeapProfiles = !!row.hasHeapProfiles;

      // Group by upid if present else by utid.
      let pUuid =
          upid === null ? this.utidToUuid.get(utid) : this.upidToUuid.get(upid);
      // These should only happen once for each track group.
      if (pUuid === undefined) {
        pUuid = this.getUuid(utid, upid);
        const summaryTrackId = uuidv4();

        const pidForColor = pid || tid || upid || utid || 0;
        const kind =
            hasSched ? PROCESS_SCHEDULING_TRACK_KIND : PROCESS_SUMMARY_TRACK;

        this.tracksToAdd.push({
          id: summaryTrackId,
          engineId: this.engineId,
          kind,
          name: `${upid === null ? tid : pid} summary`,
          config: {pidForColor, upid, utid},
        });

        const name = TrackDecider.getTrackName(
            {utid, processName, pid, threadName, tid, upid});
        const addTrackGroup = Actions.addTrackGroup({
          engineId: this.engineId,
          summaryTrackId,
          name,
          id: pUuid,
          collapsed: !hasHeapProfiles,
        });

        this.addTrackGroupActions.push(addTrackGroup);
      }
    }
  }

  async decideTracks(): Promise<DeferredAction[]> {
    await this.addCpuSchedulingTracks();
    await this.addCpuFreqTracks();
    await this.addGlobalAsyncTracks();
    await this.addGpuFreqTracks();
    await this.addGlobalCounterTracks();

    // Will populate this.addTrackGroupActions().
    await this.addProcessTrackGroups();

    await this.addProcessHeapProfileTracks();
    await this.addProcessCounterTracks();
    await this.addProcessAsyncSliceTracks();
    await this.addActualFramesTracks();
    await this.addExpectedFramesTracks();
    await this.addThreadCounterTracks();
    await this.addThreadStateTracks();
    await this.addThreadSliceTracks();
    await this.addThreadCpuSampleTracks();
    await this.addLogsTrack();
    await this.addAnnotationTracks();

    this.addTrackGroupActions.push(
        Actions.addTracks({tracks: this.tracksToAdd}));
    return this.addTrackGroupActions;
  }
}

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

type GetUuid = (utid: number, upid: number|null) => string;

function getTrackName(args: Partial<{
  name: string | null,
  utid: number,
  processName: string | null,
  pid: number | null,
  threadName: string | null,
  tid: number | null,
  upid: number | null,
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

async function getCpuSchedulingTracks(
    engineId: string, engine: Engine): Promise<AddTrackArgs[]> {
  const tracks: AddTrackArgs[] = [];
  const cpus = await engine.getCpus();
  for (const cpu of cpus) {
    tracks.push({
      engineId,
      kind: CPU_SLICE_TRACK_KIND,
      name: `Cpu ${cpu}`,
      trackGroup: SCROLLING_TRACK_GROUP,
      config: {
        cpu,
      }
    });
  }
  return tracks;
}

async function getCpuFreqTracks(
    engineId: string, engine: Engine): Promise<AddTrackArgs[]> {
  const tracks: AddTrackArgs[] = [];
  const cpus = await engine.getCpus();

  const maxCpuFreq = await engine.query(`
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
    const cpuFreqIdle = await engine.query(`
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
      const idleTrackId =
          idleTrackExists ? +cpuFreqIdle.columns[1].longValues![0] : undefined;

      tracks.push({
        engineId,
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
  return tracks;
}

async function getGlobalAsyncTracks(
    engineId: string, engine: Engine): Promise<AddTrackArgs[]> {
  const tracks: AddTrackArgs[] = [];
  const rawGlobalAsyncTracks = await engine.query(`
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
      engineId,
      kind: ASYNC_SLICE_TRACK_KIND,
      trackGroup: SCROLLING_TRACK_GROUP,
      name,
      config: {
        maxDepth,
        trackIds,
      },
    };
    tracks.push(track);
  }
  return tracks;
}

async function getGpuFreqTracks(
    engineId: string, engine: Engine): Promise<AddTrackArgs[]> {
  const tracks: AddTrackArgs[] = [];
  const numGpus = await engine.getNumberOfGpus();
  const maxGpuFreq = await engine.query(`
    select max(value)
    from counter c
    inner join gpu_counter_track t on c.track_id = t.id
    where name = 'gpufreq';
  `);

  for (let gpu = 0; gpu < numGpus; gpu++) {
    // Only add a gpu freq track if we have
    // gpu freq data.
    const freqExists = await engine.query(`
      select id
      from gpu_counter_track
      where name = 'gpufreq' and gpu_id = ${gpu}
      limit 1;
    `);
    if (slowlyCountRows(freqExists) > 0) {
      tracks.push({
        engineId,
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
  return tracks;
}

async function getGlobalCounterTracks(
    engineId: string, engine: Engine): Promise<AddTrackArgs[]> {
  const tracks: AddTrackArgs[] = [];
  // Add global or GPU counter tracks that are not bound to any pid/tid.
  const globalCounters = await engine.query(`
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
    tracks.push({
      engineId,
      kind: COUNTER_TRACK_KIND,
      name,
      trackGroup: SCROLLING_TRACK_GROUP,
      config: {
        name,
        trackId,
      }
    });
  }
  return tracks;
}

async function getLogsTrack(
    engineId: string, engine: Engine): Promise<AddTrackArgs[]> {
  const logCount = await engine.query(`select count(1) from android_logs`);
  if (logCount.columns[0].longValues![0] > 0) {
    return [{
      engineId,
      kind: ANDROID_LOGS_TRACK_KIND,
      name: 'Android logs',
      trackGroup: SCROLLING_TRACK_GROUP,
      config: {}
    }];
  }
  return [];
}

async function getAnnotationTracks(
    engineId: string, engine: Engine, upidToUuid: Map<number, string>):
    Promise<AddTrackArgs[]> {
  const tracks: AddTrackArgs[] = [];
  const annotationSliceRows = await engine.query(`
    SELECT id, name, upid FROM annotation_slice_track`);
  for (let i = 0; i < slowlyCountRows(annotationSliceRows); i++) {
    const id = annotationSliceRows.columns[0].longValues![i];
    const name = annotationSliceRows.columns[1].stringValues![i];
    const upid = annotationSliceRows.columns[2].longValues![i];
    tracks.push({
      engineId,
      kind: SLICE_TRACK_KIND,
      name,
      trackGroup: upid === 0 ? SCROLLING_TRACK_GROUP : upidToUuid.get(upid),
      config: {
        maxDepth: 0,
        namespace: 'annotation',
        trackId: id,
      },
    });
  }

  const annotationCounterRows = await engine.query(`
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
    tracks.push({
      engineId,
      kind: 'CounterTrack',
      name,
      trackGroup: upid === 0 ? SCROLLING_TRACK_GROUP : upidToUuid.get(upid),
      config: {
        name,
        namespace: 'annotation',
        trackId: id,
        minimumValue,
        maximumValue,
      }
    });
  }

  return tracks;
}

async function getThreadStateTracks(
    engineId: string, engine: Engine, getUuid: GetUuid) {
  const tracks: AddTrackArgs[] = [];
  const query = await engine.query(`
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
    const uuid = getUuid(utid, upid);
    const kind = THREAD_STATE_TRACK_KIND;
    tracks.push({
      engineId,
      kind,
      name: getTrackName({utid, tid, threadName, kind}),
      trackGroup: uuid,
      isMainThread,
      config: {utid}
    });
  }
  return tracks;
}

async function getThreadCpuSampleTracks(
    engineId: string, engine: Engine, getUuid: GetUuid) {
  const tracks: AddTrackArgs[] = [];
  const query = await engine.query(`
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
    const uuid = getUuid(utid, upid);
    tracks.push({
      engineId,
      kind: CPU_PROFILE_TRACK_KIND,
      // TODO(hjd): The threadName can be null, use getTrackName instead.
      name: `${threadName} (CPU Stack Samples)`,
      trackGroup: uuid,
      config: {utid},
    });
  }
  return tracks;
}

async function getThreadCounterTracks(
    engineId: string, engine: Engine, getUuid: GetUuid) {
  const tracks: AddTrackArgs[] = [];
  const query = await engine.query(`
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
    const uuid = getUuid(utid, upid);
    const startTs = row.startTs === null ? undefined : row.startTs;
    const endTs = row.endTs === null ? undefined : row.endTs;
    const kind = COUNTER_TRACK_KIND;
    const name = getTrackName(
        {name: trackName, utid, tid, kind, threadName, threadTrack: true});
    tracks.push({
      engineId,
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
  return tracks;
}

async function getProcessAsyncSliceTracks(
    engineId: string, engine: Engine, getUuid: GetUuid) {
  const tracks: AddTrackArgs[] = [];
  const query = await engine.query(`
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

    const uuid = getUuid(0, upid);

    // TODO(hjd): 1+N queries are bad in the track_decider
    const depthResult = await engine.query(`
      SELECT MAX(layout_depth) as max_depth
      FROM experimental_slice_layout('${rawTrackIds}');
    `);
    const maxDepth = +depthResult.columns[0].longValues![0];

    const kind = ASYNC_SLICE_TRACK_KIND;
    const name = getTrackName({name: trackName, upid, pid, processName, kind});
    tracks.push({
      engineId,
      kind,
      name,
      trackGroup: uuid,
      config: {
        trackIds,
        maxDepth,
      }
    });
  }
  return tracks;
}

async function getActualFramesTracks(
    engineId: string, engine: Engine, getUuid: GetUuid) {
  const tracks: AddTrackArgs[] = [];
  const query = await engine.query(`
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

    const uuid = getUuid(0, upid);

    // TODO(hjd): 1+N queries are bad in the track_decider
    const depthResult = await engine.query(`
      SELECT MAX(layout_depth) as max_depth
      FROM experimental_slice_layout('${rawTrackIds}');
    `);
    const maxDepth = +depthResult.columns[0].longValues![0];

    const kind = ACTUAL_FRAMES_SLICE_TRACK_KIND;
    const name = getTrackName({name: trackName, upid, pid, processName, kind});
    tracks.push({
      engineId,
      kind,
      name,
      trackGroup: uuid,
      config: {
        trackIds,
        maxDepth,
      }
    });
  }
  return tracks;
}

async function getExpectedFramesTracks(
    engineId: string, engine: Engine, getUuid: GetUuid) {
  const tracks: AddTrackArgs[] = [];
  const query = await engine.query(`
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

    const uuid = getUuid(0, upid);

    // TODO(hjd): 1+N queries are bad in the track_decider
    const depthResult = await engine.query(`
      SELECT MAX(layout_depth) as max_depth
      FROM experimental_slice_layout('${rawTrackIds}');
    `);
    const maxDepth = +depthResult.columns[0].longValues![0];

    const kind = EXPECTED_FRAMES_SLICE_TRACK_KIND;
    const name = getTrackName({name: trackName, upid, pid, processName, kind});
    tracks.push({
      engineId,
      kind,
      name,
      trackGroup: uuid,
      config: {
        trackIds,
        maxDepth,
      }
    });
  }
  return tracks;
}

async function getThreadSliceTracks(
    engineId: string, engine: Engine, getUuid: GetUuid) {
  const tracks: AddTrackArgs[] = [];
  const query = await engine.query(`
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

    const uuid = getUuid(utid, upid);

    const kind = SLICE_TRACK_KIND;
    const name = getTrackName({name: trackName, utid, tid, threadName, kind});
    tracks.push({
      engineId,
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
  return tracks;
}

async function getProcessCounterTracks(
    engineId: string, engine: Engine, getUuid: GetUuid) {
  const tracks: AddTrackArgs[] = [];
  const query = await engine.query(`
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
    const uuid = getUuid(0, upid);
    const startTs = row.startTs === null ? undefined : row.startTs;
    const endTs = row.endTs === null ? undefined : row.endTs;
    const kind = COUNTER_TRACK_KIND;
    const name = getTrackName({name: trackName, upid, pid, kind, processName});
    tracks.push({
      engineId,
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
  return tracks;
}

async function getProcessHeapProfileTracks(
    engineId: string, engine: Engine, getUuid: GetUuid) {
  const tracks: AddTrackArgs[] = [];
  const query = await engine.query(`
    select distinct(upid) from heap_profile_allocation
    union
    select distinct(upid) from heap_graph_object
  `);
  const it = iter({upid: NUM}, query);
  for (let i = 0; it.valid(); ++i, it.next()) {
    const upid = it.row.upid;
    const uuid = getUuid(0, upid);
    tracks.push({
      engineId,
      kind: HEAP_PROFILE_TRACK_KIND,
      name: `Heap Profile`,
      trackGroup: uuid,
      config: {upid}
    });
  }
  return tracks;
}

function extend<T>(arr: T[], additional: T[]) {
  const offset = arr.length;
  arr.length += additional.length;
  for (let i = 0; i < additional.length; ++i) {
    arr[offset + i] = additional[i];
  }
}

export async function decideTracks(
    engineId: string, engine: Engine): Promise<DeferredAction[]> {
  const tracksToAdd: AddTrackArgs[] = [];

  extend(tracksToAdd, await getCpuSchedulingTracks(engineId, engine));
  extend(tracksToAdd, await getCpuFreqTracks(engineId, engine));
  extend(tracksToAdd, await getGlobalAsyncTracks(engineId, engine));
  extend(tracksToAdd, await getGpuFreqTracks(engineId, engine));
  extend(tracksToAdd, await getGlobalCounterTracks(engineId, engine));

  const upidToUuid = new Map<number, string>();
  const utidToUuid = new Map<number, string>();
  const getUuid = (utid: number, upid: number|null) => {
    let uuid = upid === null ? utidToUuid.get(utid) : upidToUuid.get(upid);
    if (uuid === undefined) {
      uuid = uuidv4();
      if (upid === null) {
        utidToUuid.set(utid, uuid);
      } else {
        upidToUuid.set(upid, uuid);
      }
    }
    return uuid;
  };


  // We want to create groups of tracks in a specific order.
  // The tracks should be grouped:
  //    by upid
  //    or (if upid is null) by utid
  // the groups should be sorted by:
  //  has a heap profile or not
  //  total cpu time *for the whole parent process*
  //  upid
  //  utid
  const query = await engine.query(`
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

  const addTrackGroupActions: DeferredAction[] = [];

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
    let pUuid = upid === null ? utidToUuid.get(utid) : upidToUuid.get(upid);
    // These should only happen once for each track group.
    if (pUuid === undefined) {
      pUuid = getUuid(utid, upid);
      const summaryTrackId = uuidv4();

      const pidForColor = pid || tid || upid || utid || 0;
      const kind =
          hasSched ? PROCESS_SCHEDULING_TRACK_KIND : PROCESS_SUMMARY_TRACK;

      tracksToAdd.push({
        id: summaryTrackId,
        engineId,
        kind,
        name: `${upid === null ? tid : pid} summary`,
        config: {pidForColor, upid, utid},
      });

      const name =
          getTrackName({utid, processName, pid, threadName, tid, upid});
      const addTrackGroup = Actions.addTrackGroup({
        engineId,
        summaryTrackId,
        name,
        id: pUuid,
        collapsed: !hasHeapProfiles,
      });

      addTrackGroupActions.push(addTrackGroup);
    }
  }

  extend(
      tracksToAdd,
      await getProcessHeapProfileTracks(engineId, engine, getUuid));
  extend(tracksToAdd, await getProcessCounterTracks(engineId, engine, getUuid));
  extend(
      tracksToAdd, await getProcessAsyncSliceTracks(engineId, engine, getUuid));
  extend(tracksToAdd, await getActualFramesTracks(engineId, engine, getUuid));
  extend(tracksToAdd, await getExpectedFramesTracks(engineId, engine, getUuid));
  extend(tracksToAdd, await getThreadCounterTracks(engineId, engine, getUuid));
  extend(tracksToAdd, await getThreadStateTracks(engineId, engine, getUuid));
  extend(tracksToAdd, await getThreadSliceTracks(engineId, engine, getUuid));
  extend(
      tracksToAdd, await getThreadCpuSampleTracks(engineId, engine, getUuid));
  extend(tracksToAdd, await getLogsTrack(engineId, engine));
  extend(tracksToAdd, await getAnnotationTracks(engineId, engine, upidToUuid));

  addTrackGroupActions.push(Actions.addTracks({tracks: tracksToAdd}));
  return addTrackGroupActions;
}

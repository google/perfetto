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
import {NUM, NUM_NULL, rawQueryToRows, STR_NULL} from '../common/protos';
import {SCROLLING_TRACK_GROUP} from '../common/state';
import {ANDROID_LOGS_TRACK_KIND} from '../tracks/android_log/common';
import {ASYNC_SLICE_TRACK_KIND} from '../tracks/async_slices/common';
import {SLICE_TRACK_KIND} from '../tracks/chrome_slices/common';
import {COUNTER_TRACK_KIND} from '../tracks/counter/common';
import {CPU_FREQ_TRACK_KIND} from '../tracks/cpu_freq/common';
import {CPU_PROFILE_TRACK_KIND} from '../tracks/cpu_profile/common';
import {CPU_SLICE_TRACK_KIND} from '../tracks/cpu_slices/common';
import {HEAP_PROFILE_TRACK_KIND} from '../tracks/heap_profile/common';
import {
  PROCESS_SCHEDULING_TRACK_KIND
} from '../tracks/process_scheduling/common';
import {PROCESS_SUMMARY_TRACK} from '../tracks/process_summary/common';
import {THREAD_STATE_TRACK_KIND} from '../tracks/thread_state/common';

interface ThreadSliceTrack {
  name: string;
  maxDepth: number;
  trackId: number;
}

function getTrackName(args: Partial<{
  name: string | null,
  utid: number,
  processName: string | null,
  pid: number | null,
  threadName: string | null,
  tid: number | null,
  upid: number | null,
  kind: string
}>) {
  const {name, upid, utid, processName, threadName, pid, tid, kind} = args;

  const hasName = name !== undefined && name !== null && name !== '[NULL]';
  const hasUpid = upid !== undefined && upid !== null;
  const hasUtid = utid !== undefined && utid !== null;
  const hasProcessName = processName !== undefined && processName !== null;
  const hasThreadName = threadName !== undefined && threadName !== null;
  const hasTid = tid !== undefined && tid !== null;
  const hasPid = pid !== undefined && pid !== null;
  const hasKind = kind !== undefined;

  // If we don't have any useful information (better than
  // upid/utid) we show the track kind to help with tracking
  // down where this is coming from.
  const kindSuffix = hasKind ? ` (${kind})` : '';

  if (hasName) {
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

export async function decideTracks(
    engineId: string, engine: Engine): Promise<DeferredAction[]> {
  const numGpus = await engine.getNumberOfGpus();
  const tracksToAdd: AddTrackArgs[] = [];

  const maxCpuFreq = await engine.query(`
    select max(value)
    from counter c
    inner join cpu_counter_track t on c.track_id = t.id
    where name = 'cpufreq';
  `);

  const cpus = await engine.getCpus();

  for (const cpu of cpus) {
    tracksToAdd.push({
      engineId,
      kind: CPU_SLICE_TRACK_KIND,
      name: `Cpu ${cpu}`,
      trackGroup: SCROLLING_TRACK_GROUP,
      config: {
        cpu,
      }
    });
  }

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
    if (cpuFreqIdle.numRecords > 0) {
      const freqTrackId = +cpuFreqIdle.columns[0].longValues![0];

      const idleTrackExists: boolean = !cpuFreqIdle.columns[1].isNulls![0];
      const idleTrackId =
          idleTrackExists ? +cpuFreqIdle.columns[1].longValues![0] : undefined;

      tracksToAdd.push({
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
  for (let i = 0; i < rawGlobalAsyncTracks.numRecords; i++) {
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
    tracksToAdd.push(track);
  }

  const upidToProcessTracks = new Map();
  const rawProcessTracks = await engine.query(`
    SELECT upid, name, GROUP_CONCAT(process_track.id) AS track_ids
    FROM process_track
    GROUP BY upid, name
  `);
  for (let i = 0; i < rawProcessTracks.numRecords; i++) {
    const upid = +rawProcessTracks.columns[0].longValues![i];
    const name = rawProcessTracks.columns[1].stringValues![i];
    const rawTrackIds = rawProcessTracks.columns[2].stringValues![i];
    const trackIds = rawTrackIds.split(',').map(v => Number(v));

    const depthResult = await engine.query(`
      SELECT MAX(layout_depth) as max_depth
      FROM experimental_slice_layout('${rawTrackIds}');
    `);
    const maxDepth = +depthResult.columns[0].longValues![0];
    const kind = ASYNC_SLICE_TRACK_KIND;
    const track = {
      engineId,
      kind,
      name,
      config: {
        maxDepth,
        trackIds,
      },
    };
    const tracks = upidToProcessTracks.get(upid);
    if (tracks) {
      tracks.push(track);
    } else {
      upidToProcessTracks.set(upid, [track]);
    }
  }

  const heapProfiles = await engine.query(`
    select distinct(upid) from heap_profile_allocation
    union
    select distinct(upid) from heap_graph_object`);

  const heapUpids: Set<number> = new Set();
  for (let i = 0; i < heapProfiles.numRecords; i++) {
    const upid = heapProfiles.columns[0].longValues![i];
    heapUpids.add(+upid);
  }

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
    if (freqExists.numRecords > 0) {
      tracksToAdd.push({
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
  for (let i = 0; i < globalCounters.numRecords; i++) {
    const name = globalCounters.columns[0].stringValues![i];
    const trackId = +globalCounters.columns[1].longValues![i];
    tracksToAdd.push({
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

  interface CounterTrack {
    name: string;
    trackId: number;
    startTs?: number;
    endTs?: number;
  }

  const counterUtids = new Map<number, CounterTrack[]>();
  const threadCounters = await engine.query(`
    select thread_counter_track.name, utid, thread_counter_track.id,
    start_ts, end_ts from thread_counter_track join thread using(utid)
    where thread_counter_track.name not in ('time_in_state')
  `);
  for (let i = 0; i < threadCounters.numRecords; i++) {
    const name = threadCounters.columns[0].stringValues![i];
    const utid = +threadCounters.columns[1].longValues![i];
    const trackId = +threadCounters.columns[2].longValues![i];
    let startTs = undefined;
    let endTs = undefined;
    if (!threadCounters.columns[3].isNulls![i]) {
      startTs = +threadCounters.columns[3].longValues![i] / 1e9;
    }
    if (!threadCounters.columns[4].isNulls![i]) {
      endTs = +threadCounters.columns[4].longValues![i] / 1e9;
    }

    const track: CounterTrack = {name, trackId, startTs, endTs};
    const el = counterUtids.get(utid);
    if (el === undefined) {
      counterUtids.set(utid, [track]);
    } else {
      el.push(track);
    }
  }

  const counterUpids = new Map<number, CounterTrack[]>();
  const processCounters = await engine.query(`
    select process_counter_track.name, upid, process_counter_track.id,
    start_ts, end_ts from process_counter_track join process using(upid)
  `);
  for (let i = 0; i < processCounters.numRecords; i++) {
    const name = processCounters.columns[0].stringValues![i];
    const upid = +processCounters.columns[1].longValues![i];
    const trackId = +processCounters.columns[2].longValues![i];
    let startTs = undefined;
    let endTs = undefined;
    if (!processCounters.columns[3].isNulls![i]) {
      startTs = +processCounters.columns[3].longValues![i] / 1e9;
    }
    if (!processCounters.columns[4].isNulls![i]) {
      endTs = +processCounters.columns[4].longValues![i] / 1e9;
    }

    const track: CounterTrack = {name, trackId, startTs, endTs};
    const el = counterUpids.get(upid);
    if (el === undefined) {
      counterUpids.set(upid, [track]);
    } else {
      el.push(track);
    }
  }

  // Local experiments shows getting maxDepth separately is ~2x faster than
  // joining with threads and processes.
  const maxDepthQuery = await engine.query(`
        select
          thread_track.utid,
          thread_track.id,
          thread_track.name,
          max(depth) as maxDepth
        from slice
        inner join thread_track on slice.track_id = thread_track.id
        group by thread_track.id
      `);

  const utidToThreadTrack = new Map<number, ThreadSliceTrack[]>();
  for (let i = 0; i < maxDepthQuery.numRecords; i++) {
    const utid = maxDepthQuery.columns[0].longValues![i];
    const trackId = maxDepthQuery.columns[1].longValues![i];
    const name = maxDepthQuery.columns[2].stringValues![i];
    const maxDepth = maxDepthQuery.columns[3].longValues![i];
    const tracks = utidToThreadTrack.get(utid);
    const track = {maxDepth, trackId, name};
    if (tracks) {
      tracks.push(track);
    } else {
      utidToThreadTrack.set(utid, [track]);
    }
  }

  // For backwards compatability with older TP versions where
  // android_thread_time_in_state_event table does not exists.
  // TODO: remove once the track mega-query is improved.
  const exists =
      await engine.query(`select name from sqlite_master where type='table' and
       name='android_thread_time_in_state_event'`);
  if (exists.numRecords === 0) {
    await engine.query(`create view android_thread_time_in_state_event as
        select null as upid, null as value where 0`);
  }

  // Return all threads
  // sorted by:
  //  total cpu time *for the whole parent process*
  //  upid
  //  utid
  const threadQuery = await engine.query(`
      select
        utid,
        tid,
        upid,
        pid,
        thread.name as threadName,
        process.name as processName,
        total_dur as totalDur,
        ifnull(has_sched, false) as hasSched,
        ifnull(has_cpu_samples, false) as hasCpuSamples
      from
        thread
        left join (select utid, count(1), true as has_sched
            from sched group by utid
        ) using(utid)
        left join (select utid, count(1), true as has_cpu_samples
            from cpu_profile_stack_sample group by utid
        ) using(utid)
        left join process using(upid)
        left join (select upid, sum(dur) as total_dur
            from sched join thread using(utid)
            group by upid
          ) using(upid)
        left join (select upid, sum(value) as total_cycles
            from android_thread_time_in_state_event
            group by upid
          ) using(upid)
      where utid != 0
      group by utid, upid
      order by total_dur desc, total_cycles desc, upid, utid`);

  const upidToUuid = new Map<number, string>();
  const utidToUuid = new Map<number, string>();
  const addTrackGroupActions: DeferredAction[] = [];

  for (const row of rawQueryToRows(threadQuery, {
         utid: NUM,
         upid: NUM_NULL,
         tid: NUM_NULL,
         pid: NUM_NULL,
         threadName: STR_NULL,
         processName: STR_NULL,
         totalDur: NUM_NULL,
         hasSched: NUM,
         hasCpuSamples: NUM,
       })) {
    const utid = row.utid;
    const tid = row.tid;
    const upid = row.upid;
    const pid = row.pid;
    const threadName = row.threadName;
    const processName = row.processName;
    const hasSchedEvents = !!row.totalDur;
    const threadHasSched = !!row.hasSched;
    const threadHasCpuSamples = !!row.hasCpuSamples;
    const isMainThread = tid === pid;

    const threadTracks =
        utid === null ? undefined : utidToThreadTrack.get(utid);
    if (threadTracks === undefined &&
        (upid === null || counterUpids.get(upid) === undefined) &&
        counterUtids.get(utid) === undefined && !threadHasSched &&
        (upid === null || upid !== null && !heapUpids.has(upid))) {
      continue;
    }

    // Group by upid if present else by utid.
    let pUuid = upid === null ? utidToUuid.get(utid) : upidToUuid.get(upid);
    // These should only happen once for each track group.
    if (pUuid === undefined) {
      pUuid = uuidv4();
      const summaryTrackId = uuidv4();
      if (upid === null) {
        utidToUuid.set(utid, pUuid);
      } else {
        upidToUuid.set(upid, pUuid);
      }

      const pidForColor = pid || tid || upid || utid || 0;
      const kind = hasSchedEvents ? PROCESS_SCHEDULING_TRACK_KIND :
                                    PROCESS_SUMMARY_TRACK;

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
        collapsed: !(upid !== null && heapUpids.has(upid)),
      });

      // If the track group contains a heap profile, it should be before all
      // other processes.
      if (upid !== null && heapUpids.has(upid)) {
        addTrackGroupActions.unshift(addTrackGroup);
      } else {
        addTrackGroupActions.push(addTrackGroup);
      }

      if (upid !== null) {
        if (heapUpids.has(upid)) {
          tracksToAdd.push({
            engineId,
            kind: HEAP_PROFILE_TRACK_KIND,
            name: `Heap Profile`,
            trackGroup: pUuid,
            config: {upid}
          });
        }

        const counterNames = counterUpids.get(upid);
        if (counterNames !== undefined) {
          counterNames.forEach(element => {
            const kind = COUNTER_TRACK_KIND;
            const name = getTrackName({
              name: element.name,
              utid,
              processName,
              pid,
              threadName,
              tid,
              upid,
              kind
            });
            tracksToAdd.push({
              engineId,
              kind,
              name,
              trackGroup: pUuid,
              config: {
                name,
                trackId: element.trackId,
                startTs: element.startTs,
                endTs: element.endTs,
              }
            });
          });
        }

        if (upidToProcessTracks.has(upid)) {
          for (const track of upidToProcessTracks.get(upid)) {
            tracksToAdd.push(Object.assign(track, {
              name: getTrackName({
                name: track.name,
                processName,
                pid,
                upid,
                kind: track.kind,
              }),
              trackGroup: pUuid,
            }));
          }
        }
      }
    }
    const counterThreadNames = counterUtids.get(utid);
    if (counterThreadNames !== undefined) {
      const kind = COUNTER_TRACK_KIND;
      counterThreadNames.forEach(element => {
        const name =
            getTrackName({name: element.name, utid, tid, kind, threadName});
        tracksToAdd.push({
          engineId,
          kind,
          name,
          trackGroup: pUuid,
          config: {
            name,
            trackId: element.trackId,
            startTs: element.startTs,
            endTs: element.endTs,
          }
        });
      });
    }

    if (threadHasCpuSamples) {
      tracksToAdd.push({
        engineId,
        kind: CPU_PROFILE_TRACK_KIND,
        name: `${threadName} (CPU Stack Samples)`,
        trackGroup: pUuid,
        config: {utid},
      });
    }

    if (threadHasSched) {
      const kind = THREAD_STATE_TRACK_KIND;
      tracksToAdd.push({
        engineId,
        kind,
        name: getTrackName({utid, tid, threadName, kind}),
        trackGroup: pUuid,
        isMainThread,
        config: {utid}
      });
    }

    if (threadTracks !== undefined) {
      const kind = SLICE_TRACK_KIND;
      for (const config of threadTracks) {
        tracksToAdd.push({
          engineId,
          kind,
          name: getTrackName({name: config.name, utid, tid, threadName, kind}),
          trackGroup: pUuid,
          isMainThread,
          config: {maxDepth: config.maxDepth, trackId: config.trackId},
        });
      }
    }
  }

  const logCount = await engine.query(`select count(1) from android_logs`);
  if (logCount.columns[0].longValues![0] > 0) {
    tracksToAdd.push({
      engineId,
      kind: ANDROID_LOGS_TRACK_KIND,
      name: 'Android logs',
      trackGroup: SCROLLING_TRACK_GROUP,
      config: {}
    });
  }

  const annotationSliceRows = await engine.query(`
    SELECT id, name, upid FROM annotation_slice_track`);
  for (let i = 0; i < annotationSliceRows.numRecords; i++) {
    const id = annotationSliceRows.columns[0].longValues![i];
    const name = annotationSliceRows.columns[1].stringValues![i];
    const upid = annotationSliceRows.columns[2].longValues![i];
    tracksToAdd.push({
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
  for (let i = 0; i < annotationCounterRows.numRecords; i++) {
    const id = annotationCounterRows.columns[0].longValues![i];
    const name = annotationCounterRows.columns[1].stringValues![i];
    const upid = annotationCounterRows.columns[2].longValues![i];
    const minimumValue = annotationCounterRows.columns[3].isNulls![i] ?
        undefined :
        annotationCounterRows.columns[3].doubleValues![i];
    const maximumValue = annotationCounterRows.columns[4].isNulls![i] ?
        undefined :
        annotationCounterRows.columns[4].doubleValues![i];
    tracksToAdd.push({
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

  addTrackGroupActions.push(Actions.addTracks({tracks: tracksToAdd}));
  return addTrackGroupActions;
}

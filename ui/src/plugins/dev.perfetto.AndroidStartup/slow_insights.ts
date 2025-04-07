// Copyright (C) 2025 The Android Open Source Project
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
import {STR, LONG, NUM, QueryResult} from '../../trace_processor/query_result';
import {createQuerySliceTrack} from '../../components/tracks/query_slice_track';
import {TrackNode} from '../../public/workspace';

// The container that keeps track of startups.
interface Startup {
  // The startup id.
  id: number;
  // The package name.
  package: string;
  // Time start
  ts: bigint;
  // Time end
  ts_end: bigint;
}

// The slices for slow start reasons.
interface Slice {
  // Time start
  ts: bigint;
  // Duration
  dur: bigint;
  // Sice name
  name : string;
}

// The log tag
const tag = 'SlowStartInsights';

const enum ReasonId {
  REASON_ID_UNSPECIFIED = 0,
  NO_BASELINE_OR_CLOUD_PROFILES = 1,
  RUN_FROM_APK = 2,
  UNLOCK_RUNNING = 3,
  APP_IN_DEBUGGABLE_MODE = 4,
  GC_ACTIVITY = 5,
  DEX2OAT_RUNNING = 6,
  INSTALLD_RUNNING = 7,
  MAIN_THREAD_TIME_SPENT_IN_RUNNABLE = 8,
  MAIN_THREAD_TIME_SPENT_IN_INTERRUPTIBLE_SLEEP = 9,
  MAIN_THREAD_TIME_SPENT_IN_BLOCKING_IO = 10,
  MAIN_THREAD_TIME_SPENT_IN_OPEN_DEX_FILES_FROM_OAT = 11,
  TIME_SPENT_IN_BIND_APPLICATION = 12,
  TIME_SPENT_IN_VIEW_INFLATION = 13,
  TIME_SPENT_IN_RESOURCES_MANAGER_GET_RESOURCES = 14,
  TIME_SPENT_VERIFYING_CLASSES = 15,
  POTENTIAL_CPU_CONTENTION_WITH_ANOTHER_PROCESS = 16,
  JIT_ACTIVITY = 17,
  MAIN_THREAD_LOCK_CONTENTION = 18,
  MAIN_THREAD_MONITOR_CONTENTION = 19,
  JIT_COMPILED_METHODS = 20,
  BROADCAST_DISPATCHED_COUNT = 21,
  BROADCAST_RECEIVED_COUNT = 22,
  STARTUP_RUNNING_CONCURRENT = 23,
  MAIN_THREAD_BINDER_TRANSCATIONS_BLOCKED = 24,
}

/**
 * Returns a track node that contains slow start insights
 * for the packages that started up in a trace.
 * @param trace The loaded trace.
 * @returns a track node with the slow start insights.
 * `undefined` if there are no app startups detected.
 */
export async function slowInsightsTrack(
  trace: Trace,
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  const startups: Array<Startup> = [];

  // Find app startups
  let result = await trace.engine.query(
    `SELECT startup_id AS id, package, ts, ts_end FROM android_startups;`,
    tag,
  );

  const it = result.iter({id: NUM, package: STR, ts: LONG, ts_end: LONG});
  for (; it.valid(); it.next()) {
    startups.push({
      id: it.id,
      package: it.package,
      ts: it.ts,
      ts_end: it.ts_end,
    });
  }

  if (startups.length === 0) {
    // Nothing interesting to report.
    return undefined;
  }

  const uri = '/android_startups_slow_insights';
  const title = 'Slow Startup Insights';
  const track = await createQuerySliceTrack({
    trace: trace,
    uri: uri,
    data: {
      sqlSource: `
        SELECT
          ts,
          ts_end - ts as dur,
          package as name
        FROM android_startups`,
      columns: ['ts', 'dur', 'name'],
    },
  });
  trace.tracks.registerTrack({
    uri,
    title,
    track,
  });
  const trackNode = new TrackNode({title, uri}); 
  addChildTrack(
    await getProcessDebuggableTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getBaselineProfileTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getRunFromApkTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getUnlockRunningTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getGcActivityTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getMainThreadBinderTransactionsTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getBroadcastDispatchedTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getBroadcastReceivedTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getOpenDexFilesFromOatTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getVerifyClassTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getLockContentionTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getMonitorContentionTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getBindApplicationTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getViewInflationTrack(trace, startups, reasonId, pkgName), trackNode);
  addChildTrack(
    await getResourcesManagerTrack(trace, startups, reasonId, pkgName), trackNode);
  return trackNode;
}

function addChildTrack(
  childNode: TrackNode | undefined,
  trackNode: TrackNode,
): void {
  if (!!childNode) {
    trackNode.addChildLast(childNode);
  }
}

function addResultToSlices(
  result: QueryResult,
  slices: Slice[],
): void {
  const it = result.iter({ts: LONG, dur: LONG, name: STR});
  for (; it.valid(); it.next()) {
    slices.push({
      ts: it.ts,
      dur: it.dur,
      name: it.name,
    });
  }
}

async function getUnlockRunningTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName:string,
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  const tag = 'UnlockRunning';
  let sliceId = 0;
  let selected = false;
  const curReasonId = ReasonId.UNLOCK_RUNNING;
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    SELECT slice.ts AS ts, slice.dur AS dur, slice.name AS name
    FROM slice, android_startups launches
    JOIN thread_track ON slice.track_id = thread_track.id
    JOIN thread USING(utid)
    JOIN process USING(upid)
    WHERE launches.startup_id = ${startup.id}
    AND slice.name = "KeyguardUpdateMonitor#onAuthenticationSucceeded"
    AND process.name = "com.android.systemui"
    AND slice.ts >= launches.ts
    AND (slice.ts + slice.dur) <= launches.ts_end
    LIMIT 3`,
    tag,
    );

    if (!result) {
      continue;
    }
    if (!selected && pkgName === startup.package && reasonId === curReasonId) {
      selected = true;
      sliceId = slices.length;
    }
    addResultToSlices(result, slices);
  }

  const uri = `/android_startups/unlock_running`;
  const title = `Unlock Running`;
  return await 
    getSlowReasonTrackAndSelect(trace, slices, uri, title, selected, sliceId);
}

async function getProcessDebuggableTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  const tag = 'ProcessDebuggable';
  let sliceId = 0;
  let selected = false;
  const curReasonId = ReasonId.APP_IN_DEBUGGABLE_MODE;
  for (const startup of startups) {
    const result = await trace.engine.query(
    `
    SELECT ts, ts_end - ts AS dur, package AS name
    FROM android_startups launch
    JOIN android_process_metadata p ON p.process_name = launch.package
    WHERE launch.startup_id = ${startup.id}
      AND p.debuggable
    LIMIT 1`,
    tag,
    );

    if (!result) {
      continue;
    }
    if (!selected && pkgName === startup.package && reasonId === curReasonId) {
      selected = true;
      sliceId = slices.length;
    }
    addResultToSlices(result, slices);
  }

  const uri = `/android_startups/process_duggable`;
  const title = `Debuggable Process`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, selected, sliceId);
}

async function getBaselineProfileTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  const tag = 'MissingBaseLineProfile';
  let sliceId = 0;
  let selected = false;
  const curReasonId = ReasonId.NO_BASELINE_OR_CLOUD_PROFILES;
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    SELECT slice_ts AS ts, slice_dur AS dur, slice_name AS name
    FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(${startup.id},
      "location=* status=* filter=* reason=*")
    WHERE
      -- when location is the package odex file and the reason is "install" or "install-dm",
      -- if the compilation filter is not "speed-profile", baseline/cloud profile is missing.
      SUBSTR(STR_SPLIT(slice_name, " status=", 0), LENGTH("location=") + 1)
        GLOB ("*" || $pkg_name || "*odex")
      AND (STR_SPLIT(slice_name, " reason=", 1) = "install"
      OR STR_SPLIT(slice_name, " reason=", 1) = "install-dm")
    ORDER BY slice_dur DESC
    LIMIT 1`,
    tag,
    );

    if (!result) {
      continue;
    }
    if (!selected && pkgName === startup.package && reasonId === curReasonId) {
      selected = true;
      sliceId = slices.length;
    }
    addResultToSlices(result, slices);
  }

  const uri = `/android_startups/missing_baseline_profile`;
  const title = `Missing Baseline Profile`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, selected, sliceId);
}

async function getRunFromApkTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  const tag = 'RunFromApk';
  let sliceId = 0;
  let selected = false;
  const curReasonId = ReasonId.RUN_FROM_APK;
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    SELECT slice_ts AS ts, slice_dur AS dur, slice_name AS name
    FROM android_thread_slices_for_all_startups l
    WHERE
      l.startup_id = ${startup.id} AND is_main_thread AND
      slice_name GLOB "location=* status=* filter=* reason=*" AND
      STR_SPLIT(STR_SPLIT(slice_name, " filter=", 1), " reason=", 0)
        GLOB ("*" || "run-from-apk" || "*")
    ORDER BY slice_dur DESC
    LIMIT 3`,
    tag,
    );

    if (!result) {
      continue;
    }
    if (!selected && pkgName === startup.package && reasonId === curReasonId) {
      selected = true;
      sliceId = slices.length;
    }
    addResultToSlices(result, slices);
  }

  const uri = `/android_startups/run_from_apk`;
  const title = `Run From APK`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, selected, sliceId);
}

async function getGcActivityTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  const tag = 'GcActivity';
  let sliceId = 0;
  let selected = false;
  const curReasonId = ReasonId.GC_ACTIVITY;
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    SELECT slice_ts AS ts, slice_dur as dur, slice_name AS name
    FROM android_thread_slices_for_all_startups slice
    WHERE
      slice.startup_id = ${startup.id} AND
      (
        slice_name GLOB "*semispace GC" OR
        slice_name GLOB "*mark sweep GC" OR
        slice_name GLOB "*concurrent copying GC"
      )
    ORDER BY slice_dur DESC
    LIMIT 3`,
    tag,
    );

    if (!result) {
      continue;
    }
    if (!selected && pkgName === startup.package && reasonId === curReasonId) {
      selected = true;
      sliceId = slices.length;
    }
    addResultToSlices(result, slices);
  }

  const uri = `/android_startups/gc_activity`;
  const title = `GC Activity`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, selected, sliceId);
}

async function getMainThreadBinderTransactionsTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  const tag = 'MainThreadBinderTransactions';
  let sliceId = 0;
  let selected = false;
  const curReasonId = ReasonId.MAIN_THREAD_BINDER_TRANSCATIONS_BLOCKED;
  for (const startup of startups) {
    let threshold = 2e7;
    let result = await trace.engine.query(
    `
    SELECT request.slice_ts as ts, request.slice_dur as dur,
      request.slice_name as name
    FROM (
      SELECT slice_id as id, slice_dur, is_main_thread,
        slice_ts, s.utid, slice_name
      FROM android_thread_slices_for_all_startups s
      JOIN process ON (
        EXTRACT_ARG(s.arg_set_id, "destination process") = process.pid
      )
      WHERE s.startup_id = ${startup.id} AND slice_name GLOB "binder transaction"
        AND slice_dur > ${threshold}
    ) request
    JOIN following_flow(request.id) arrow
    JOIN slice reply ON reply.id = arrow.slice_in
    JOIN thread USING (utid)
    WHERE reply.dur > ${threshold} AND request.is_main_thread
    LIMIT 1`,
    tag,
    );

    if (!result) {
      continue;
    }
    if (!selected && pkgName === startup.package && reasonId === curReasonId) {
      selected = true;
      sliceId = slices.length;
    }
    addResultToSlices(result, slices);
  }

  const uri = `/android_startups/main_thread_binder_transactions_blocked`;
  const title = `Main Thread Binder Transactions Blocked`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, selected, sliceId);
}

async function getBroadcastDispatchedTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  // The log tag
  const tag = 'Broadcast dispatched';
  const sliceGlob = `'Broadcast dispatched*'`;
  const slices: Array<Slice> = [];
  const curReasonId = ReasonId.BROADCAST_DISPATCHED_COUNT;
  const thresholdCnt = 15; 

  const sliceId =
    await getBroadcastSlices(trace, tag, sliceGlob, startups, slices,
      reasonId, pkgName, curReasonId, thresholdCnt);

  const uri = `/android_startups/broadcast_dispatched`;
  const title = `Broadcast Dispatched`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, sliceId >= 0, sliceId);
}

async function getBroadcastReceivedTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  // The log tag
  const tag = 'Broadcast received';
  const sliceGlob = `'broadcastReceiveReg*'`;
  const slices: Array<Slice> = [];
  const curReasonId = ReasonId.BROADCAST_RECEIVED_COUNT;
  const thresholdCnt = 50;

  const sliceId =
    await getBroadcastSlices(trace, tag, sliceGlob, startups, slices,
      reasonId, pkgName, curReasonId, thresholdCnt);

  const uri = `/android_startups/broadcast_received`;
  const title = `Number of Broadcast Received`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, sliceId >= 0, sliceId);
}

async function getBroadcastSlices(
  trace: Trace,
  tag: string,
  sliceGlob: string,
  startups: Startup[],
  slices: Slice[],
  reasonId: number,
  pkgName: string,
  curReasonId: number,
  thresholdCnt: number,
): Promise<number> {
  let sliceId = -1;
  let selected = false;
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    SELECT COUNT(*) as count
    FROM slice s
    JOIN thread_track t ON s.track_id = t.id
    JOIN thread USING(utid)
    JOIN (
      SELECT ts, ts_end
      FROM android_startups
      WHERE startup_id = ${startup.id}
    ) launch
    WHERE
      s.name GLOB ${sliceGlob} AND
      s.ts BETWEEN launch.ts AND launch.ts_end`,
    tag,
    );

    if (!result || result.iter({count:NUM}).count < thresholdCnt){
      continue;
    }
    if (!selected && pkgName === startup.package && reasonId === curReasonId) {
      selected = true;
      sliceId = slices.length;
    }
    slices.push({
      ts: startup.ts,
      dur: startup.ts_end - startup.ts,
      name: startup.package
    });
  }
  return sliceId;
}

async function getOpenDexFilesFromOatTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  // The log tag
  const tag = 'OpenDexFilesFromOat';
  const sliceGlob = `'OpenDexFilesFromOat*'`;
  const thresholdPct = 20;
  const curReasonId = ReasonId.MAIN_THREAD_TIME_SPENT_IN_OPEN_DEX_FILES_FROM_OAT;
  const slices: Array<Slice> = [];

  const sliceId =
    await getSlicesByName(trace, tag, sliceGlob, 0, thresholdPct, startups,
      slices, reasonId, pkgName, curReasonId, true);

  const uri = `/android_startups/open_dex_files_from_oat`;
  const title = `Open Dex Files From Oat`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, sliceId >= 0, sliceId);
}

async function getVerifyClassTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  // The log tag
  const tag = 'VerifyClass';
  const sliceGlob = `'VerifyClass*'`;
  const thresholdPct = 15;
  const curReasonId = ReasonId.TIME_SPENT_VERIFYING_CLASSES;
  const slices: Array<Slice> = [];

  const sliceId =
    await getSlicesByName(trace, tag, sliceGlob, 0, thresholdPct, startups,
      slices, reasonId, pkgName, curReasonId, false);

  const uri = `/android_startups/verify_class`;
  const title = `Verify Class`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, sliceId >= 0, sliceId);
}

async function getLockContentionTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  // The log tag
  const tag = 'LockContention';
  let sliceGlob = `'Lock contention on*'`;
  let thresholdPct = 20;
  const curReasonId = ReasonId.MAIN_THREAD_LOCK_CONTENTION;
  const slices: Array<Slice> = [];

  const sliceId =
    await getSlicesByName(trace, tag, sliceGlob, 0, thresholdPct, startups,
      slices, reasonId, pkgName, curReasonId, true);

  const uri = `/android_startups/lock_contention`;
  const title = `Lock Contention`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, sliceId >= 0, sliceId);
}

async function getMonitorContentionTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  // The log tag
  const tag = 'MonitorContention';
  const sliceGlob = `'Lock contention on a monitor*'`;
  const thresholdPct = 15;
  const curReasonId = ReasonId.MAIN_THREAD_MONITOR_CONTENTION;
  const slices: Array<Slice> = [];

  const sliceId =
    await getSlicesByName(trace, tag, sliceGlob, 0, thresholdPct, startups,
      slices, reasonId, pkgName, curReasonId, true);

  const uri = `/android_startups/monitor_contention`;
  const title = `Monitor Contention`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, sliceId >= 0, sliceId);
}

async function getBindApplicationTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  // The log tag
  const tag = 'BindApplication';
  const sliceGlob = `'bindApplication'`;
  const thresholdNs = 1250000000;
  const curReasonId = ReasonId.TIME_SPENT_IN_BIND_APPLICATION;
  const slices: Array<Slice> = [];

  const sliceId =
    await getSlicesByName(trace, tag, sliceGlob, thresholdNs, 0, startups,
      slices, reasonId, pkgName, curReasonId, false);

  const uri = `/android_startups/bind_application`;
  const title = `Bind Application`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, sliceId >= 0, sliceId);
}

async function getViewInflationTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  // The log tag
  const tag = 'ViewInflation';
  const sliceGlob = `'inflate'`;
  const thresholdNs = 450000000;
  const curReasonId = ReasonId.TIME_SPENT_IN_VIEW_INFLATION;
  const slices: Array<Slice> = [];

  const sliceId =
    await getSlicesByName(trace, tag, sliceGlob, thresholdNs, 0, startups,
      slices, reasonId, pkgName, curReasonId, false);

  const uri = `/android_startups/view_inflation`;
  const title = `View Inflation`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, sliceId >= 0, sliceId);
}

async function getResourcesManagerTrack(
  trace: Trace,
  startups: Startup[],
  reasonId: number,
  pkgName: string,
): Promise<TrackNode | undefined> {
  // The log tag
  const tag = 'getResources';
  const sliceGlob = `'ResourcesManager#getResources'`;
  const thresholdNs = 130000000;
  const curReasonId = ReasonId.TIME_SPENT_IN_RESOURCES_MANAGER_GET_RESOURCES;
  const slices: Array<Slice> = [];

  const sliceId =
    await getSlicesByName(trace, tag, sliceGlob, thresholdNs, 0, startups,
      slices, reasonId, pkgName, curReasonId, false);

  const uri = `/android_startups/get_resources`;
  const title = `Get Resources`;
  return await
    getSlowReasonTrackAndSelect(trace, slices, uri, title, sliceId >= 0, sliceId);
}

async function getSlicesByName(
  trace: Trace,
  tag: string,
  sliceGlob: string,
  thresholdNs: number,
  thresholdPct: number,
  startups: Startup[],
  slices: Slice[],
  reasonId: number,
  pkgName: string,
  curReasonId: number,
  mainThreadOnly: boolean,
): Promise<number> {
  let sliceId = -1;
  let selected = false;
  const numSlices = 3;
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    SELECT slice_ts AS ts, slice_dur AS dur, slice_name AS name, is_main_thread as isMainThread
    FROM android_thread_slices_for_all_startups l
    WHERE l.startup_id = ${startup.id}
      AND slice_name GLOB ${sliceGlob}
    ORDER BY slice_dur DESC
    `,
    tag,
    );

    let it = result.iter({ts: LONG, dur: LONG, name: STR, isMainThread: NUM});
    let sum = 0;
    for (; it.valid(); it.next()) {
        if (mainThreadOnly && it.isMainThread == 0) {
          continue;
        }
        sum += Number(it.dur);
    }
    if (thresholdPct <= 0 || sum < (startup.ts_end - startup.ts) / BigInt(100) * BigInt(thresholdPct)
      &&(thresholdNs <= 0 || sum < thresholdNs)) { 
      continue;
    }
     if (!selected && pkgName === startup.package && reasonId == curReasonId) {
      selected = true;
      sliceId = slices.length;
    }
    let count = 0;
    it = result.iter({ts: LONG, dur: LONG, name: STR, isMainThread: NUM});
    for (; it.valid() && count < numSlices; it.next()) {
      if (mainThreadOnly && it.isMainThread == 0) {
        continue;
      }
      slices.push({
        ts: it.ts,
        dur: it.dur,
        name: it.name,
      });
      count++;
    }
  }
  return sliceId;
}

async function getSlowReasonTrack(
  trace: Trace,
  slices: Slice[],
  uri: string,
  title: string,
): Promise<TrackNode | undefined> {
  if (slices.length !== 0) {
    const sqlSource = slices
    .map((slice) => {
      return `SELECT
        ${slice.ts} AS ts,
        ${slice.dur} AS dur,
        '${slice.name}' AS name
      `;
    })
    .join('UNION ALL ');

    const track = await createQuerySliceTrack({
      trace: trace,
      uri: uri,
      data: {
        sqlSource: sqlSource,
        columns: ['ts', 'dur', 'name'],
      },
    });
    trace.tracks.registerTrack({
      uri,
      title,
      track,
    });
    return new TrackNode({title, uri});
  }

  return undefined;
}

async function getSlowReasonTrackAndSelect(
  trace: Trace,
  slices: Slice[],
  uri: string,
  title: string,
  selected: boolean,
  sliceId: number,
): Promise<TrackNode | undefined> {
  const trackNode = await getSlowReasonTrack(trace, slices, uri, title);
  if (!trackNode) {
    return undefined;
  }
  if (selected) {
    trace.selection.selectTrackEvent(uri, sliceId, {scrollToSelection: true,});
  }
  return trackNode;
}



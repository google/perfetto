// Copyright (C) 2024 The Android Open Source Project
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
import {STR, LONG, NUM} from '../../trace_processor/query_result';
import {createQuerySliceTrack} from '../../components/tracks/query_slice_track';
import {TrackNode} from '../../public/workspace';

// The container that keeps track of packages that have slow start reason.
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

/**
 * Returns a track node that contains slow start status
 * for the packages that started up in a trace.
 * @param trace The loaded trace.
 * @returns a track node with the slow start status.
 * `undefined` if there are no app startups detected.
 */
export async function slowInsightsTrack(
  trace: Trace,
): Promise<TrackNode | undefined> {
  const startups: Array<Startup> = [];

  // Find app startups
  let result = await trace.engine.query(
    `
        INCLUDE PERFETTO MODULE android.startup.startups;
        SELECT startup_id AS id, package, ts, ts_end FROM android_startups;`,
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

  // Create the slow reason track and also avoid re-querying.
  const sqlSource = startups
    .map((startup) => {
      return `SELECT
        ${startup.ts} AS ts,
        ${startup.ts_end - startup.ts} AS dur,
        '${startup.package}' AS name
      `;
    })
    .join('UNION ALL ');

  const uri = '/android_startups_slow_insights';
  const title = 'Slow Startup Insights';
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
  const trackNode = new TrackNode({title, uri}); 
  const processDebuggableTrack = await getProcessDebuggableTrack(trace, startups);
  if (!!processDebuggableTrack) {
    trackNode.addChildLast(processDebuggableTrack);
  }
  const baselineProfileTrack = await getBaselineProfileTrack(trace, startups);
  if (!!baselineProfileTrack) {
    trackNode.addChildLast(baselineProfileTrack);
  }
  const runFromApkTrack = await getRunFromApkTrack(trace, startups);
  if (!!runFromApkTrack) {
    trackNode.addChildLast(runFromApkTrack);
  }
  const unlockRunningTrack = await getUnlockRunningTrack(trace, startups);
  if (!!unlockRunningTrack) {
    trackNode.addChildLast(unlockRunningTrack);
  }
  const gcActivityTrack = await getGcActivityTrack(trace, startups);
  if (!!gcActivityTrack) {
    trackNode.addChildLast(gcActivityTrack);
  }
  const mainThreadBinderTransactionsTrack =
    await getMainThreadBinderTransactionsTrack(trace, startups);
  if (!!mainThreadBinderTransactionsTrack) {
    trackNode.addChildLast(mainThreadBinderTransactionsTrack);
  }
  const broadcastDispatchedTrack =
    await getBroadcastDispatchedTrack(trace, startups);
  if (!!broadcastDispatchedTrack) {
    trackNode.addChildLast(broadcastDispatchedTrack);
  }
  const broadcastReceivedTrack =
    await getBroadcastReceivedTrack(trace, startups);
  if (!!broadcastReceivedTrack) {
    trackNode.addChildLast(broadcastReceivedTrack);
  }
  const openDexFilesFromOatTrack =
    await getOpenDexFilesFromOatTrack(trace, startups);
  if (!!openDexFilesFromOatTrack) {
    trackNode.addChildLast(openDexFilesFromOatTrack);
  }
  const verifyClassTrack =
    await getVerifyClassTrack(trace, startups);
  if (!!verifyClassTrack) {
    trackNode.addChildLast(verifyClassTrack);
  }
  const lockContentionTrack =
    await getLockContentionTrack(trace, startups);
  if (!!lockContentionTrack) {
    trackNode.addChildLast(lockContentionTrack);
  }
  const monitorContentionTrack =
    await getMonitorContentionTrack(trace, startups);
  if (!!monitorContentionTrack) {
    trackNode.addChildLast(monitorContentionTrack);
  }
  const bindApplicationTrack =
    await getBindApplicationTrack(trace, startups);
  if (!!bindApplicationTrack) {
    trackNode.addChildLast(bindApplicationTrack);
  }
  const viewInflationTrack =
    await getViewInflationTrack(trace, startups);
  if (!!viewInflationTrack) {
    trackNode.addChildLast(viewInflationTrack);
  }
  const resourcesManagerTrack =
    await getResourcesManagerTrack(trace, startups);
  if (!!resourcesManagerTrack) {
    trackNode.addChildLast(resourcesManagerTrack);
  }
  return trackNode;
}

async function getUnlockRunningTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  let tag = 'UnlockRunning';
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    INCLUDE PERFETTO MODULE android.startup.startups;
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
    const it = result.iter({ts: LONG, dur: LONG, name: STR});
    for (; it.valid(); it.next()) {
      slices.push({
        ts: it.ts,
        dur: it.dur,
        name: it.name,
      });
    }
  }

  const uri = `/android_startups/unlockrunning`;
  const title = `Unlock Running`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getProcessDebuggableTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  let tag = 'ProcessDebuggable';
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    INCLUDE PERFETTO MODULE android.startup.startups;
    SELECT RUN_METRIC('android/process_metadata.sql');
    SELECT ts, ts_end - ts AS dur, package AS name
    FROM android_startups launch
        WHERE launch.startup_id = ${startup.id}
          AND is_process_debuggable(launch.package)`,
    tag,
    );
    const it = result.iter({ts: LONG, dur: LONG, name: STR});
    for (; it.valid(); it.next()) {
      slices.push({
        ts: it.ts,
        dur: it.dur,
        name: it.name,
      });
    }
  }

  const uri = `/android_startups/processduggable`;
  const title = `Debuggable Process`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getBaselineProfileTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  let tag = 'MissingBaseLineProfile';
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    INCLUDE PERFETTO MODULE android.startup.startups;
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
    const it = result.iter({ts: LONG, dur: LONG, name: STR});
    for (; it.valid(); it.next()) {
      slices.push({
        ts: it.ts,
        dur: it.dur,
        name: it.name,
      });
    }
  }

  const uri = `/android_startups/baselineprofile`;
  const title = `Missing Baseline Profile`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getRunFromApkTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  let tag = 'RunFromApk';
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    INCLUDE PERFETTO MODULE android.startup.startups;
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
    const it = result.iter({ts: LONG, dur: LONG, name: STR});
    for (; it.valid(); it.next()) {
      slices.push({
        ts: it.ts,
        dur: it.dur,
        name: it.name,
      });
    }
  }

  const uri = `/android_startups/runfromapk`;
  const title = `Run From APK`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getGcActivityTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  let tag = 'GcActivity';
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    INCLUDE PERFETTO MODULE android.startup.startups;
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
    const it = result.iter({ts: LONG, dur: LONG, name: STR});
    for (; it.valid(); it.next()) {
      slices.push({
        ts: it.ts,
        dur: it.dur,
        name: it.name,
      });
    }
  }

  const uri = `/android_startups/gcactivity`;
  const title = `GC Activity`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getMainThreadBinderTransactionsTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  const slices: Array<Slice> = [];

  // The log tag
  let tag = 'MainThreadBinderTransactions';
  for (const startup of startups) {
    let threshold = 2e7;
    let result = await trace.engine.query(
    `
    INCLUDE PERFETTO MODULE android.startup.startups;
    SELECT request.slice_ts as ts, request.slice_dur as dur,
      request.slice_name as name
    FROM (
      SELECT tid, slice_id as id, slice_dur, thread_name, process.name as process,
        s.arg_set_id, is_main_thread,
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
    ORDER BY request.slice_dur DESC
    LIMIT 3`,
    tag,
    );
    const it = result.iter({ts: LONG, dur: LONG, name: STR});
    for (; it.valid(); it.next()) {
      slices.push({
        ts: it.ts,
        dur: it.dur,
        name: it.name,
      });
    }
  }

  const uri = `/android_startups/main_thread_binder_transactions`;
  const title = `MainThreadBinderTransactions`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getBroadcastDispatchedTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  // The log tag
  let tag = 'Broadcast dispatched';
  let slice_glob = `'Broadcast dispatched*'`;

  let slices = await getBroadcastSlices(trace, tag, slice_glob, startups);

  const uri = `/android_startups/broadcast_dispatched`;
  const title = `Broadcast Dispatched`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getBroadcastReceivedTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  // The log tag
  let tag = 'Broadcast received';
  let slice_glob = `'broadcastReceiveReg*'`;

  let slices = await getBroadcastSlices(trace, tag, slice_glob, startups);

  const uri = `/android_startups/broadcast_received`;
  const title = `Broadcast Received`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getBroadcastSlices(
  trace: Trace,
  tag: string,
  slice_glob: string,
  startups: Startup[],
): Promise<Array<Slice>> {
  const slices: Array<Slice> = [];

  // The log tag
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    INCLUDE PERFETTO MODULE android.startup.startups;
    SELECT s.ts AS ts, dur,  s.name AS name
    FROM slice s
    JOIN thread_track t ON s.track_id = t.id
    JOIN thread USING(utid)
    JOIN (
      SELECT ts, ts_end
      FROM android_startups
      WHERE startup_id = ${startup.id}
    ) launch
    WHERE
      s.name GLOB ${slice_glob} AND
      s.ts BETWEEN launch.ts AND launch.ts_end
    ORDER BY dur DESC
    LIMIT 3`,
    tag,
    );
    const it = result.iter({ts: LONG, dur: LONG, name: STR});
    for (; it.valid(); it.next()) {
      slices.push({
        ts: it.ts,
        dur: it.dur,
        name: it.name,
      });
    }
  }
  return slices;
}

async function getOpenDexFilesFromOatTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  // The log tag
  let tag = 'OpenDexFilesFromOat';
  let slice_glob = `'OpenDexFilesFromOat*'`;
  let result = await trace.engine.query(
    `
    SELECT RUN_METRIC('android/startup/slow_start_thresholds.sql');
    SELECT threshold_open_dex_files_from_oat_percentage() as threshold_pct;
    `,
    tag,
  );
  let threshold_pct = 0;
  let it = result.iter({threshold_pct: LONG});
  threshold_pct = Number(it.threshold_pct);

  let slices = await getMainThreadSlices(trace, tag, slice_glob, 0, threshold_pct, startups);

  const uri = `/android_startups/open_dex_files_from_oat`;
  const title = `Open Dex Files From Oat`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getVerifyClassTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  // The log tag
  let tag = 'VerifyClass';
  let slice_glob = `'VerifyClass*'`;
  let result = await trace.engine.query(
    `
    SELECT RUN_METRIC('android/startup/slow_start_thresholds.sql');
    SELECT threshold_verify_classes_percentage() as threshold_pct;
    `,
    tag,
  );
  let threshold_pct = 0;
  let it = result.iter({threshold_pct: LONG});
  threshold_pct = Number(it.threshold_pct);

  let slices = await getMainThreadSlices(trace, tag, slice_glob, 0, threshold_pct, startups);

  const uri = `/android_startups/verify_class`;
  const title = `Verify Class`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getLockContentionTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  // The log tag
  let tag = 'LockContention';
  let slice_glob = `'Lock contention on*'`;
  let result = await trace.engine.query(
    `
    SELECT RUN_METRIC('android/startup/slow_start_thresholds.sql');
    SELECT threshold_lock_contention_percentage() as threshold_pct;
    `,
    tag,
  );
  let threshold_pct = 0;
  let it = result.iter({threshold_pct: LONG});
  threshold_pct = Number(it.threshold_pct);

  let slices = await getMainThreadSlices(trace, tag, slice_glob, 0, threshold_pct, startups);

  const uri = `/android_startups/lock_contention`;
  const title = `Lock Contention`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getMonitorContentionTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  // The log tag
  let tag = 'MonitorContention';
  let slice_glob = `'Lock contention on a monitor*'`;
  let result = await trace.engine.query(
    `
    SELECT RUN_METRIC('android/startup/slow_start_thresholds.sql');
    SELECT threshold_monitor_contention_percentage() as threshold_pct;
    `,
    tag,
  );
  let threshold_pct = 0;
  let it = result.iter({threshold_pct: LONG});
  threshold_pct = Number(it.threshold_pct);

  let slices = await getMainThreadSlices(trace, tag, slice_glob, 0, threshold_pct, startups);

  const uri = `/android_startups/monitor_contention`;
  const title = `Monitor Contention`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getBindApplicationTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  // The log tag
  let tag = 'BindApplication';
  let slice_glob = `'bindApplication'`;
  let result = await trace.engine.query(
    `
    SELECT RUN_METRIC('android/startup/slow_start_thresholds.sql');
    SELECT threshold_bind_application_ns() as threshold_ns;
    `,
    tag,
  );
  let threshold_ns = 0;
  let it = result.iter({threshold_ns: LONG});
  threshold_ns = Number(it.threshold_ns);

  let slices = await getMainThreadSlices(trace, tag, slice_glob, threshold_ns, 0, startups);

  const uri = `/android_startups/bind_application`;
  const title = `Bind Application`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getViewInflationTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  // The log tag
  let tag = 'ViewInflation';
  let slice_glob = `'inflate'`;
  let result = await trace.engine.query(
    `
    SELECT RUN_METRIC('android/startup/slow_start_thresholds.sql');
    SELECT threshold_view_inflation_ns() as threshold_ns;
    `,
    tag,
  );
  let threshold_ns = 0;
  let it = result.iter({threshold_ns: LONG});
  threshold_ns = Number(it.threshold_ns);

  let slices = await getMainThreadSlices(trace, tag, slice_glob, threshold_ns, 0, startups);

  const uri = `/android_startups/view_inflation`;
  const title = `View Inflation`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getResourcesManagerTrack(
  trace: Trace,
  startups: Startup[],
): Promise<TrackNode | undefined> {
  // The log tag
  let tag = 'getResources';
  let slice_glob = `'ResourcesManager#getResources'`;
  let result = await trace.engine.query(
    `
    SELECT RUN_METRIC('android/startup/slow_start_thresholds.sql');
    SELECT threshold_resources_manager_get_resources_ns() as threshold_ns;
    `,
    tag,
  );
  let threshold_ns = 0;
  let it = result.iter({threshold_ns: LONG});
  threshold_ns = Number(it.threshold_ns);

  let slices = await getMainThreadSlices(trace, tag, slice_glob, threshold_ns, 0, startups);

  const uri = `/android_startups/get_resources`;
  const title = `Get Resources`;
  return getSlowReasonTrack(trace, slices, uri, title);
}

async function getMainThreadSlices(
  trace: Trace,
  tag: string,
  slice_glob: string,
  threshold_ns: number,
  threshold_pct: number,
  startups: Startup[],
): Promise<Array<Slice>> {
  const slices: Array<Slice> = [];

  // The log tag
  for (const startup of startups) {
    let result = await trace.engine.query(
    `
    INCLUDE PERFETTO MODULE android.startup.startups;
    SELECT slice_ts AS ts, slice_dur AS dur, slice_name AS name
    FROM android_thread_slices_for_all_startups l
    WHERE l.startup_id = ${startup.id}
      AND slice_name GLOB ${slice_glob}
    ORDER BY slice_dur DESC
    LIMIT 3`,
    tag,
    );
    let it = result.iter({ts: LONG, dur: LONG, name: STR});
    let sum = 0;
    for (; it.valid(); it.next()) {
        sum += Number(it.dur);
    }
    if (threshold_pct > 0 && sum > (startup.ts_end - startup.ts) / BigInt(100) * BigInt(threshold_pct)
      ||(threshold_ns > 0 && sum > threshold_ns)) { 
      it = result.iter({ts: LONG, dur: LONG, name: STR});
      for (; it.valid(); it.next()) {
        slices.push({
          ts: it.ts,
          dur: it.dur,
          name: it.name,
        });
      }
    }
  }
  return slices;
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



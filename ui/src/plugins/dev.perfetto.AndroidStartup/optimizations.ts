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

// The metadata container that keeps track of optimizations for packages that have startup events.
interface Startup {
  // The startup id.
  id: number;
  // The package name.
  package: string;
  // Time start
  ts: bigint;
  // Time end
  ts_end: bigint;
  // compilation filter
  filter?: string;
  // optimization status
  optimized?: boolean;
}

// The log tag
const tag = 'DexOptInsights';
// The pattern for the optimization filter.
const FILTER_PATTERN = /filter=([^\s]+)/;

/**
 * Returns a track node that contains optimization status
 * for the packages that started up in a trace.
 * @param trace The loaded trace.
 * @returns a track node with the optimizations status.
 * `undefined` if there are no app startups detected.
 */
export async function optimizationsTrack(
  trace: Trace,
): Promise<TrackNode | undefined> {
  const startups: Array<Startup> = [];
  const classLoadingTracks: Array<Promise<TrackNode>> = [];

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

  for (const startup of startups) {
    // For each startup id get the optimization status
    result = await trace.engine.query(
      `
        INCLUDE PERFETTO MODULE android.startup.startups;
        SELECT slice_name AS name FROM
          android_slices_for_startup_and_slice_name(${startup.id}, 'location=* status=* filter=* reason=*');`,
      tag,
    );
    const it = result.iter({name: STR});
    for (; it.valid(); it.next()) {
      const name = it.name;
      const relevant = name.indexOf(startup.package) >= 0;
      if (relevant) {
        const matches = name.match(FILTER_PATTERN);
        if (matches) {
          const filter = matches[1];
          startup.filter = filter;
          startup.optimized = filter === 'speed-profile';
        }
      }
    }
    const childTrack = classLoadingTrack(trace, startup);
    classLoadingTracks.push(childTrack);
  }

  // Create the optimizations track and also avoid re-querying for the data we already have.
  const sqlSource = startups
    .map((startup) => {
      return `SELECT
        ${startup.ts} AS ts,
        ${startup.ts_end - startup.ts} AS dur,
        '${buildName(startup)}' AS name,
        '${buildDetails(startup)}' AS details
      `;
    })
    .join('UNION ALL '); // The trailing space is important.

  const uri = '/android_startups_optimization_status';
  const title = 'Optimization Status';
  const track = await createQuerySliceTrack({
    trace: trace,
    uri: uri,
    data: {
      sqlSource: sqlSource,
      columns: ['ts', 'dur', 'name', 'details'],
    },
    argColumns: ['details'],
  });
  trace.tracks.registerTrack({
    uri,
    title,
    track,
  });
  const trackNode = new TrackNode({title, uri});
  for await (const classLoadingTrack of classLoadingTracks) {
    trackNode.addChildLast(classLoadingTrack);
  }
  return trackNode;
}

async function classLoadingTrack(
  trace: Trace,
  startup: Startup,
): Promise<TrackNode> {
  const sqlSource = `
    SELECT slice_ts as ts, slice_dur as dur, slice_name AS name FROM
      android_class_loading_for_startup
      WHERE startup_id = ${startup.id}
  `;
  const uri = `/android_startups/${startup.id}/classloading`;
  const title = `Unoptimized Class Loading in (${startup.package})`;
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

function buildName(startup: Startup): string {
  if (
    !!startup.filter === false ||
    startup.filter === 'verify' ||
    startup.filter === 'speed'
  ) {
    return `Sub-optimal compilation state (${startup.filter})`;
  } else if (startup.filter === 'speed-profile') {
    return 'Ideal compilation state (speed-profile)';
  } else {
    return `Unknown compilation state (${startup.filter})`;
  }
}

function buildDetails(startup: Startup): string {
  if (startup.filter === 'verify' || !!startup.filter === false) {
    return `No methods are precompiled, and class loading is unoptimized`;
  } else if (startup.filter === 'speed') {
    return 'Methods are all precompiled, and class loading is unoptimized';
  } else if (startup.filter === 'speed-profile') {
    return 'Methods and classes in the profile are optimized';
  } else {
    return `Unknown compilation state (${startup.filter})`;
  }
}

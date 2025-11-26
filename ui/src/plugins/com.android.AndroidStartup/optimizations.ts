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
import {TrackNode} from '../../public/workspace';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {DebugSliceTrackDetailsPanel} from '../../components/tracks/debug_slice_track_details_panel';

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
  const startupsResult = await trace.engine.query(
    `
      INCLUDE PERFETTO MODULE android.startup.startups;
      SELECT startup_id as id, package FROM android_startups;
    `,
  );

  // Nothing interesting to report.
  if (startupsResult.numRows() === 0) {
    return undefined;
  }

  const classLoadingTracks: Array<TrackNode> = [];
  const it = startupsResult.iter({id: NUM, package: STR});
  for (; it.valid(); it.next()) {
    const childTrack = classLoadingTrack(trace, {
      id: it.id,
      package: it.package,
    });
    classLoadingTracks.push(childTrack);
  }

  await trace.engine.query(
    `
      CREATE PERFETTO FUNCTION _startup_compilation_state(filter STRING)
      RETURNS STRING
      AS
      SELECT CASE
        WHEN $filter IN ('verify', 'speed') OR $filter IS NULL
          THEN FORMAT('Sub-optimal compilation state (%s)', ifnull($filter, 'unknown'))
        WHEN $filter = 'speed-profile'
          THEN 'Ideal compilation state (speed-profile)'
        ELSE
          FORMAT('Unknown compilation state (%s)', $filter)
      END;

      CREATE PERFETTO FUNCTION _startup_compilation_state_details(filter STRING)
      RETURNS STRING
      AS
      SELECT CASE
        WHEN $filter = 'verify' or $filter IS NULL
          THEN 'No methods are precompiled, and class loading is unoptimized'
        WHEN $filter = 'speed'
          THEN 'Methods are all precompiled, and class loading is unoptimized'
        WHEN $filter = 'speed-profile'
          THEN 'Methods and classes in the profile are optimized'
        ELSE
          FORMAT('Unknown compilation state (%s)', $filter)
      END;

      CREATE PERFETTO FUNCTION _startup_filter_extraction(startup_id INT)
      RETURNS TABLE(compile_ts LONG, filter STRING)
      AS
      SELECT
        MAX(slice_ts) AS compile_ts,
        regexp_extract(slice_name, 'filter=([^\\s]+)') as filter
      FROM android_thread_slices_for_all_startups
      WHERE slice_name GLOB 'location=* status=* filter=* reason=*'
        AND startup_id = $startup_id;

      CREATE PERFETTO TABLE _startup_optimization_slices AS
      SELECT
        s.ts,
        s.ts_end - s.ts as dur,
        s.startup_id as id,
        _startup_compilation_state(f.filter) AS name,
        _startup_compilation_state_details(f.filter) AS raw_details
      FROM android_startups s
      LEFT JOIN _startup_filter_extraction(s.startup_id) f
    `,
  );

  const uri = '/android_startups_optimization_status';
  const tableName = `_startup_optimization_slices`;
  trace.tracks.registerTrack({
    uri,
    renderer: SliceTrack.create({
      trace: trace,
      uri,
      dataset: new SourceDataset({
        src: tableName,
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
          raw_details: STR,
        },
      }),
      detailsPanel: (row) => {
        return new DebugSliceTrackDetailsPanel(trace, tableName, row.id);
      },
    }),
  });
  const trackNode = new TrackNode({name: 'Optimization Status', uri});
  for (const classLoadingTrack of classLoadingTracks) {
    trackNode.addChildLast(classLoadingTrack);
  }
  return trackNode;
}

function classLoadingTrack(
  trace: Trace,
  startup: {id: number; package: string},
): TrackNode {
  const uri = `/android_startups/${startup.id}/classloading`;
  trace.tracks.registerTrack({
    uri,
    renderer: SliceTrack.create({
      trace,
      uri,
      dataset: new SourceDataset({
        src: `
          SELECT
            slice_ts as ts,
            slice_dur as dur,
            slice_name AS name,
            slice_id as id
          FROM android_class_loading_for_startup
        `,
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
        },
        filter: {
          col: 'id',
          eq: startup.id,
        },
      }),
    }),
  });
  return new TrackNode({
    name: `Unoptimized Class Loading in (${startup.package})`,
    uri,
  });
}

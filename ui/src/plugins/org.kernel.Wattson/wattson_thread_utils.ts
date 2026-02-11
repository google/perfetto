// Copyright (C) 2026 The Android Open Source Project
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

import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {THREAD_STATE_TRACK_KIND} from '../../public/track_kinds';
import {WATTSON_THREAD_TRACK_KIND} from './track_kinds';

export async function addWattsonThreadTrack(
  trace: Trace,
  utid: number,
  threadTrackUri?: string,
): Promise<void> {
  const uri = `dev.perfetto.Sched#WattsonThreadCounter_${utid}`;
  if (trace.currentWorkspace.getTrackByUri(uri)) {
    trace.scrollTo({track: {uri, expandGroup: true}});
    return;
  }

  await trace.engine.query(`
    INCLUDE PERFETTO MODULE wattson.tasks.attribution;
    INCLUDE PERFETTO MODULE wattson.tasks.task_slices;
  `);

  const sqlSource = `
    WITH _filtered_thread AS (
      SELECT ts, dur, cpu, _auto_id
      FROM _all_tasks_flattened_slices
      WHERE utid = ${utid}
    ),
    _per_thread_estimate AS (
      SELECT ii.ts, ii.dur, uw.estimated_mw
      FROM _interval_intersect!(
        (
          _ii_subquery!(_unioned_wattson_estimates_mw),
          _ii_subquery!(_filtered_thread)
        ),
        (cpu)
      ) AS ii
      JOIN _unioned_wattson_estimates_mw AS uw ON uw._auto_id = id_0
      JOIN _filtered_thread AS s ON s._auto_id = id_1
    ),
    -- Need to fill in gaps where thread isn't running with 0mW entry
    gapless AS (
      SELECT ts, dur, estimated_mw FROM _per_thread_estimate
      UNION ALL
      SELECT
        ts + dur AS ts,
        COALESCE(LEAD(ts) OVER (ORDER BY ts), trace_end()) - ts - dur AS dur,
        0
      FROM _per_thread_estimate
    )
    SELECT
      ts,
      dur,
      estimated_mw AS value
    FROM gapless
    WHERE dur > 0
  `;

  const renderer = await createQueryCounterTrack({
    trace,
    uri,
    data: {sqlSource},
  });

  trace.tracks.registerTrack({
    uri,
    renderer,
    tags: {
      kinds: [WATTSON_THREAD_TRACK_KIND],
      utid,
    },
  });

  // Find the thread track and add the new track as a sibling
  let threadNode: TrackNode | undefined;
  if (threadTrackUri) {
    threadNode = trace.currentWorkspace.getTrackByUri(threadTrackUri);
  } else {
    const threadTrack = trace.tracks
      .getAllTracks()
      .find(
        (t) =>
          t.tags?.kinds?.includes(THREAD_STATE_TRACK_KIND) &&
          t.tags?.utid === utid,
      );
    if (threadTrack) {
      threadNode = trace.currentWorkspace.getTrackByUri(threadTrack.uri);
    }
  }

  if (threadNode?.parent) {
    const newNode = new TrackNode({
      uri,
      name: `${threadNode.name} Wattson power estimates`,
    });
    threadNode.parent.addChildBefore(newNode, threadNode);
  } else {
    const name = threadNode?.name ?? (utid === 0 ? 'swapper' : `utid ${utid}`);
    const newNode = new TrackNode({
      uri,
      name: `${name} Wattson power estimates`,
    });
    trace.currentWorkspace.addChildLast(newNode);
  }

  trace.scrollTo({track: {uri, expandGroup: true}});
}

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

import {CounterTrack} from '../../components/tracks/counter_track';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {THREAD_STATE_TRACK_KIND} from '../../public/track_kinds';
import {WATTSON_THREAD_TRACK_KIND} from './track_kinds';

export async function addWattsonThreadTrack(
  trace: Trace,
  utid: number,
  options?: {
    threadTrackUri?: string;
    pin?: boolean;
    scrollTo?: boolean;
  },
): Promise<void> {
  const uri = `dev.perfetto.Sched#WattsonThreadCounter_${utid}`;
  const existingTrack = trace.currentWorkspace.getTrackByUri(uri);
  if (existingTrack) {
    if (options?.pin && !existingTrack.isPinned) {
      existingTrack.pin();
    }
    if (options?.scrollTo ?? !options?.pin) {
      trace.scrollTo({track: {uri, expandGroup: true}});
    }
    return;
  }

  await trace.engine.query(`
    INCLUDE PERFETTO MODULE wattson.tasks.attribution;
    INCLUDE PERFETTO MODULE wattson.tasks.task_slices;
  `);

  const sqlSource = `
    WITH _per_thread_estimate AS (
      SELECT ts, dur, estimated_mw
      FROM _estimates_w_tasks_attribution
      WHERE utid = ${utid}
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

  const renderer = await CounterTrack.createMaterialized({
    trace,
    uri,
    sqlSource,
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
  if (options?.threadTrackUri) {
    threadNode = trace.currentWorkspace.getTrackByUri(options.threadTrackUri);
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

  const name = threadNode?.name ?? (utid === 0 ? 'swapper' : `utid ${utid}`);
  const newNode = new TrackNode({
    uri,
    name: `${name} Wattson power estimates`,
  });

  if (threadNode?.parent) {
    threadNode.parent.addChildBefore(newNode, threadNode);
  } else {
    trace.currentWorkspace.addChildLast(newNode);
  }

  if (options?.pin) {
    newNode.pin();
  }

  if (options?.scrollTo ?? !options?.pin) {
    trace.scrollTo({track: {uri, expandGroup: true}});
  }
}

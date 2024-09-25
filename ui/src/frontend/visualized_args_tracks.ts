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

import {uuidv4} from '../base/uuid';
// import {THREAD_SLICE_TRACK_KIND} from '../public';
import {NUM} from '../trace_processor/query_result';
import {globals} from './globals';
import {VisualisedArgsTrack} from './visualized_args_track';
import {TrackNode} from '../public/workspace';
import {Trace} from '../public/trace';

const VISUALISED_ARGS_SLICE_TRACK_URI_PREFIX = 'perfetto.VisualisedArgs';

export async function addVisualisedArgTracks(trace: Trace, argName: string) {
  const escapedArgName = argName.replace(/[^a-zA-Z]/g, '_');
  const tableName = `__arg_visualisation_helper_${escapedArgName}_slice`;

  const result = await trace.engine.query(`
        drop table if exists ${tableName};

        create table ${tableName} as
        with slice_with_arg as (
          select
            slice.id,
            slice.track_id,
            slice.ts,
            slice.dur,
            slice.thread_dur,
            NULL as cat,
            args.display_value as name
          from slice
          join args using (arg_set_id)
          where args.key='${argName}'
        )
        select
          *,
          (select count()
           from ancestor_slice(s1.id) s2
           join slice_with_arg s3 on s2.id=s3.id
          ) as depth
        from slice_with_arg s1
        order by id;

        select
          track_id as trackId,
          max(depth) as maxDepth
        from ${tableName}
        group by track_id;
    `);

  const it = result.iter({trackId: NUM, maxDepth: NUM});
  for (; it.valid(); it.next()) {
    const trackId = it.trackId;
    const maxDepth = it.maxDepth;

    const uri = `${VISUALISED_ARGS_SLICE_TRACK_URI_PREFIX}#${uuidv4()}`;
    trace.tracks.registerTrack({
      uri,
      title: argName,
      chips: ['metric'],
      track: new VisualisedArgsTrack({
        trace,
        uri,
        trackId,
        maxDepth,
        argName,
      }),
    });

    // Find the thread slice track that corresponds with this trackID and insert
    // this track before it.
    const threadSliceTrack = globals.workspace.flatTracks.find((trackNode) => {
      if (!trackNode.uri) return false;
      const trackDescriptor = globals.trackManager.getTrack(trackNode.uri);
      return (
        trackDescriptor &&
        trackDescriptor.tags?.kind === 'ThreadSliceTrack' &&
        trackDescriptor.tags?.trackIds?.includes(trackId)
      );
    });

    const parentGroup = threadSliceTrack?.parent;
    if (parentGroup) {
      const newTrack = new TrackNode({uri, title: argName});
      parentGroup.addChildBefore(newTrack, threadSliceTrack);
    }
  }
}

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

import {uuidv4} from '../../base/uuid';
import {NUM} from '../../trace_processor/query_result';
import {TrackNode} from '../../public/workspace';
import {Trace} from '../../public/trace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {createVisualizedArgsTrack} from './visualized_args_track';

const VISUALIZED_ARGS_SLICE_TRACK_URI_PREFIX = 'perfetto.VisualizedArgs';

export async function addVisualizedArgTracks(trace: Trace, argName: string) {
  const result = await trace.engine.query(`
    select distinct track_id as trackId
    from slice
    where arg_set_id in (select arg_set_id from args where key = '${argName}')
  `);
  const addedTracks: TrackNode[] = [];
  const it = result.iter({trackId: NUM});
  for (; it.valid(); it.next()) {
    const trackId = it.trackId;

    const uri = `${VISUALIZED_ARGS_SLICE_TRACK_URI_PREFIX}#${uuidv4()}`;
    trace.tracks.registerTrack({
      uri,
      chips: ['arg'],
      renderer: await createVisualizedArgsTrack({
        trace,
        uri,
        trackId,
        argName,
        onClose: () => {
          // Remove all added for this argument
          addedTracks.forEach((t) => t.parent?.removeChild(t));
        },
      }),
    });

    // Find the thread slice track that corresponds with this trackID and insert
    // this track before it.
    const threadSliceTrack = trace.currentWorkspace.flatTracks.find(
      (trackNode) => {
        if (!trackNode.uri) return false;
        const track = trace.tracks.getTrack(trackNode.uri);
        return (
          track &&
          track.tags?.kinds?.includes(SLICE_TRACK_KIND) &&
          track.tags?.trackIds?.includes(trackId)
        );
      },
    );

    const parentGroup = threadSliceTrack?.parent;
    if (parentGroup) {
      const newTrack = new TrackNode({uri, name: argName});
      parentGroup.addChildBefore(newTrack, threadSliceTrack);
      addedTracks.push(newTrack);
    }
  }
}

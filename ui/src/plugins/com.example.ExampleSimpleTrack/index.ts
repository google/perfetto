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

import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'com.example.ExampleSimpleTrack';

  async onTraceLoad(ctx: Trace): Promise<void> {
    // Choose a title for the track.
    const title = 'Slices that begin with "a"';

    // Choose a URI for the track - the URI should be unique within the scope of
    // the trace, but consistent so that it's the same between trace loads.
    const uri = `com.example.ExampleSimpleTrack#SlicesThatBeginWithA`;

    // Create & register the track.
    ctx.tracks.registerTrack({
      uri,
      title,
      track: new DatasetSliceTrack({
        trace: ctx,
        uri,
        dataset: new SourceDataset({
          // This is where we choose the SQL expression that describes the
          // events that appear on the track.
          src: `
            select
              id,
              ts,
              dur,
              name
            from slice
            where name glob 'a*'
          `,

          // Tell the track that these fields are available in the SQL we
          // provided. `id` and `ts` are required, `dur` tells the track to draw
          // slices instead of instants, and `name` tells the track the text to
          // display on the slices.
          schema: {
            id: NUM,
            ts: LONG,
            dur: LONG,
            name: STR,
          },
        }),
      }),
    });

    // Add the track to the default workspace, right at the top by providing a
    // large negative sort order.
    //
    // Note, if you want your plugin to be enabled by default on
    // ui.perfetto.dev, please don't do this. Be respectful to other uses and
    // organize your tracks neatly.
    const trackNode = new TrackNode({uri, title, sortOrder: -100});
    ctx.workspace.addChildInOrder(trackNode);
  }
}

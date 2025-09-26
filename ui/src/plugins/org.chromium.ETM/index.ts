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
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'org.chromium.ETM';

  async onTraceLoad(trace: Trace) {
    const title = 'ETM Session ID';
    const uri = `${trace.pluginId}#ETMSessionID`;
    const renderer = await CounterTrack.createMaterialized({
      trace,
      uri,
      dataset: new SourceDataset({
        src: `
          SELECT
            counter.id AS id
            ts,
            value
          FROM counter
          INNER JOIN counter_track ON counter_track.id = counter.track_id
          WHERE name = "ETMSession"
        `,
        schema: {
          id: NUM,
          ts: LONG,
          value: NUM,
        },
      }),
    });

    trace.tracks.registerTrack({
      uri,
      renderer,
      description: 'Track to show current ETM session on timeline',
    });

    const group = new TrackNode({
      name: 'ETM',
      isSummary: true,
    });

    const trackNode = new TrackNode({uri, name: title});
    group.addChildInOrder(trackNode);
    trace.workspace.addChildInOrder(group);
  }
}

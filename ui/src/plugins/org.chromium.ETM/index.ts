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

import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {TrackNode} from '../../public/workspace';

export default class ChromiumEtmPlugin implements PerfettoPlugin {
  static readonly id = 'org.chromium.ETM';

  async onTraceLoad(trace: Trace) {
    const title = 'ETM Session ID';
    const uri = `${ChromiumEtmPlugin.id}#ETMSessionID`;
    const query =
      'select ts, value from counter inner join ' +
      'counter_track on counter_track.id = counter.track_id ' +
      'where name = "ETMSession"';

    const renderer = await createQueryCounterTrack({
      trace,
      uri,
      data: {
        sqlSource: query,
      },
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
    trace.defaultWorkspace.addChildInOrder(group);
  }
}

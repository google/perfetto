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

import {SliceTrack} from '../../components/tracks/slice_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, LONG_NULL, STR} from '../../trace_processor/query_result';

const TRACK_NAME = 'Desktop Mode Windows';
const TRACK_URI = '/desktop_windows';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidDesktopMode';

  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query('INCLUDE PERFETTO MODULE android.desktop_mode');

    ctx.tracks.registerTrack({
      uri: TRACK_URI,
      renderer: await SliceTrack.createMaterialized({
        trace: ctx,
        uri: TRACK_URI,
        dataset: new SourceDataset({
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
          },
          src: `
            SELECT
              ts,
              dur,
              ifnull(p.package_name, 'uid=' || dw.uid) AS name
            FROM android_desktop_mode_windows dw
            LEFT JOIN package_list p
              ON CAST (dw.uid AS INT) % 100000 = p.uid
              AND p.uid != 1000
          `,
        }),
      }),
    });

    ctx.commands.registerCommand({
      id: 'com.android.AddDesktopModeTrack',
      name: 'Add Track: ' + TRACK_NAME,
      callback: () => this.addSimpleTrack(ctx),
    });
  }

  private addSimpleTrack(ctx: Trace) {
    const trackNode = new TrackNode({uri: TRACK_URI, name: TRACK_NAME});
    ctx.defaultWorkspace.addChildInOrder(trackNode);
    trackNode.pin();
  }
}

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

import {createQuerySliceTrack} from '../../public/lib/tracks/query_slice_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';

const INCLUDE_DESKTOP_MODULE_QUERY = `INCLUDE PERFETTO MODULE android.desktop_mode`;

const QUERY = `
SELECT
  ROW_NUMBER() OVER (ORDER BY ts) AS id,
  ts,
  dur,
  ifnull(p.package_name, 'uid=' || dw.uid) AS name
FROM android_desktop_mode_windows dw
LEFT JOIN package_list p ON CAST (dw.uid AS INT) % 100000 = p.uid AND p.uid != 1000
`;

const COLUMNS = ['id', 'ts', 'dur', 'name'];
const TRACK_NAME = 'Desktop Mode Windows';
const TRACK_URI = '/desktop_windows';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AndroidDesktopMode';

  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query(INCLUDE_DESKTOP_MODULE_QUERY);
    await this.registerTrack(ctx, QUERY);
    ctx.commands.registerCommand({
      id: 'dev.perfetto.DesktopMode#AddTrackDesktopWindowss',
      name: 'Add Track: ' + TRACK_NAME,
      callback: () => this.addSimpleTrack(ctx),
    });
  }

  private async registerTrack(ctx: Trace, sql: string) {
    const track = await createQuerySliceTrack({
      trace: ctx,
      uri: TRACK_URI,
      data: {
        sqlSource: sql,
        columns: COLUMNS,
      },
    });
    ctx.tracks.registerTrack({
      uri: TRACK_URI,
      title: TRACK_NAME,
      track,
    });
  }

  private addSimpleTrack(ctx: Trace) {
    const trackNode = new TrackNode({uri: TRACK_URI, title: TRACK_NAME});
    ctx.workspace.addChildInOrder(trackNode);
    trackNode.pin();
  }
}

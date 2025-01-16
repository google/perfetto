// Copyright (C) 2023 The Android Open Source Project
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

import {TrackNode} from '../../public/workspace';
import {NUM} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {createScreenshotsTrack} from './screenshots_track';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Screenshots';
  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.screenshots;
      select
        count() as count
      from android_screenshots
    `);
    const {count} = res.firstRow({count: NUM});

    if (count > 0) {
      const title = 'Screenshots';
      const uri = '/screenshots';
      ctx.tracks.registerTrack({
        uri,
        title,
        track: createScreenshotsTrack(ctx, uri),
      });
      const trackNode = new TrackNode({uri, title, sortOrder: -60});
      ctx.workspace.addChildInOrder(trackNode);
    }
  }
}

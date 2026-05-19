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

import './styles.scss';
import {TrackNode} from '../../public/workspace';
import {NUM} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {createScreenshotsTrack} from './screenshots_track';
import {
  ScreenshotScrubberTrack,
  ScreenshotSpanTrack,
} from './screenshot_span_track';

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
      const uri = '/screenshots';
      ctx.tracks.registerTrack({
        uri,
        renderer: createScreenshotsTrack(ctx, uri),
      });
      const trackNode = new TrackNode({
        uri,
        name: 'Screenshots',
        sortOrder: -60,
      });
      ctx.defaultWorkspace.addChildInOrder(trackNode);

      const spanUri = '/screenshots_span';
      ctx.tracks.registerTrack({
        uri: spanUri,
        renderer: new ScreenshotSpanTrack(ctx),
      });
      const spanNode = new TrackNode({
        uri: spanUri,
        name: 'Screenshots (Filmstrip)',
        sortOrder: -58,
      });
      ctx.defaultWorkspace.addChildInOrder(spanNode);

      const scrubberUri = '/screenshots_scrubber';
      ctx.tracks.registerTrack({
        uri: scrubberUri,
        renderer: new ScreenshotScrubberTrack(ctx),
      });
      const scrubberNode = new TrackNode({
        uri: scrubberUri,
        name: 'Screenshots (Scrubber)',
        sortOrder: -57,
      });
      ctx.defaultWorkspace.addChildInOrder(scrubberNode);
    }
  }
}

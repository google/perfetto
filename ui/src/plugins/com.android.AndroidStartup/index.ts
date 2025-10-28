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

import {LONG, LONG_NULL, NUM, STR} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {TrackNode} from '../../public/workspace';
import {optimizationsTrack} from './optimizations';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidStartup';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const e = ctx.engine;
    await e.query(`
      include perfetto module android.startup.startups;
    `);

    const cnt = await e.query('select count() cnt from android_startups');
    if (cnt.firstRow({cnt: LONG}).cnt === 0n) {
      return;
    }

    await e.query(`
      include perfetto module android.startup.startup_breakdowns;
    `);

    const startupTrackUri = `/android_startups`;
    ctx.tracks.registerTrack({
      uri: startupTrackUri,
      renderer: await SliceTrack.createMaterialized({
        trace: ctx,
        uri: startupTrackUri,
        dataset: new SourceDataset({
          schema: {
            id: NUM,
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
          },
          src: `
            SELECT
              startup_id AS id,
              ts,
              dur,
              package AS name
            FROM android_startups
          `,
        }),
      }),
    });

    // Needs a sort order lower than 'Ftrace Events' so that it is prioritized in the UI.
    const startupTrack = new TrackNode({
      name: 'Android App Startups',
      uri: startupTrackUri,
      sortOrder: -6,
    });
    ctx.defaultWorkspace.addChildInOrder(startupTrack);

    const breakdownTrackUri = '/android_startups_breakdown';
    ctx.tracks.registerTrack({
      uri: breakdownTrackUri,
      renderer: await SliceTrack.createMaterialized({
        trace: ctx,
        uri: breakdownTrackUri,
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
              reason AS name
            FROM android_startup_opinionated_breakdown
          `,
        }),
      }),
    });

    // Needs a sort order lower than 'Ftrace Events' so that it is prioritized in the UI.
    const breakdownTrack = new TrackNode({
      name: 'Android App Startups Breakdown',
      uri: breakdownTrackUri,
      sortOrder: -6,
    });
    startupTrack.addChildLast(breakdownTrack);

    const optimizations = await optimizationsTrack(ctx);
    if (optimizations) {
      startupTrack.addChildLast(optimizations);
    }
  }
}

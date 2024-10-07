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

import {LONG} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {
  SimpleSliceTrack,
  SimpleSliceTrackConfig,
} from '../../frontend/simple_slice_track';
import {TrackNode} from '../../public/workspace';
import {DebugSliceDetailsPanel} from '../../public/lib/debug_tracks/details_tab';
class AndroidStartup implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    const e = ctx.engine;
    await e.query(`include perfetto module android.startup.startups;`);

    const cnt = await e.query('select count() cnt from android_startups');
    if (cnt.firstRow({cnt: LONG}).cnt === 0n) {
      return;
    }

    const config: SimpleSliceTrackConfig = {
      data: {
        sqlSource: `
          SELECT l.ts AS ts, l.dur AS dur, l.package AS name
          FROM android_startups l
        `,
        columns: ['ts', 'dur', 'name'],
      },
      columns: {ts: 'ts', dur: 'dur', name: 'name'},
      argColumns: [],
    };
    const uri = `/android_startups`;
    const title = 'Android App Startups';
    const track = new SimpleSliceTrack(ctx, {trackUri: uri}, config);
    ctx.tracks.registerTrack({
      uri,
      title: 'Android App Startups',
      track,
      detailsPanel: ({eventId}) =>
        new DebugSliceDetailsPanel(ctx, track.sqlTableName, eventId),
    });
    const trackNode = new TrackNode({title, uri});
    ctx.workspace.addChildInOrder(trackNode);
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.AndroidStartup',
  plugin: AndroidStartup,
};

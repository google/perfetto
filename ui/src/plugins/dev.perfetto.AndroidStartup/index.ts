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

import {LONG, Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {
  SimpleSliceTrack,
  SimpleSliceTrackConfig,
} from '../../frontend/simple_slice_track';

class AndroidStartup implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
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
    ctx.registerStaticTrack({
      uri: `dev.perfetto.AndroidStartup#startups`,
      displayName: 'Android App Startups',
      trackFactory: (trackCtx) => {
        return new SimpleSliceTrack(ctx.engine, trackCtx, config);
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.AndroidStartup',
  plugin: AndroidStartup,
};

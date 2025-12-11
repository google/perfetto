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

import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidCounterTracks';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const {engine} = ctx;

    await engine.query(`include perfetto module android.binder;`);

    const counterTrackSource = `
      select client_ts as ts, count(*) value
      from android_binder_txns
      group by ts
    `;

    const trackNode = await this.loadCounterTrack(
      ctx,
      counterTrackSource,
      '/android_counter_track',
      'Android Counter Track',
    );
    ctx.defaultWorkspace.addChildFirst(trackNode);
  }

  private async loadCounterTrack(
    ctx: Trace,
    sqlSource: string,
    uri: string,
    title: string,
  ) {
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      data: {
        sqlSource,
        columns: ['ts', 'value'],
      },
    });

    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });

    return new TrackNode({name: title, uri, sortOrder: -7});
  }
}

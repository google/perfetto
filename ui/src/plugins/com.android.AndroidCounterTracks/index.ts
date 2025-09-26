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

import {
  CounterRowSchema,
  CounterTrack,
} from '../../components/tracks/counter_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidCounterTracks';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const {engine} = ctx;

    await engine.query(`include perfetto module android.binder;`);

    const trackNode = await this.loadCounterTrack(
      ctx,
      new SourceDataset({
        src: `
          SELECT
            client_ts AS ts,
            COUNT(*) value
          FROM android_binder_txns
          GROUP BY ts
        `,
        schema: {
          ts: LONG,
          value: NUM,
        },
      }),
      '/android_counter_track',
      'Android Counter Track',
    );
    ctx.workspace.addChildFirst(trackNode);
  }

  private async loadCounterTrack(
    ctx: Trace,
    dataset: SourceDataset<CounterRowSchema>,
    uri: string,
    title: string,
  ) {
    const track = await CounterTrack.createMaterialized({
      trace: ctx,
      uri,
      dataset,
    });

    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });

    return new TrackNode({name: title, uri, sortOrder: -7});
  }
}

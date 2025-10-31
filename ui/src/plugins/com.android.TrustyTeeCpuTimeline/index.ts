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

import {SliceTrack} from '../../components/tracks/slice_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.TrustyTeeCpuTimeline';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const uri = `com.android.TrustyTeeCpuTimeline#TrustyTeeCpuTimeline`;
    const query = `
      SELECT
        sched.id AS id,
        ts,
        dur,
        cpu,
        priority,
        name,
        utid,
        thread.name AS threadName,
        cpu AS depth
      FROM sched
      JOIN thread
        USING (utid)
      WHERE threadName GLOB 'trusty-nop*'
    `;

    ctx.tracks.registerTrack({
      uri,
      renderer: SliceTrack.create({
        trace: ctx,
        uri,
        dataset: new SourceDataset({
          src: query,
          schema: {
            id: NUM,
            ts: LONG,
            dur: LONG,
            name: STR,
            depth: NUM,
          },
        }),
        // Blank details panel - overrides details panel that assumes slices are
        // from the slice table.
        detailsPanel: () => {
          return {
            render: () => undefined,
          };
        },
      }),
    });

    const trackNode = new TrackNode({
      uri,
      name: 'Trusty Tee CPU Timeline',
      sortOrder: -100,
    });
    ctx.defaultWorkspace.addChildInOrder(trackNode);
  }
}

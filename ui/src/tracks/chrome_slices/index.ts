// Copyright (C) 2021 The Android Open Source Project
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

import {uuidv4} from '../../base/uuid';
import {ChromeSliceDetailsTab} from '../../frontend/chrome_slice_details_tab';
import {
  BottomTabToSCSAdapter,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {getTrackName} from '../../public/utils';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {ChromeSliceTrack, SLICE_TRACK_KIND} from './chrome_slice_track';

class ChromeSlicesPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
        with max_depth_materialized as (
          select track_id, max(depth) as maxDepth
          from slice
          group by track_id
        )
        select
          thread_track.utid as utid,
          thread_track.id as trackId,
          thread_track.name as trackName,
          EXTRACT_ARG(thread_track.source_arg_set_id,
                      'is_root_in_scope') as isDefaultTrackForScope,
          tid,
          thread.name as threadName,
          maxDepth,
          thread.upid as upid
        from thread_track
        join thread using(utid)
        join max_depth_materialized mdd on mdd.track_id = thread_track.id
  `);

    const it = result.iter({
      utid: NUM,
      trackId: NUM,
      trackName: STR_NULL,
      isDefaultTrackForScope: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
      maxDepth: NUM,
      upid: NUM_NULL,
    });

    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const tid = it.tid;
      const threadName = it.threadName;
      const maxDepth = it.maxDepth;

      const displayName = getTrackName({
        name: trackName,
        utid,
        tid,
        threadName,
        kind: 'Slices',
      });

      ctx.registerTrack({
        uri: `perfetto.ChromeSlices#${trackId}`,
        displayName,
        trackIds: [trackId],
        kind: SLICE_TRACK_KIND,
        trackFactory: ({trackKey}) => {
          const newTrackArgs = {
            engine: ctx.engine,
            trackKey,
          };
          return new ChromeSliceTrack(newTrackArgs, trackId, maxDepth);
        },
      });
    }

    ctx.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (sel) => {
          if (sel.kind !== 'CHROME_SLICE') {
            return undefined;
          }
          return new ChromeSliceDetailsTab({
            config: {
              table: sel.table ?? 'slice',
              id: sel.id,
            },
            engine: ctx.engine,
            uuid: uuidv4(),
          });
        },
      }),
    );
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.ChromeSlices',
  plugin: ChromeSlicesPlugin,
};

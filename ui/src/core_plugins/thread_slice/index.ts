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
import {THREAD_SLICE_TRACK_KIND} from '../../public';
import {ThreadSliceDetailsTab} from '../../frontend/thread_slice_details_tab';
import {
  BottomTabToSCSAdapter,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {getThreadUriPrefix, getTrackName} from '../../public/utils';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {ThreadSliceTrack} from '../../frontend/thread_slice_track';
import {removeFalsyValues} from '../../base/array_utils';

class ThreadSlicesPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;

    const result = await engine.query(`
      include perfetto module viz.summary.slices;
      include perfetto module viz.summary.threads;
      include perfetto module viz.threads;

      select
        thread_track.utid as utid,
        thread_track.id as trackId,
        thread_track.name as trackName,
        EXTRACT_ARG(thread_track.source_arg_set_id,
                    'is_root_in_scope') as isDefaultTrackForScope,
        tid,
        t.name as threadName,
        max_depth as maxDepth,
        t.upid as upid,
        is_main_thread as isMainThread,
        is_kernel_thread AS isKernelThread
      from thread_track
      join _threads_with_kernel_flag t using(utid)
      join _slice_track_summary sts on sts.id = thread_track.id
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
      isMainThread: NUM_NULL,
      isKernelThread: NUM,
    });

    for (; it.valid(); it.next()) {
      const {
        upid,
        utid,
        trackId,
        trackName,
        tid,
        threadName,
        maxDepth,
        isMainThread,
        isKernelThread,
        isDefaultTrackForScope,
      } = it;
      const displayName = getTrackName({
        name: trackName,
        utid,
        tid,
        threadName,
        kind: 'Slices',
      });

      ctx.registerTrack({
        uri: `${getThreadUriPrefix(upid, utid)}_slice_${trackId}`,
        title: displayName,
        tags: {
          trackIds: [trackId],
          kind: THREAD_SLICE_TRACK_KIND,
          utid,
          upid: upid ?? undefined,
          ...(isDefaultTrackForScope === 1 && {isDefaultTrackForScope: true}),
        },
        chips: removeFalsyValues([
          isKernelThread === 0 && isMainThread === 1 && 'main thread',
        ]),
        trackFactory: ({trackKey}) => {
          const newTrackArgs = {
            engine: ctx.engine,
            trackKey,
          };
          return new ThreadSliceTrack(newTrackArgs, trackId, maxDepth);
        },
      });
    }

    ctx.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (sel) => {
          if (sel.kind !== 'SLICE') {
            return undefined;
          }
          return new ThreadSliceDetailsTab({
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
  pluginId: 'perfetto.ThreadSlices',
  plugin: ThreadSlicesPlugin,
};

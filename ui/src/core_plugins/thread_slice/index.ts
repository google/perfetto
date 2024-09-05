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
import {THREAD_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {ThreadSliceDetailsTab} from '../../frontend/thread_slice_details_tab';
import {BottomTabToSCSAdapter} from '../../public/utils';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {getThreadUriPrefix, getTrackName} from '../../public/utils';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {ThreadSliceTrack} from '../../frontend/thread_slice_track';
import {removeFalsyValues} from '../../base/array_utils';
import {getOrCreateGroupForThread} from '../../public/standard_groups';
import {TrackNode} from '../../public/workspace';

class ThreadSlicesPlugin implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    const {engine} = ctx;

    const result = await engine.query(`
      include perfetto module viz.summary.slices;
      include perfetto module viz.summary.threads;
      include perfetto module viz.threads;

      select
        tt.utid as utid,
        tt.id as trackId,
        tt.name as trackName,
        extract_arg(
          tt.source_arg_set_id,
          'is_root_in_scope'
        ) as isDefaultTrackForScope,
        t.tid,
        t.name as threadName,
        s.max_depth as maxDepth,
        t.upid as upid,
        t.is_main_thread as isMainThread,
        t.is_kernel_thread AS isKernelThread
      from _thread_track_summary_by_utid_and_name s
      join _threads_with_kernel_flag t using(utid)
      join thread_track tt on s.track_id = tt.id
      where s.track_count = 1
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

      const uri = `${getThreadUriPrefix(upid, utid)}_slice_${trackId}`;
      ctx.tracks.registerTrack({
        uri,
        title: displayName,
        tags: {
          trackIds: [trackId],
          kind: THREAD_SLICE_TRACK_KIND,
          utid,
          upid: upid ?? undefined,
          ...(isDefaultTrackForScope === 1 && {isDefaultTrackForScope: true}),
          ...(isKernelThread === 1 && {kernelThread: true}),
        },
        chips: removeFalsyValues([
          isKernelThread === 0 && isMainThread === 1 && 'main thread',
        ]),
        track: new ThreadSliceTrack(
          {
            engine: ctx.engine,
            uri,
          },
          trackId,
          maxDepth,
        ),
      });
      const group = getOrCreateGroupForThread(ctx.workspace, utid);
      const track = new TrackNode(uri, displayName);
      track.sortOrder = 20;
      group.insertChildInOrder(track);
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

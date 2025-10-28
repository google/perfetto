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
  createQueryCounterTrack,
  SqlDataSource,
} from '../../components/tracks/query_counter_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND, SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';
import {createTraceProcessorSliceTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_slice_track';

async function registerAllocsTrack(
  ctx: Trace,
  uri: string,
  dataSource: SqlDataSource,
) {
  const track = await createQueryCounterTrack({
    trace: ctx,
    uri,
    data: dataSource,
  });
  ctx.tracks.registerTrack({
    uri,
    renderer: track,
  });
}

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidDmabuf';
  static readonly dependencies = [
    ProcessThreadGroupsPlugin,
    StandardGroupsPlugin,
    TraceProcessorTrackPlugin,
  ];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.memory.dmabuf`);

    const pids = await e.query(
      `SELECT DISTINCT upid, IIF(upid IS NULL, utid, NULL) AS utid FROM android_memory_cumulative_dmabuf`,
    );
    const it = pids.iter({upid: NUM_NULL, utid: NUM_NULL});
    for (; it.valid(); it.next()) {
      if (it.upid != null) {
        const uri = `/android_process_dmabuf_upid_${it.upid}`;
        const config: SqlDataSource = {
          sqlSource: `SELECT ts, value FROM android_memory_cumulative_dmabuf
                 WHERE upid = ${it.upid}`,
        };
        await registerAllocsTrack(ctx, uri, config);
        ctx.plugins
          .getPlugin(ProcessThreadGroupsPlugin)
          .getGroupForProcess(it.upid)
          ?.addChildInOrder(new TrackNode({uri, name: 'dmabuf allocs'}));
      } else if (it.utid != null) {
        const uri = `/android_process_dmabuf_utid_${it.utid}`;
        const config: SqlDataSource = {
          sqlSource: `SELECT ts, value FROM android_memory_cumulative_dmabuf
                 WHERE utid = ${it.utid}`,
        };
        await registerAllocsTrack(ctx, uri, config);
        ctx.plugins
          .getPlugin(ProcessThreadGroupsPlugin)
          .getGroupForThread(it.utid)
          ?.addChildInOrder(new TrackNode({uri, name: 'dmabuf allocs'}));
      }
    }
    const memoryGroupFn = () => {
      return ctx.plugins
        .getPlugin(StandardGroupsPlugin)
        .getOrCreateStandardGroup(ctx.defaultWorkspace, 'MEMORY');
    };
    const node = await addGlobalCounter(ctx, memoryGroupFn);
    await addGlobalAllocs(ctx, () => {
      return node ?? memoryGroupFn();
    });
  }
}

async function addGlobalCounter(ctx: Trace, parent: () => TrackNode) {
  const track = await ctx.engine.query(`
    select id, name
    from track
    where type = 'android_dma_heap'
  `);
  const it = track.maybeFirstRow({id: NUM, name: STR});
  if (!it) {
    return undefined;
  }
  const {id, name: title} = it;
  const uri = `/android_dmabuf_counter`;
  ctx.tracks.registerTrack({
    uri,
    tags: {
      kinds: [COUNTER_TRACK_KIND],
      trackIds: [id],
    },
    renderer: new TraceProcessorCounterTrack(ctx, uri, {}, id, title),
  });
  const node = new TrackNode({
    uri,
    name: title,
  });
  parent().addChildInOrder(node);
  return node;
}

async function addGlobalAllocs(ctx: Trace, parent: () => TrackNode) {
  const track = await ctx.engine.query(`
    select min(name) as name, group_concat(id) as trackIds
    from track
    where type = 'android_dma_allocations'
    group by track_group_id
  `);
  const it = track.maybeFirstRow({trackIds: STR, name: STR});
  if (!it) {
    return undefined;
  }
  const {trackIds, name: title} = it;
  const uri = `/android_dmabuf_allocs`;
  const ids = trackIds.split(',').map((x) => Number(x));
  ctx.tracks.registerTrack({
    uri,
    tags: {
      kinds: [SLICE_TRACK_KIND],
      trackIds: ids,
    },
    renderer: await createTraceProcessorSliceTrack({
      trace: ctx,
      uri,
      trackIds: ids,
    }),
  });
  const node = new TrackNode({
    uri,
    name: title,
  });
  parent().addChildInOrder(node);
}

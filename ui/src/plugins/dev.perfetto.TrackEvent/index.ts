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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {TrackNode} from '../../public/workspace';
import {assertExists, assertTrue} from '../../base/logging';
import {COUNTER_TRACK_KIND, SLICE_TRACK_KIND} from '../../public/track_kinds';
import {createTraceProcessorSliceTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_slice_track';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';
import {getTrackName} from '../../public/utils';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TrackEvent';
  static readonly dependencies = [
    ProcessThreadGroupsPlugin,
    TraceProcessorTrackPlugin,
  ];

  private parentTrackNodes = new Map<string, TrackNode>();

  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(`
      include perfetto module viz.summary.track_event;
      select
        ifnull(g.upid, t.upid) as upid,
        g.utid,
        g.parent_id as parentId,
        g.is_counter AS isCounter,
        g.name,
        g.unit,
        g.builtin_counter_type as builtinCounterType,
        g.has_data AS hasData,
        g.has_children AS hasChildren,
        g.track_ids as trackIds,
        g.order_id as orderId,
        t.name as threadName,
        t.tid as tid,
        ifnull(p.pid, tp.pid) as pid,
        ifnull(p.name, tp.name) as processName
      from _track_event_tracks_ordered_groups g
      left join process p using (upid)
      left join thread t using (utid)
      left join process tp on tp.upid = t.upid
    `);
    const it = res.iter({
      upid: NUM_NULL,
      utid: NUM_NULL,
      parentId: NUM_NULL,
      isCounter: NUM,
      name: STR_NULL,
      unit: STR_NULL,
      builtinCounterType: STR_NULL,
      hasData: NUM,
      hasChildren: NUM,
      trackIds: STR,
      orderId: NUM,
      threadName: STR_NULL,
      tid: NUM_NULL,
      pid: NUM_NULL,
      processName: STR_NULL,
    });
    const processGroupsPlugin = ctx.plugins.getPlugin(
      ProcessThreadGroupsPlugin,
    );
    const trackIdToTrackNode = new Map<number, TrackNode>();
    for (; it.valid(); it.next()) {
      const {
        upid,
        utid,
        parentId,
        isCounter,
        name,
        unit,
        builtinCounterType,
        hasData,
        hasChildren,
        trackIds: rawTrackIds,
        orderId,
        threadName,
        tid,
        pid,
        processName,
      } = it;

      // Don't add track_event tracks which don't have any data and don't have
      // any children.
      if (!hasData && !hasChildren) {
        continue;
      }

      const kind = isCounter ? COUNTER_TRACK_KIND : SLICE_TRACK_KIND;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const title = getTrackName({
        name,
        utid,
        upid,
        kind,
        threadTrack: utid !== null,
        threadName,
        processName,
        tid,
        pid,
      });
      const uri = `/track_event_${trackIds[0]}`;
      if (hasData && isCounter) {
        // Don't show any builtin counter.
        if (builtinCounterType !== null) {
          continue;
        }
        assertTrue(trackIds.length === 1);
        const trackId = trackIds[0];
        ctx.tracks.registerTrack({
          uri,
          title,
          tags: {
            kind,
            trackIds: [trackIds[0]],
            upid: upid ?? undefined,
            utid: utid ?? undefined,
          },
          track: new TraceProcessorCounterTrack(
            ctx,
            uri,
            {
              unit: unit ?? undefined,
            },
            trackId,
            title,
          ),
        });
      } else if (hasData) {
        ctx.tracks.registerTrack({
          uri,
          title,
          tags: {
            kind,
            trackIds: trackIds,
            upid: upid ?? undefined,
            utid: utid ?? undefined,
          },
          track: createTraceProcessorSliceTrack({trace: ctx, uri, trackIds}),
        });
      }
      const parent = this.findParentTrackNode(
        ctx,
        processGroupsPlugin,
        trackIdToTrackNode,
        parentId ?? undefined,
        upid ?? undefined,
        utid ?? undefined,
        hasChildren,
      );
      const node = new TrackNode({
        title,
        sortOrder: orderId,
        isSummary: hasData === 0,
        uri: uri,
      });
      parent.addChildInOrder(node);
      trackIdToTrackNode.set(trackIds[0], node);
    }
  }

  private findParentTrackNode(
    ctx: Trace,
    processGroupsPlugin: ProcessThreadGroupsPlugin,
    trackIdToTrackNode: Map<number, TrackNode>,
    parentId: number | undefined,
    upid: number | undefined,
    utid: number | undefined,
    hasChildren: number,
  ): TrackNode {
    if (parentId !== undefined) {
      return assertExists(trackIdToTrackNode.get(parentId));
    }
    if (utid !== undefined) {
      return assertExists(processGroupsPlugin.getGroupForThread(utid));
    }
    if (upid !== undefined) {
      return assertExists(processGroupsPlugin.getGroupForProcess(upid));
    }
    if (hasChildren) {
      return ctx.workspace.tracks;
    }
    const id = `/track_event_root`;
    let node = this.parentTrackNodes.get(id);
    if (node === undefined) {
      node = new TrackNode({
        title: 'Global Track Events',
        isSummary: true,
      });
      ctx.workspace.addChildInOrder(node);
      this.parentTrackNodes.set(id, node);
    }
    return node;
  }
}

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

import {assertExists} from '../../base/logging';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND, SLICE_TRACK_KIND} from '../../public/track_kinds';
import {getTrackName} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {SLICE_TRACK_SCHEMAS} from './slice_tracks';
import {TraceProcessorCounterTrack} from './trace_processor_counter_track';
import {COUNTER_TRACK_SCHEMAS} from './counter_tracks';
import {createTraceProcessorSliceTrack} from './trace_processor_slice_track';
import {TopLevelTrackGroup, TrackGroupSchema} from './types';
import {removeFalsyValues} from '../../base/array_utils';
import {createAggregationToTabAdaptor} from '../../components/aggregation_adapter';
import {CounterSelectionAggregator} from './counter_selection_aggregator';
import {SliceSelectionAggregator} from './slice_selection_aggregator';
import {PivotTableTab} from './pivot_table_tab';
import {MinimapRow} from '../../public/minimap';
import {Time} from '../../base/time';
import {Flamegraph} from '../../widgets/flamegraph';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
} from '../../components/query_flamegraph';
import {AreaSelection, areaSelectionsEqual} from '../../public/selection';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TraceProcessorTrack';
  static readonly dependencies = [
    ProcessThreadGroupsPlugin,
    StandardGroupsPlugin,
  ];

  private groups = new Map<string, TrackNode>();

  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.addCounters(ctx);
    await this.addSlices(ctx);
    this.addAggregations(ctx);
    this.addMinimapContentProvider(ctx);
  }

  private async addCounters(ctx: Trace) {
    const result = await ctx.engine.query(`
      include perfetto module viz.threads;

      with tracks_summary as (
        select
          ct.type,
          ct.name,
          ct.id,
          ct.unit,
          extract_arg(ct.dimension_arg_set_id, 'utid') as utid,
          extract_arg(ct.dimension_arg_set_id, 'upid') as upid
        from counter_track ct
        join _counter_track_summary using (id)
        order by ct.name
      )
      select
        s.*,
        thread.tid,
        thread.name as threadName,
        ifnull(p.pid, tp.pid) as pid,
        ifnull(p.name, tp.name) as processName,
        ifnull(thread.is_main_thread, 0) as isMainThread,
        ifnull(k.is_kernel_thread, 0) AS isKernelThread
      from tracks_summary s
      left join process p on s.upid = p.upid
      left join thread using (utid)
      left join _threads_with_kernel_flag k using (utid)
      left join process tp on thread.upid = tp.upid
      order by lower(s.name)
    `);

    const schemas = new Map(COUNTER_TRACK_SCHEMAS.map((x) => [x.type, x]));
    const it = result.iter({
      id: NUM,
      type: STR,
      name: STR_NULL,
      unit: STR_NULL,
      utid: NUM_NULL,
      upid: NUM_NULL,
      threadName: STR_NULL,
      processName: STR_NULL,
      tid: NUM_NULL,
      pid: NUM_NULL,
      isMainThread: NUM,
      isKernelThread: NUM,
    });
    for (; it.valid(); it.next()) {
      const {
        type,
        id: trackId,
        name,
        unit,
        utid,
        upid,
        threadName,
        processName,
        tid,
        pid,
        isMainThread,
        isKernelThread,
      } = it;
      const schema = schemas.get(type);
      if (schema === undefined) {
        continue;
      }
      const {group, topLevelGroup} = schema;
      const trackName = getTrackName({
        name,
        tid,
        threadName,
        pid,
        processName,
        upid,
        utid,
        kind: COUNTER_TRACK_KIND,
        threadTrack: utid !== undefined,
      });
      const uri = `/counter_${trackId}`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kind: COUNTER_TRACK_KIND,
          trackIds: [trackId],
          type: type,
          upid: upid ?? undefined,
          utid: utid ?? undefined,
          ...(isKernelThread === 1 && {kernelThread: true}),
        },
        chips: removeFalsyValues([
          isKernelThread === 0 && isMainThread === 1 && 'main thread',
        ]),
        renderer: new TraceProcessorCounterTrack(
          ctx,
          uri,
          {
            yMode: schema.mode,
            yRangeSharingKey: schema.shareYAxis ? it.type : undefined,
            unit: unit ?? undefined,
          },
          trackId,
          trackName,
        ),
      });
      this.addTrack(
        ctx,
        topLevelGroup,
        group,
        upid,
        utid,
        new TrackNode({
          uri,
          name: trackName,
          sortOrder: utid !== undefined || upid !== undefined ? 30 : 0,
        }),
      );
    }
  }

  private async addSlices(ctx: Trace) {
    const result = await ctx.engine.query(`
      include perfetto module viz.threads;

      with grouped as materialized (
        select
          t.type,
          t.name,
          extract_arg(t.dimension_arg_set_id, 'utid') as utid,
          extract_arg(t.dimension_arg_set_id, 'upid') as upid,
          group_concat(t.id) as trackIds,
          count() as trackCount
        from _slice_track_summary s
        join track t using (id)
        group by type, upid, utid, name
      )
      select
        s.type,
        s.name,
        s.utid,
        ifnull(s.upid, tp.upid) as upid,
        s.trackIds as trackIds,
        __max_layout_depth(s.trackCount, s.trackIds) as maxDepth,
        thread.tid,
        thread.name as threadName,
        ifnull(p.pid, tp.pid) as pid,
        ifnull(p.name, tp.name) as processName,
        ifnull(thread.is_main_thread, 0) as isMainThread,
        ifnull(k.is_kernel_thread, 0) AS isKernelThread
      from grouped s
      left join process p on s.upid = p.upid
      left join thread using (utid)
      left join _threads_with_kernel_flag k using (utid)
      left join process tp on thread.upid = tp.upid
      order by lower(s.name)
    `);

    const schemas = new Map(SLICE_TRACK_SCHEMAS.map((x) => [x.type, x]));
    const it = result.iter({
      type: STR,
      name: STR_NULL,
      utid: NUM_NULL,
      upid: NUM_NULL,
      trackIds: STR,
      maxDepth: NUM,
      tid: NUM_NULL,
      threadName: STR_NULL,
      pid: NUM_NULL,
      processName: STR_NULL,
      isMainThread: NUM,
      isKernelThread: NUM,
    });
    for (; it.valid(); it.next()) {
      const {
        trackIds: rawTrackIds,
        type,
        name,
        maxDepth,
        utid,
        upid,
        threadName,
        processName,
        tid,
        pid,
        isMainThread,
        isKernelThread,
      } = it;
      const schema = schemas.get(type);
      if (schema === undefined) {
        continue;
      }
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const {group, topLevelGroup} = schema;
      const trackName = getTrackName({
        name,
        tid,
        threadName,
        pid,
        processName,
        upid,
        utid,
        kind: SLICE_TRACK_KIND,
        threadTrack: utid !== undefined,
      });
      const uri = `/slice_${trackIds[0]}`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kind: SLICE_TRACK_KIND,
          trackIds: trackIds,
          type: type,
          upid: upid ?? undefined,
          utid: utid ?? undefined,
          ...(isKernelThread === 1 && {kernelThread: true}),
        },
        chips: removeFalsyValues([
          isKernelThread === 0 && isMainThread === 1 && 'main thread',
        ]),
        renderer: await createTraceProcessorSliceTrack({
          trace: ctx,
          uri,
          maxDepth,
          trackIds,
        }),
      });
      this.addTrack(
        ctx,
        topLevelGroup,
        group,
        upid,
        utid,
        new TrackNode({
          uri,
          name: trackName,
          sortOrder: utid !== undefined || upid !== undefined ? 20 : 0,
        }),
      );
    }
  }

  private addTrack(
    ctx: Trace,
    topLevelGroup: TopLevelTrackGroup,
    group: string | TrackGroupSchema | undefined,
    upid: number | null,
    utid: number | null,
    track: TrackNode,
  ) {
    switch (topLevelGroup) {
      case 'PROCESS': {
        const process = assertExists(
          ctx.plugins
            .getPlugin(ProcessThreadGroupsPlugin)
            .getGroupForProcess(assertExists(upid)),
        );
        this.getGroupByName(process, group, upid).addChildInOrder(track);
        break;
      }
      case 'THREAD': {
        const thread = assertExists(
          ctx.plugins
            .getPlugin(ProcessThreadGroupsPlugin)
            .getGroupForThread(assertExists(utid)),
        );
        this.getGroupByName(thread, group, utid).addChildInOrder(track);
        break;
      }
      case undefined: {
        this.getGroupByName(ctx.workspace.tracks, group, upid).addChildInOrder(
          track,
        );
        break;
      }
      default: {
        const standardGroup = ctx.plugins
          .getPlugin(StandardGroupsPlugin)
          .getOrCreateStandardGroup(ctx.workspace, topLevelGroup);
        this.getGroupByName(standardGroup, group, null).addChildInOrder(track);
        break;
      }
    }
  }

  private getGroupByName(
    node: TrackNode,
    group: string | TrackGroupSchema | undefined,
    scopeId: number | null,
  ) {
    if (group === undefined) {
      return node;
    }
    // This is potentially dangerous - ids MUST be unique within the entire
    // workspace - this seems to indicate that we could end up duplicating ids in
    // different nodes.
    const name = typeof group === 'string' ? group : group.name;
    const expanded =
      typeof group === 'string' ? false : group.expanded ?? false;
    const groupId = `tp_group_${scopeId}_${name.toLowerCase().replace(' ', '_')}`;
    const groupNode = this.groups.get(groupId);
    if (groupNode) {
      return groupNode;
    }
    const newGroup = new TrackNode({
      uri: `/${group}`,
      isSummary: true,
      name,
      collapsed: !expanded,
    });
    node.addChildInOrder(newGroup);
    this.groups.set(groupId, newGroup);
    return newGroup;
  }

  private addAggregations(ctx: Trace) {
    ctx.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(ctx, new CounterSelectionAggregator()),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(ctx, new SliceSelectionAggregator()),
    );
    ctx.selection.registerAreaSelectionTab(new PivotTableTab(ctx));
    ctx.selection.registerAreaSelectionTab(createSliceFlameGraphPanel(ctx));
  }

  private addMinimapContentProvider(ctx: Trace) {
    ctx.minimap.registerContentProvider({
      priority: 1,
      getData: async (timeSpan, resolution) => {
        const traceSpan = timeSpan.toTimeSpan();
        const sliceResult = await ctx.engine.query(`
              SELECT
                bucket,
                upid,
                IFNULL(SUM(utid_sum) / CAST(${resolution} AS FLOAT), 0) AS load
              FROM thread
              INNER JOIN (
                SELECT
                  IFNULL(CAST((ts - ${traceSpan.start}) / ${resolution} AS INT), 0) AS bucket,
                  SUM(dur) AS utid_sum,
                  utid
                FROM slice
                INNER JOIN thread_track ON slice.track_id = thread_track.id
                GROUP BY
                  bucket,
                  utid
              ) USING(utid)
              WHERE
                upid IS NOT NULL
              GROUP BY
                bucket,
                upid;
            `);

        const slicesData = new Map<string, MinimapRow>();
        const it = sliceResult.iter({bucket: LONG, upid: NUM, load: NUM});
        for (; it.valid(); it.next()) {
          const bucket = it.bucket;
          const upid = it.upid;
          const load = it.load;

          const ts = Time.add(traceSpan.start, resolution * bucket);

          const upidStr = upid.toString();
          let loadArray = slicesData.get(upidStr);
          if (loadArray === undefined) {
            loadArray = [];
            slicesData.set(upidStr, loadArray);
          }
          loadArray.push({ts, dur: resolution, load});
        }

        const rows: MinimapRow[] = [];
        for (const row of slicesData.values()) {
          rows.push(row);
        }
        return rows;
      },
    });
  }
}

function createSliceFlameGraphPanel(trace: Trace) {
  let previousSelection: AreaSelection | undefined;
  let sliceFlamegraph: QueryFlamegraph | undefined;
  return {
    id: 'slice_flamegraph_selection',
    name: 'Slice Flamegraph',
    render(selection: AreaSelection) {
      const selectionChanged =
        previousSelection === undefined ||
        !areaSelectionsEqual(previousSelection, selection);
      previousSelection = selection;
      if (selectionChanged) {
        sliceFlamegraph = computeSliceFlamegraph(trace, selection);
      }

      if (sliceFlamegraph === undefined) {
        return undefined;
      }

      return {isLoading: false, content: sliceFlamegraph.render()};
    },
  };
}

function computeSliceFlamegraph(trace: Trace, currentSelection: AreaSelection) {
  const trackIds = [];
  for (const trackInfo of currentSelection.tracks) {
    if (trackInfo?.tags?.kind !== SLICE_TRACK_KIND) {
      continue;
    }
    if (trackInfo.tags?.trackIds === undefined) {
      continue;
    }
    trackIds.push(...trackInfo.tags.trackIds);
  }
  if (trackIds.length === 0) {
    return undefined;
  }
  const metrics = metricsFromTableOrSubquery(
    `
      (
        select *
        from _viz_slice_ancestor_agg!((
          select s.id, s.dur
          from slice s
          left join slice t on t.parent_id = s.id
          where s.ts >= ${currentSelection.start}
            and s.ts <= ${currentSelection.end}
            and s.track_id in (${trackIds.join(',')})
            and t.id is null
        ))
      )
    `,
    [
      {
        name: 'Duration',
        unit: 'ns',
        columnName: 'self_dur',
      },
      {
        name: 'Samples',
        unit: '',
        columnName: 'self_count',
      },
    ],
    'include perfetto module viz.slices;',
  );
  return new QueryFlamegraph(trace, metrics, {
    state: Flamegraph.createDefaultState(metrics),
  });
}

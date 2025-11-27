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
import {
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {TrackNode} from '../../public/workspace';
import {assertExists, assertTrue} from '../../base/logging';
import {COUNTER_TRACK_KIND, SLICE_TRACK_KIND} from '../../public/track_kinds';
import {createTraceProcessorSliceTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_slice_track';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';
import {getTrackName} from '../../public/utils';
import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';
import {AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
  QueryFlamegraphWithMetrics,
} from '../../components/query_flamegraph';
import {Flamegraph, FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import {CallstackDetailsSection} from '../dev.perfetto.TraceProcessorTrack/callstack_details_section';
import {Store} from '../../base/store';
import {z} from 'zod';
import {createPerfettoTable} from '../../trace_processor/sql_utils';

function createTrackEventDetailsPanel(trace: Trace) {
  return () =>
    new ThreadSliceDetailsPanel(trace, {
      rightSections: [new CallstackDetailsSection(trace)],
    });
}

const TRACK_EVENT_PLUGIN_STATE_SCHEMA = z.object({
  areaSelectionFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
});

type TrackEventPluginState = z.infer<typeof TRACK_EVENT_PLUGIN_STATE_SCHEMA>;

export default class TrackEventPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TrackEvent';
  static readonly dependencies = [
    ProcessThreadGroupsPlugin,
    TraceProcessorTrackPlugin,
  ];

  private parentTrackNodes = new Map<string, TrackNode>();
  private store?: Store<TrackEventPluginState>;

  private migrateTrackEventPluginState(init: unknown): TrackEventPluginState {
    const result = TRACK_EVENT_PLUGIN_STATE_SCHEMA.safeParse(init);
    return result.data ?? {};
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.store = ctx.mountStore(TrackEventPlugin.id, (init) =>
      this.migrateTrackEventPluginState(init),
    );

    await ctx.engine.query(`include perfetto module viz.summary.track_event;`);

    // Step 1: Materialize track metadata
    // Can be cleaned up at the end of this function as only tables and
    // immediate queries depend on this.
    await using _ = await createPerfettoTable({
      name: '__track_event_tracks',
      engine: ctx.engine,
      as: `
        select
          ifnull(g.upid, t.upid) as upid,
          g.utid,
          g.parent_id as parentId,
          g.is_counter AS isCounter,
          g.name,
          g.description,
          g.unit,
          g.y_axis_share_key as yAxisShareKey,
          g.builtin_counter_type as builtinCounterType,
          g.has_data AS hasData,
          g.has_children AS hasChildren,
          g.has_callstacks AS hasCallstacks,
          g.min_track_id as minTrackId,
          g.track_ids as trackIds,
          g.order_id as orderId,
          t.name as threadName,
          t.tid as tid,
          ifnull(p.pid, tp.pid) as pid,
          ifnull(p.name, tp.name) as processName,
          (length(g.track_ids) - length(replace(g.track_ids, ',', '')) + 1) as trackCount
        from _track_event_tracks_ordered_groups g
        left join process p using (upid)
        left join thread t using (utid)
        left join process tp on tp.upid = t.upid
      `,
    });

    // Step 2: Create shared depth table for slice tracks with multiple trackIds
    await createPerfettoTable({
      name: '__trackevent_track_layout_depth',
      engine: ctx.engine,
      as: `
        select id, t.minTrackId, layout_depth as depth
        from __track_event_tracks t
        join experimental_slice_layout(t.trackIds) s
        where isCounter = 0 and trackCount > 1
        order by s.id
      `,
    });

    const res = await ctx.engine.query('select * from __track_event_tracks');
    const it = res.iter({
      upid: NUM_NULL,
      utid: NUM_NULL,
      parentId: NUM_NULL,
      isCounter: NUM,
      name: STR_NULL,
      description: STR_NULL,
      unit: STR_NULL,
      yAxisShareKey: STR_NULL,
      builtinCounterType: STR_NULL,
      hasData: NUM,
      hasChildren: NUM,
      hasCallstacks: NUM,
      trackIds: STR,
      orderId: NUM,
      threadName: STR_NULL,
      tid: LONG_NULL,
      pid: LONG_NULL,
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
        description,
        unit,
        yAxisShareKey,
        builtinCounterType,
        hasData,
        hasChildren,
        hasCallstacks,
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
      const trackName = getTrackName({
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
          description: description ?? undefined,
          tags: {
            kinds: [kind],
            trackIds: [trackIds[0]],
            upid: upid ?? undefined,
            utid: utid ?? undefined,
            trackEvent: true,
          },
          renderer: new TraceProcessorCounterTrack(
            ctx,
            uri,
            {
              unit: unit ?? undefined,
              // We combine the yAxisShareKey with the parentId to ensure that
              // only tracks under the same parent are grouped.
              yRangeSharingKey:
                yAxisShareKey === null
                  ? undefined
                  : `trackEvent-${parentId}-${yAxisShareKey}`,
            },
            trackId,
            trackName,
          ),
        });
      } else if (hasData) {
        ctx.tracks.registerTrack({
          uri,
          description: description ?? undefined,
          tags: {
            kinds: [kind],
            trackIds: trackIds,
            upid: upid ?? undefined,
            utid: utid ?? undefined,
            trackEvent: true,
            hasCallstacks: hasCallstacks === 1,
          },
          renderer: await createTraceProcessorSliceTrack({
            trace: ctx,
            uri,
            trackIds,
            detailsPanel: createTrackEventDetailsPanel(ctx),
            depthTableName:
              trackIds.length > 1
                ? '__trackevent_track_layout_depth'
                : undefined,
          }),
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
        name: trackName,
        sortOrder: orderId,
        isSummary: hasData === 0,
        uri,
      });
      parent.addChildInOrder(node);
      trackIdToTrackNode.set(trackIds[0], node);
    }

    // Register area selection tab for callstack flamegraph
    ctx.selection.registerAreaSelectionTab(
      this.createTrackEventCallstackFlamegraphTab(ctx),
    );
  }

  private createTrackEventCallstackFlamegraphTab(trace: Trace) {
    let previousSelection: AreaSelection | undefined;
    let flamegraphWithMetrics: QueryFlamegraphWithMetrics | undefined;
    return {
      id: 'track_event_callstack_flamegraph',
      name: 'Track Event Callstacks',
      render: (selection: AreaSelection) => {
        const changed =
          previousSelection === undefined ||
          !areaSelectionsEqual(previousSelection, selection);
        if (changed) {
          flamegraphWithMetrics = this.computeTrackEventCallstackFlamegraph(
            trace,
            selection,
          );
          previousSelection = selection;
        }
        if (flamegraphWithMetrics === undefined) {
          return undefined;
        }
        const {flamegraph, metrics} = flamegraphWithMetrics;
        const store = assertExists(this.store);
        return {
          isLoading: false,
          content: flamegraph.render({
            metrics,
            state: store.state.areaSelectionFlamegraphState,
            onStateChange: (state) => {
              store.edit((draft) => {
                draft.areaSelectionFlamegraphState = state;
              });
            },
          }),
        };
      },
    };
  }

  private computeTrackEventCallstackFlamegraph(
    trace: Trace,
    selection: AreaSelection,
  ): QueryFlamegraphWithMetrics | undefined {
    const trackIds = [];
    for (const trackInfo of selection.tracks) {
      const tids = trackInfo?.tags?.trackIds;
      if (tids && trackInfo.tags.hasCallstacks === true) {
        trackIds.push(...tids);
      }
    }
    if (trackIds.length === 0) {
      return undefined;
    }
    const metrics = metricsFromTableOrSubquery(
      `
      (
        with relevant_slices as (
          select id
          from _interval_intersect_single!(
            ${selection.start},
            ${selection.end},
            (
              select
                id,
                ts,
                max(dur, 0) as dur
              from slice
              where track_id in (${trackIds.join()})
            )
          )
        )
        select
          id,
          parent_id as parentId,
          name,
          mapping_name,
          source_file || ':' || line_number as source_location,
          self_count
        from _callstacks_for_callsites!((
          select callsite_id
          from relevant_slices
          join slice using (id)
          join __intrinsic_track_event_callstacks using (slice_id)
          where ts >= ${selection.start}
            and ts <= ${selection.end}
            and callsite_id is not null
          union all
          select end_callsite_id as callsite_id
          from relevant_slices
          join slice using (id)
          join __intrinsic_track_event_callstacks using (slice_id)
          where ts + dur >= ${selection.start}
            and ts + dur <= ${selection.end}
            and dur > 0
            and end_callsite_id is not null
        ))
      )
    `,
      [
        {
          name: 'Samples',
          unit: '',
          columnName: 'self_count',
        },
      ],
      `
     include perfetto module callstacks.stack_profile;
     include perfetto module intervals.intersect;
    `,
      [{name: 'mapping_name', displayName: 'Mapping'}],
      [
        {
          name: 'source_location',
          displayName: 'Source Location',
          mergeAggregation: 'ONE_OR_SUMMARY',
        },
      ],
    );
    const store = assertExists(this.store);
    store.edit((draft) => {
      draft.areaSelectionFlamegraphState = Flamegraph.updateState(
        draft.areaSelectionFlamegraphState,
        metrics,
      );
    });
    return {flamegraph: new QueryFlamegraph(trace), metrics};
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
      return ctx.defaultWorkspace.tracks;
    }
    const id = `/track_event_root`;
    let node = this.parentTrackNodes.get(id);
    if (node === undefined) {
      node = new TrackNode({
        name: 'Global Track Events',
        isSummary: true,
      });
      ctx.defaultWorkspace.addChildInOrder(node);
      this.parentTrackNodes.set(id, node);
    }
    return node;
  }
}

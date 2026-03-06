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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {NUM, STR} from '../../trace_processor/query_result';
import {createHeapProfileTrack} from './heap_profile_track';
import {TrackNode} from '../../public/workspace';
import {
  createPerfettoTable,
  createPerfettoView,
} from '../../trace_processor/sql_utils';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {Track} from '../../public/track';
import {FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import {Store} from '../../base/store';
import {z} from 'zod';
import {assertExists} from '../../base/assert';
import {
  isProfileDescriptor,
  ProfileDescriptor,
  profileDescriptor,
  ProfileType,
} from './common';
import {
  AreaSelection,
  areaSelectionsEqual,
  AreaSelectionTab,
} from '../../public/selection';
import {HeapProfileFlamegraphDetailsPanel} from './heap_profile_details_panel';

const EVENT_TABLE_NAME = 'heap_profile_events';

const HEAP_PROFILE_PLUGIN_STATE_SCHEMA = z.record(
  z.enum(ProfileType),
  z.object({
    trackFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
    areaSelectionFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  }),
);

type HeapProfilePluginState = z.infer<typeof HEAP_PROFILE_PLUGIN_STATE_SCHEMA>;

function trackUri(upid: number, type: string): string {
  return `/process_${upid}/${type}_heap_profile`;
}

export default class HeapProfilePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.HeapProfile';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  private readonly trackMap = new Map<string, Track>();
  private store?: Store<HeapProfilePluginState>;

  private migrateHeapProfilePluginState(init: unknown): HeapProfilePluginState {
    const result = HEAP_PROFILE_PLUGIN_STATE_SCHEMA.safeParse(init);
    return (
      result.data ?? {
        [ProfileType.NATIVE_HEAP_PROFILE]: {},
        [ProfileType.GENERIC_HEAP_PROFILE]: {},
        [ProfileType.JAVA_HEAP_SAMPLES]: {},
        [ProfileType.JAVA_HEAP_GRAPH]: {},
      }
    );
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    this.store = trace.mountStore(HeapProfilePlugin.id, (init) =>
      this.migrateHeapProfilePluginState(init),
    );
    await this.createHeapProfileTable(trace);
    const heapTypes = await this.getHeapTypes(trace);
    await this.addProcessTracks(trace, heapTypes);

    // For applicable heap types, register an area selection
    for (const heapType of heapTypes) {
      const descriptor = profileDescriptor(heapType);
      if (descriptor.type === ProfileType.JAVA_HEAP_GRAPH) {
        // There's no area selection for java heap dumps.
        continue;
      }
      trace.selection.registerAreaSelectionTab(
        this.heapProfileSelectionHandler(trace, descriptor),
      );
    }

    trace.onTraceReady.addListener(async () => {
      await this.selectHeapProfile(trace);
    });
  }

  private async createHeapProfileTable(trace: Trace) {
    await createPerfettoTable({
      engine: trace.engine,
      name: EVENT_TABLE_NAME,
      as: `
        WITH heap_profile_points AS (
          SELECT
            MIN(id) as id,
            ts,
            upid,
            heap_name
          FROM heap_profile_allocation
          GROUP BY ts, upid, heap_name
        ), heap_profile_slices AS (
          SELECT
            id,
            upid,
            heap_name,
            LAG(ts, 1, trace_start()) OVER (PARTITION BY upid, heap_name ORDER BY ts) + 1 AS ts,
            ts AS ts_end
          FROM heap_profile_points
        )

        SELECT
          MIN(id) as id,
          graph_sample_ts AS ts,
          upid,
          0 AS dur,
          0 AS depth,
          'java_heap_graph' AS type
        FROM heap_graph_object
        GROUP BY graph_sample_ts, upid

        UNION ALL

        SELECT
          id,
          ts,
          upid,
          ts_end - ts AS dur,
          0 AS depth,
          'heap_profile:' || heap_name AS type
        FROM heap_profile_slices
      `,
    });
  }

  private async getHeapTypes(trace: Trace): Promise<string[]> {
    const heapTypesResult = await trace.engine.query(`
      SELECT DISTINCT type
      FROM ${EVENT_TABLE_NAME}
    `);
    const heapTypes = [];
    for (const it = heapTypesResult.iter({type: STR}); it.valid(); it.next()) {
      heapTypes.push(it.type);
    }
    return heapTypes;
  }

  private async addProcessTracks(trace: Trace, heapTypes: readonly string[]) {
    const trackGroupsPlugin = trace.plugins.getPlugin(
      ProcessThreadGroupsPlugin,
    );
    const incomplete = await this.getIncomplete(trace);

    let typeIdx = 0;
    for (const heapType of heapTypes) {
      // Create a view for this particular type
      const viewName = `${EVENT_TABLE_NAME}_view_${typeIdx}`;
      await createPerfettoView({
        engine: trace.engine,
        name: viewName,
        as: `
          SELECT *
          FROM ${EVENT_TABLE_NAME}
          WHERE type = '${heapType}'
        `,
      });
      typeIdx++;

      const upidResult = await trace.engine.query(`
        SELECT DISTINCT upid
        FROM ${viewName}
      `);

      const upids = [];
      for (const it = upidResult.iter({upid: NUM}); it.valid(); it.next()) {
        upids.push(it.upid);
      }

      for (const upid of upids) {
        const group = trackGroupsPlugin.getGroupForProcess(upid);
        if (!group) continue;

        const store = assertExists(this.store);
        const uri = trackUri(upid, heapType);
        const descriptor = profileDescriptor(heapType);
        const track: Track = {
          uri,
          tags: {
            upid: upid,
            kinds: [heapType],
          },
          renderer: createHeapProfileTrack(
            trace,
            uri,
            viewName,
            upid,
            incomplete,
            store.state[descriptor.type].trackFlamegraphState,
            (state) => {
              store.edit((draft) => {
                draft[descriptor.type].trackFlamegraphState = state;
              });
            },
          ),
        };
        trace.tracks.registerTrack(track);

        this.trackMap.set(uri, track);
        const trackNode = new TrackNode({
          uri,
          name: descriptor.label,
          sortOrder: -30,
        });
        group.addChildInOrder(trackNode);
      }
    }
  }

  private async getIncomplete(trace: Trace): Promise<boolean> {
    const it = await trace.engine.query(`
      SELECT value
      FROM stats
      WHERE name = 'heap_graph_non_finalized_graph'
    `);
    const incomplete = it.firstRow({value: NUM}).value > 0;
    return incomplete;
  }

  private async selectHeapProfile(ctx: Trace) {
    const result = await ctx.engine.query(`
        SELECT
          id,
          upid,
          type
        FROM ${EVENT_TABLE_NAME}
        ORDER BY type, ts
        LIMIT 1
      `);

    const iter = result.maybeFirstRow({id: NUM, upid: NUM, type: STR});
    if (!iter) return;

    const uri = trackUri(iter.upid, iter.type);
    const track = this.trackMap.get(uri);
    if (!track) return;

    if (profileDescriptor(iter.type).type === ProfileType.JAVA_HEAP_GRAPH) {
      ctx.selection.selectTrackEvent(track.uri, iter.id);
    } else {
      ctx.selection.selectArea({
        start: ctx.traceInfo.start,
        end: ctx.traceInfo.end,
        trackUris: [uri],
      });
    }
  }

  private heapProfileSelectionHandler(
    trace: Trace,
    descriptor: ProfileDescriptor,
  ): AreaSelectionTab {
    let previousSelection: AreaSelection | undefined;
    let flamegraphPanel: HeapProfileFlamegraphDetailsPanel | undefined;
    return {
      id: `heap_profiler_flamegraph_selection_${descriptor.heapName}`,
      name: `${descriptor.label} flamegraph`,
      render: (selection: AreaSelection) => {
        const store = assertExists(this.store);
        const selectionChanged =
          previousSelection === undefined ||
          !areaSelectionsEqual(previousSelection, selection);
        previousSelection = selection;
        if (!selectionChanged) {
          return {isLoading: false, content: flamegraphPanel?.render()};
        }
        const upids = matchingTracks(selection, descriptor.type).map(
          (track) => track.tags!.upid,
        );
        // For the time being support selecting exactly one process.
        flamegraphPanel =
          upids.length !== 1
            ? undefined
            : new HeapProfileFlamegraphDetailsPanel(
                trace,
                false,
                upids[0]!,
                descriptor,
                selection.start,
                selection.end,
                store.state[descriptor.type].areaSelectionFlamegraphState,
                (state) => {
                  store.edit((draft) => {
                    draft[descriptor.type].areaSelectionFlamegraphState = state;
                  });
                },
              );
        return {
          isLoading: false,
          content: flamegraphPanel?.render(),
        };
      },
    };
  }
}

function matchingTracks(
  selection: AreaSelection,
  profileType: ProfileType,
): Track[] {
  return selection.tracks.filter((track) => {
    for (const kind of track.tags?.kinds || []) {
      if (
        isProfileDescriptor(kind) &&
        profileDescriptor(kind).type === profileType
      ) {
        return true;
      }
    }
    return false;
  });
}

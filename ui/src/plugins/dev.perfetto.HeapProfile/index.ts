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

import './styles.scss';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import type {time} from '../../base/time';
import {NUM, STR} from '../../trace_processor/query_result';
import {createHeapProfileTrack} from './heap_profile_track';
import {TrackNode} from '../../public/workspace';
import {
  createPerfettoTable,
  createPerfettoView,
} from '../../trace_processor/sql_utils';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import type {Track} from '../../public/track';
import {FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import type {Store} from '../../base/store';
import {z} from 'zod';
import {ensureExists} from '../../base/assert';
import {
  isProfileDescriptor,
  type ProfileDescriptor,
  profileDescriptor,
  ProfileType,
} from './common';
import {
  type AreaSelection,
  areaSelectionsEqual,
  type AreaSelectionTab,
} from '../../public/selection';
import {HeapProfileFlamegraphDetailsPanel} from './heap_profile_details_panel';
import {EvtSource} from '../../base/events';
import type {App} from '../../public/app';
import type {Flag} from '../../public/feature_flag';

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

  // Defined here (the Heap Dump Explorer's dependency) so both the Heap
  // Dump Explorer and HeapProfile can read the flag without a circular
  // import.
  static openHeapDumpExplorerByDefaultFlag: Flag;

  static onActivate(app: App) {
    HeapProfilePlugin.openHeapDumpExplorerByDefaultFlag =
      app.featureFlags.register({
        id: 'openHeapDumpExplorerByDefault',
        name: 'Open Heap Dump Explorer by default',
        description:
          'When enabled, traces that contain Java heap-graph data and no ' +
          'common timeline data (slice / sched) open directly in the Heap ' +
          'Dump Explorer instead of the timeline.',
        defaultValue: true,
      });
  }

  private readonly trackMap = new Map<string, Track>();
  private store?: Store<HeapProfilePluginState>;

  private readonly nodeSelectedEvt = new EvtSource<{
    pathHashes: string;
    isDominator: boolean;
    upid: number;
    ts: time;
  }>();

  registerOnNodeSelectedListener(
    cb: (args: {
      pathHashes: string;
      isDominator: boolean;
      upid: number;
      ts: time;
    }) => void,
  ): Disposable {
    return this.nodeSelectedEvt.addListener(cb);
  }

  private migrateHeapProfilePluginState(init: unknown): HeapProfilePluginState {
    const result = HEAP_PROFILE_PLUGIN_STATE_SCHEMA.safeParse(init);
    return (
      result.data ?? {
        [ProfileType.NATIVE_HEAP_PROFILE]: {},
        [ProfileType.GENERIC_HEAP_PROFILE]: {},
        [ProfileType.JAVA_HEAP_SAMPLES]: {},
        [ProfileType.JAVA_HEAP_GRAPH]: {},
        [ProfileType.OOME_CALLSTACK]: {},
      }
    );
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    this.store = trace.mountStore(HeapProfilePlugin.id, (init) =>
      this.migrateHeapProfilePluginState(init),
    );
    await this.createHeapProfileTable(trace);
    // Ordered by priority, so the tracks and the area-selection flamegraph tabs
    // registered below come out in the right order.
    const heapTypes = await this.getHeapTypes(trace);
    await this.addProcessTracks(trace, heapTypes);

    // For applicable heap types, register an area selection
    for (const heapType of heapTypes) {
      const descriptor = profileDescriptor(heapType.type);
      if (
        descriptor.type === ProfileType.JAVA_HEAP_GRAPH ||
        descriptor.type === ProfileType.OOME_CALLSTACK
      ) {
        // There's no area selection for java heap dumps or OOME callstacks.
        continue;
      }
      trace.selection.registerAreaSelectionTab(
        this.heapProfileSelectionHandler(trace, descriptor, heapType.priority),
      );
    }

    trace.onTraceReady.addListener(async () => {
      await this.selectHeapProfile(trace);
    });
  }

  private async createHeapProfileTable(trace: Trace) {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.oome;',
    );
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_profile.intervals;',
    );

    await createPerfettoTable({
      engine: trace.engine,
      name: EVENT_TABLE_NAME,
      as: `
        WITH events AS (
          -- heap_graph already has exactly one row per dump (with its own id),
          -- so read it directly rather than de-duplicating the much larger
          -- heap_graph_object table down to one row per dump.
          SELECT
            id,
            ts,
            upid,
            0 AS dur,
            0 AS depth,
            'java_heap_graph' AS type,
            NULL AS retained,
            NULL AS allocated,
            NULL AS delta
          FROM heap_graph

          UNION ALL

          -- Draw each dump over its profiling interval (see the module). The
          -- byte totals are surfaced in the slice name / tooltip.
          SELECT
            id,
            ts,
            upid,
            dur,
            0 AS depth,
            'heap_profile:' || heap_name AS type,
            retained,
            allocated,
            delta
          FROM _android_heap_profile_intervals

          UNION ALL

          SELECT
            id,
            ts,
            upid,
            0 AS dur,
            0 AS depth,
            'oome_callstack' AS type,
            NULL AS retained,
            NULL AS allocated,
            NULL AS delta
          FROM heap_graph
          WHERE dump_reason = 'OOME'
        )

        -- Display/selection priority: lower comes first. This is the single
        -- source of truth; TypeScript reads it back for track ordering.
        SELECT
          *,
          CASE type
            WHEN 'java_heap_graph' THEN 0
            WHEN 'heap_profile:libc.malloc' THEN 1
            WHEN 'heap_profile:com.android.art' THEN 2
            WHEN 'oome_callstack' THEN 4
            ELSE 3
          END AS priority
        FROM events
      `,
    });
  }

  private async getHeapTypes(
    trace: Trace,
  ): Promise<ReadonlyArray<{type: string; priority: number}>> {
    const heapTypesResult = await trace.engine.query(`
      SELECT DISTINCT type, priority
      FROM ${EVENT_TABLE_NAME}
      ORDER BY priority
    `);
    const heapTypes = [];
    for (
      const it = heapTypesResult.iter({type: STR, priority: NUM});
      it.valid();
      it.next()
    ) {
      heapTypes.push({type: it.type, priority: it.priority});
    }
    return heapTypes;
  }

  private async addProcessTracks(
    trace: Trace,
    heapTypes: ReadonlyArray<{type: string; priority: number}>,
  ) {
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
          WHERE type = '${heapType.type}'
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

        const store = ensureExists(this.store);
        const uri = trackUri(upid, heapType.type);
        const descriptor = profileDescriptor(heapType.type);
        const track: Track = {
          uri,
          tags: {
            upid: upid,
            kinds: [heapType.type],
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
            heapType.type === 'java_heap_graph'
              ? (args) => this.nodeSelectedEvt.notify(args)
              : undefined,
          ),
        };
        trace.tracks.registerTrack(track);

        this.trackMap.set(uri, track);
        const trackNode = new TrackNode({
          uri,
          name: descriptor.label,
          sortOrder: -30 + heapType.priority,
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
    const hdeWillTakeOver =
      HeapProfilePlugin.openHeapDumpExplorerByDefaultFlag.get() &&
      !(await traceHasTimelineData(ctx));
    const javaHeapGraphFilter = hdeWillTakeOver
      ? `WHERE type != 'java_heap_graph' AND type != 'oome_callstack'`
      : '';
    const result = await ctx.engine.query(`
        SELECT id, upid, type
        FROM ${EVENT_TABLE_NAME}
        ${javaHeapGraphFilter}
        ORDER BY priority, ts
        LIMIT 1
      `);

    const iter = result.maybeFirstRow({id: NUM, upid: NUM, type: STR});
    if (!iter) return;

    const uri = trackUri(iter.upid, iter.type);
    const track = this.trackMap.get(uri);
    if (!track) return;

    const profileType = profileDescriptor(iter.type).type;
    if (
      profileType === ProfileType.JAVA_HEAP_GRAPH ||
      profileType === ProfileType.OOME_CALLSTACK
    ) {
      ctx.selection.selectTrackEvent(track.uri, iter.id);
    } else {
      // Select every area-selectable heap track for this process so each heap
      // flamegraph tab has its track in the selection and actually renders.
      const tracksResult = await ctx.engine.query(`
        SELECT DISTINCT type
        FROM ${EVENT_TABLE_NAME}
        WHERE upid = ${iter.upid}
          AND type != 'java_heap_graph'
          AND type != 'oome_callstack'
      `);
      const trackUris = [];
      for (const it = tracksResult.iter({type: STR}); it.valid(); it.next()) {
        const trackUriForType = trackUri(iter.upid, it.type);
        if (this.trackMap.has(trackUriForType)) {
          trackUris.push(trackUriForType);
        }
      }
      ctx.selection.selectArea({
        start: ctx.traceInfo.start,
        end: ctx.traceInfo.end,
        trackUris: trackUris.length > 0 ? trackUris : [uri],
      });
    }
  }

  private heapProfileSelectionHandler(
    trace: Trace,
    descriptor: ProfileDescriptor,
    priority: number,
  ): AreaSelectionTab {
    let previousSelection: AreaSelection | undefined;
    let flamegraphPanel: HeapProfileFlamegraphDetailsPanel | undefined;
    return {
      id: `heap_profiler_flamegraph_selection_${descriptor.heapName}`,
      name: `${descriptor.label} flamegraph`,
      // AreaSelectionTab priority is "higher first", so negate our
      // "lower first" priority to keep the same order (native before ART).
      priority: -priority,
      render: (selection: AreaSelection) => {
        const store = ensureExists(this.store);
        const selectionChanged =
          previousSelection === undefined ||
          !areaSelectionsEqual(previousSelection, selection);
        previousSelection = selection;
        if (selectionChanged) {
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
                      draft[descriptor.type].areaSelectionFlamegraphState =
                        state;
                    });
                  },
                );
        }
        // Hide the tab entirely when this selection has no flamegraph for this
        // heap type, rather than showing a tab handle with empty content.
        if (flamegraphPanel === undefined) {
          return undefined;
        }
        return {isLoading: false, content: flamegraphPanel.render()};
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

export async function traceHasTimelineData(ctx: Trace): Promise<boolean> {
  // We treat a small number of slices as not having timeline data cos
  // there are some inevitable slices like trace triggers on oom etc.
  const res = await ctx.engine.query(`
    SELECT
      (SELECT count(id) FROM slice) > 50 OR
      EXISTS(SELECT 1 FROM sched) OR
      EXISTS(SELECT 1 FROM heap_profile_allocation) OR
      EXISTS(SELECT 1 FROM perf_sample)
      AS res
  `);
  return res.firstRow({res: NUM}).res > 0;
}

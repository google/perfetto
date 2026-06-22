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

import m from 'mithril';
import type {TrackData} from '../../components/tracks/track_data';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {assertExists} from '../../base/assert';
import {getThreadUriPrefix} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {type AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {
  metricsFromTableOrSubquery,
  type QueryFlamegraphMetric,
} from '../../components/query_flamegraph';
import {FlamegraphPanel} from '../../components/flamegraph_panel';
import {
  Flamegraph,
  FLAMEGRAPH_STATE_SCHEMA,
  type FlamegraphState,
} from '../../widgets/flamegraph';
import type {Store} from '../../base/store';
import {z} from 'zod';
import {SourceDataset} from '../../trace_processor/dataset';
import {createProfilingTrack} from '../dev.perfetto.CpuProfile/profiling_track';
import CpuProfilePlugin from '../dev.perfetto.CpuProfile';

export interface Data extends TrackData {
  tsStarts: BigInt64Array;
}

const INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND = 'InstrumentsSamplesProfileTrack';

const INSTRUMENTS_SAMPLES_PROFILE_PLUGIN_STATE_SCHEMA = z.object({
  areaSelectionFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  detailsPanelFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
});

type InstrumentsSamplesProfilePluginState = z.infer<
  typeof INSTRUMENTS_SAMPLES_PROFILE_PLUGIN_STATE_SCHEMA
>;

function makeUriForProc(upid: number) {
  return `/process_${upid}/instruments_samples_profile`;
}

export default class InstrumentsSamplesProfilePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.InstrumentsSamplesProfile';
  static readonly dependencies = [ProcessThreadGroupsPlugin, CpuProfilePlugin];

  private store?: Store<InstrumentsSamplesProfilePluginState>;

  private migrateInstrumentsSamplesProfilePluginState(
    init: unknown,
  ): InstrumentsSamplesProfilePluginState {
    const result =
      INSTRUMENTS_SAMPLES_PROFILE_PLUGIN_STATE_SCHEMA.safeParse(init);
    return result.data ?? {};
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.store = ctx.mountStore(InstrumentsSamplesProfilePlugin.id, (init) =>
      this.migrateInstrumentsSamplesProfilePluginState(init),
    );
    await ctx.engine.query('INCLUDE PERFETTO MODULE callstacks.stack_profile;');
    const pResult = await ctx.engine.query(`
      select distinct upid
      from instruments_sample
      join thread using (utid)
      where callsite_id is not null and upid is not null
    `);
    const store = assertExists(this.store);
    for (const it = pResult.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const uri = makeUriForProc(upid);
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND],
          upid,
        },
        renderer: createProcessInstrumentsSamplesProfileTrack(
          ctx,
          uri,
          upid,
          store.state.detailsPanelFlamegraphState,
          (state) => {
            store.edit((draft) => {
              draft.detailsPanelFlamegraphState = state;
            });
          },
        ),
      });
      // const slicesUri = `${uri}_slices`;
      // const tableName = `slices_${slicesUri.replace(/[^a-zA-Z0-9]/g, '_')}`;
      // await ctx.engine.query(`
      //   CREATE TABLE ${tableName} AS
      //   WITH samples AS (
      //     SELECT
      //       p.id AS sample_id,
      //       ts,
      //       LEAD(ts, 1, (SELECT end_ts FROM trace_bounds)) OVER (ORDER BY ts) - ts AS dur,
      //       callsite_id
      //     FROM instruments_sample p
      //     JOIN thread USING (utid)
      //     WHERE callsite_id IS NOT NULL
      //       AND upid = ${upid}
      //   ),
      //   callstack_path AS (
      //     SELECT
      //       callsite_id,
      //       id AS forest_id,
      //       parent_id AS forest_parent_id,
      //       name,
      //       0 AS depth
      //     FROM _callstack_spc_forest
      //     WHERE callsite_id IN (SELECT DISTINCT callsite_id FROM samples)
      //       AND is_leaf_function_in_callsite_frame = 1
      //
      //     UNION ALL
      //
      //     SELECT
      //       p.callsite_id,
      //       f.id AS forest_id,
      //       f.parent_id AS forest_parent_id,
      //       f.name,
      //       p.depth + 1 AS depth
      //     FROM callstack_path p
      //     JOIN _callstack_spc_forest f ON p.forest_parent_id = f.id
      //   ),
      //   path_with_max_depth AS (
      //     SELECT
      //       callsite_id,
      //       name,
      //       depth,
      //       MAX(depth) OVER (PARTITION BY callsite_id) AS max_depth
      //     FROM callstack_path
      //   )
      //   SELECT
      //     s.sample_id AS id,
      //     s.ts,
      //     s.dur,
      //     p.name,
      //     (p.max_depth - p.depth) AS depth,
      //     s.callsite_id AS callsiteId
      //   FROM samples s
      //   JOIN path_with_max_depth p USING (callsite_id)
      // `);
      // ctx.tracks.registerTrack({
      //   uri: slicesUri,
      //   tags: {
      //     kinds: [INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND],
      //     upid,
      //   },
      //   renderer: createProcessInstrumentsSamplesCallstackSlicesTrack(
      //     ctx,
      //     slicesUri,
      //     tableName,
      //     upid,
      //     store.state.detailsPanelFlamegraphState,
      //     (state) => {
      //       store.edit((draft) => {
      //         draft.detailsPanelFlamegraphState = state;
      //       });
      //     },
      //   ),
      // });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForProcess(upid);
      const track = new TrackNode({
        uri,
        name: 'Process Callstacks',
        sortOrder: -40,
      });
      group?.addChildInOrder(track);
      // const slicesTrack = new TrackNode({
      //   uri: slicesUri,
      //   name: 'Process Callstack Slices',
      //   sortOrder: -39,
      // });
      // group?.addChildInOrder(slicesTrack);
    }
    const tResult = await ctx.engine.query(`
      select distinct
        utid,
        tid,
        thread.name as threadName,
        upid
      from instruments_sample
      join thread using (utid)
      where callsite_id is not null
    `);
    for (
      const it = tResult.iter({
        utid: NUM,
        tid: LONG,
        threadName: STR_NULL,
        upid: NUM_NULL,
      });
      it.valid();
      it.next()
    ) {
      const {threadName, utid, tid, upid} = it;
      const name =
        threadName === null
          ? `Thread Callstacks ${tid}`
          : `${threadName} Callstacks ${tid}`;
      const uri = `${getThreadUriPrefix(upid, utid)}_instruments_samples_profile`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND],
          utid,
          upid: upid ?? undefined,
        },
        renderer: createThreadInstrumentsSamplesProfileTrack(
          ctx,
          uri,
          utid,
          store.state.detailsPanelFlamegraphState,
          (state) => {
            store.edit((draft) => {
              draft.detailsPanelFlamegraphState = state;
            });
          },
        ),
      });
      // const slicesUri = `${uri}_slices`;
      // const tableName = `slices_${slicesUri.replace(/[^a-zA-Z0-9]/g, '_')}`;
      // await ctx.engine.query(`
      //   CREATE TABLE ${tableName} AS
      //   WITH samples AS (
      //     SELECT
      //       p.id AS sample_id,
      //       ts,
      //       LEAD(ts, 1, (SELECT end_ts FROM trace_bounds)) OVER (ORDER BY ts) - ts AS dur,
      //       callsite_id
      //     FROM instruments_sample p
      //     WHERE callsite_id IS NOT NULL
      //       AND utid = ${utid}
      //   ),
      //   callstack_path AS (
      //     SELECT
      //       callsite_id,
      //       id AS forest_id,
      //       parent_id AS forest_parent_id,
      //       name,
      //       0 AS depth
      //     FROM _callstack_spc_forest
      //     WHERE callsite_id IN (SELECT DISTINCT callsite_id FROM samples)
      //       AND is_leaf_function_in_callsite_frame = 1
      //
      //     UNION ALL
      //
      //     SELECT
      //       p.callsite_id,
      //       f.id AS forest_id,
      //       f.parent_id AS forest_parent_id,
      //       f.name,
      //       p.depth + 1 AS depth
      //     FROM callstack_path p
      //     JOIN _callstack_spc_forest f ON p.forest_parent_id = f.id
      //   ),
      //   path_with_max_depth AS (
      //     SELECT
      //       callsite_id,
      //       name,
      //       depth,
      //       MAX(depth) OVER (PARTITION BY callsite_id) AS max_depth
      //     FROM callstack_path
      //   )
      //   SELECT
      //     s.sample_id AS id,
      //     s.ts,
      //     s.dur,
      //     p.name,
      //     (p.max_depth - p.depth) AS depth,
      //     s.callsite_id AS callsiteId
      //   FROM samples s
      //   JOIN path_with_max_depth p USING (callsite_id)
      // `);
      // ctx.tracks.registerTrack({
      //   uri: slicesUri,
      //   tags: {
      //     kinds: [INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND],
      //     utid,
      //     upid: upid ?? undefined,
      //   },
      //   renderer: createThreadInstrumentsSamplesCallstackSlicesTrack(
      //     ctx,
      //     slicesUri,
      //     tableName,
      //     utid,
      //     store.state.detailsPanelFlamegraphState,
      //     (state) => {
      //       store.edit((draft) => {
      //         draft.detailsPanelFlamegraphState = state;
      //       });
      //     },
      //   ),
      // });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForThread(utid);
      const track = new TrackNode({uri, name, sortOrder: -50});
      group?.addChildInOrder(track);
      // const slicesTrack = new TrackNode({
      //   uri: slicesUri,
      //   name: threadName === null
      //     ? `Thread Callstack Slices ${tid}`
      //     : `${threadName} Callstack Slices ${tid}`,
      //   sortOrder: -49,
      // });
      // group?.addChildInOrder(slicesTrack);
    }

    ctx.onTraceReady.addListener(async () => {
      await selectInstrumentsSample(ctx);
    });

    ctx.selection.registerAreaSelectionTab(this.createAreaSelectionTab(ctx));
  }

  private createAreaSelectionTab(trace: Trace) {
    let previousSelection: undefined | AreaSelection;
    let flamegraphMetrics: ReadonlyArray<QueryFlamegraphMetric> | undefined;
    return {
      id: 'instruments_sample_flamegraph',
      name: 'Instruments Sample Flamegraph',
      render: (selection: AreaSelection) => {
        const changed =
          previousSelection === undefined ||
          !areaSelectionsEqual(previousSelection, selection);
        if (changed) {
          flamegraphMetrics =
            this.computeInstrumentsSampleFlamegraph(selection);
          previousSelection = selection;
        }
        if (flamegraphMetrics === undefined) {
          return undefined;
        }
        const store = assertExists(this.store);
        return {
          isLoading: false,
          content: m(FlamegraphPanel, {
            trace,
            metrics: flamegraphMetrics,
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

  private computeInstrumentsSampleFlamegraph(
    currentSelection: AreaSelection,
  ): ReadonlyArray<QueryFlamegraphMetric> | undefined {
    const upids = getUpidsFromInstrumentsSampleAreaSelection(currentSelection);
    const utids = getUtidsFromInstrumentsSampleAreaSelection(currentSelection);
    if (utids.length === 0 && upids.length === 0) {
      return undefined;
    }
    const metrics = metricsFromTableOrSubquery({
      tableOrSubquery: `
      (
        select id, parent_id as parentId, name, self_count
        from _callstacks_for_callsites!((
          select p.callsite_id
          from instruments_sample p
          join thread t using (utid)
          where p.ts >= ${currentSelection.start}
            and p.ts <= ${currentSelection.end}
            and (
              p.utid in (${utids.join(',')})
              or t.upid in (${upids.join(',')})
            )
        ))
      )
    `,
      tableMetrics: [
        {
          name: 'Instruments Samples',
          unit: '',
          columnName: 'self_count',
        },
      ],
      dependencySql: 'include perfetto module appleos.instruments.samples',
      nameColumnLabel: 'Symbol',
    });
    const store = assertExists(this.store);
    store.edit((draft) => {
      draft.areaSelectionFlamegraphState = Flamegraph.updateState(
        draft.areaSelectionFlamegraphState,
        metrics,
      );
    });
    return metrics;
  }
}

async function selectInstrumentsSample(ctx: Trace) {
  const profile = await assertExists(ctx.engine).query(`
    select upid
    from instruments_sample
    join thread using (utid)
    where callsite_id is not null
    order by ts desc
    limit 1
  `);
  if (profile.numRows() !== 1) return;
  const row = profile.firstRow({upid: NUM});
  const upid = row.upid;

  // Create an area selection over the first process with a instruments samples track
  ctx.selection.selectArea({
    start: ctx.traceInfo.start,
    end: ctx.traceInfo.end,
    trackUris: [makeUriForProc(upid)],
  });
}

function getUpidsFromInstrumentsSampleAreaSelection(
  currentSelection: AreaSelection,
) {
  const upids = [];
  for (const trackInfo of currentSelection.tracks) {
    if (
      trackInfo?.tags?.kinds?.includes(
        INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND,
      ) &&
      trackInfo.tags?.utid === undefined
    ) {
      upids.push(assertExists(trackInfo.tags?.upid));
    }
  }
  return upids;
}

function getUtidsFromInstrumentsSampleAreaSelection(
  currentSelection: AreaSelection,
) {
  const utids = [];
  for (const trackInfo of currentSelection.tracks) {
    if (
      trackInfo?.tags?.kinds?.includes(
        INSTRUMENTS_SAMPLES_PROFILE_TRACK_KIND,
      ) &&
      trackInfo.tags?.utid !== undefined
    ) {
      utids.push(trackInfo.tags?.utid);
    }
  }
  return utids;
}

export function createProcessInstrumentsSamplesProfileTrack(
  trace: Trace,
  uri: string,
  upid: number,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
) {
  return createProfilingTrack(
    trace,
    uri,
    {
      dataset: new SourceDataset({
        schema: {
          id: NUM,
          ts: LONG,
          callsiteId: NUM,
        },
        src: `
          SELECT
            p.id,
            ts,
            callsite_id AS callsiteId,
            upid
          FROM instruments_sample p
          JOIN thread USING (utid)
          WHERE callsite_id IS NOT NULL
          ORDER BY ts
        `,
        filter: {
          col: 'upid',
          eq: upid,
        },
      }),
      callsiteQuery: (ts) => `
        SELECT p.callsite_id
        FROM instruments_sample p
        JOIN thread t USING (utid)
        WHERE p.ts = ${ts}
          AND t.upid = ${upid}
      `,
      sqlModule: 'appleos.instruments.samples',
      metricName: 'Instruments Samples',
      panelTitle: 'Instruments Samples',
      sliceName: 'Instruments Sample',
    },
    detailsPanelState,
    onDetailsPanelStateChange,
  );
}

export function createThreadInstrumentsSamplesProfileTrack(
  trace: Trace,
  uri: string,
  utid: number,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
) {
  return createProfilingTrack(
    trace,
    uri,
    {
      dataset: new SourceDataset({
        schema: {
          id: NUM,
          ts: LONG,
          callsiteId: NUM,
        },
        src: `
          SELECT
            p.id,
            ts,
            callsite_id AS callsiteId,
            utid
          FROM instruments_sample p
          WHERE callsite_id IS NOT NULL
          ORDER BY ts
        `,
        filter: {
          col: 'utid',
          eq: utid,
        },
      }),
      callsiteQuery: (ts) => `
        SELECT p.callsite_id
        FROM instruments_sample p
        WHERE p.ts = ${ts}
          AND p.utid = ${utid}
      `,
      sqlModule: 'appleos.instruments.samples',
      metricName: 'Instruments Samples',
      panelTitle: 'Instruments Samples',
      sliceName: 'Instruments Sample',
    },
    detailsPanelState,
    onDetailsPanelStateChange,
  );
}

// export function createProcessInstrumentsSamplesCallstackSlicesTrack(
//   trace: Trace,
//   uri: string,
//   tableName: string,
//   upid: number,
//   detailsPanelState: FlamegraphState | undefined,
//   onDetailsPanelStateChange: (state: FlamegraphState) => void,
// ) {
//   return createCallstackSlicesTrack(
//     trace,
//     uri,
//     {
//       dataset: new SourceDataset({
//         schema: {
//           id: NUM,
//           ts: LONG,
//           callsiteId: NUM,
//         },
//         src: `
//           SELECT
//             p.id,
//             ts,
//             callsite_id AS callsiteId,
//             upid
//           FROM instruments_sample p
//           JOIN thread USING (utid)
//           WHERE callsite_id IS NOT NULL
//           ORDER BY ts
//         `,
//         filter: {
//           col: 'upid',
//           eq: upid,
//         },
//       }),
//       callsiteQuery: (ts) => `
//         SELECT p.callsite_id
//         FROM instruments_sample p
//         JOIN thread t USING (utid)
//         WHERE p.ts = ${ts}
//           AND t.upid = ${upid}
//       `,
//       sqlModule: 'appleos.instruments.samples',
//       metricName: 'Instruments Samples',
//       panelTitle: 'Instruments Samples',
//       sliceName: 'Instruments Sample',
//     },
//     tableName,
//     detailsPanelState,
//     onDetailsPanelStateChange,
//   );
// }
//
// export function createThreadInstrumentsSamplesCallstackSlicesTrack(
//   trace: Trace,
//   uri: string,
//   tableName: string,
//   utid: number,
//   detailsPanelState: FlamegraphState | undefined,
//   onDetailsPanelStateChange: (state: FlamegraphState) => void,
// ) {
//   return createCallstackSlicesTrack(
//     trace,
//     uri,
//     {
//       dataset: new SourceDataset({
//         schema: {
//           id: NUM,
//           ts: LONG,
//           callsiteId: NUM,
//         },
//         src: `
//           SELECT
//             p.id,
//             ts,
//             callsite_id AS callsiteId,
//             utid
//           FROM instruments_sample p
//           WHERE callsite_id IS NOT NULL
//           ORDER BY ts
//         `,
//         filter: {
//           col: 'utid',
//           eq: utid,
//         },
//       }),
//       callsiteQuery: (ts) => `
//         SELECT p.callsite_id
//         FROM instruments_sample p
//         WHERE p.ts = ${ts}
//           AND p.utid = ${utid}
//       `,
//       sqlModule: 'appleos.instruments.samples',
//       metricName: 'Instruments Samples',
//       panelTitle: 'Instruments Samples',
//       sliceName: 'Instruments Sample',
//     },
//     tableName,
//     detailsPanelState,
//     onDetailsPanelStateChange,
//   );
// }

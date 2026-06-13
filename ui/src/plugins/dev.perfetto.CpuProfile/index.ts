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

import m from 'mithril';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {
  createCpuProfileTrack,
  createCpuProfileSlicesTrack,
} from './cpu_profile_track';
import {getThreadUriPrefix} from '../../public/utils';
import {exists} from '../../base/utils';
import {TrackNode} from '../../public/workspace';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {type AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {
  metricsFromTableOrSubquery,
  type QueryFlamegraphMetric,
} from '../../components/query_flamegraph';
import {FlamegraphPanel} from '../../components/flamegraph_panel';
import {Flamegraph, FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import {assertExists} from '../../base/assert';
import type {Store} from '../../base/store';
import {z} from 'zod';

const CPU_PROFILE_TRACK_KIND = 'CpuProfileTrack';

const CPU_PROFILE_PLUGIN_STATE_SCHEMA = z
  .object({
    areaSelectionFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
    detailsPanelFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  })
  .readonly();

type CpuProfilePluginState = z.infer<typeof CPU_PROFILE_PLUGIN_STATE_SCHEMA>;

export default class CpuProfilePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CpuProfile';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  private store?: Store<CpuProfilePluginState>;

  private migrateCpuProfilePluginState(init: unknown): CpuProfilePluginState {
    const result = CPU_PROFILE_PLUGIN_STATE_SCHEMA.safeParse(init);
    return result.data ?? {};
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.store = ctx.mountStore(CpuProfilePlugin.id, (init) =>
      this.migrateCpuProfilePluginState(init),
    );
    await ctx.engine.query('INCLUDE PERFETTO MODULE callstacks.stack_profile;');
    const result = await ctx.engine.query(`
      with thread_cpu_sample as (
        select distinct utid
        from cpu_profile_stack_sample
      )
      select
        utid,
        tid,
        upid,
        thread.name as threadName
      from thread_cpu_sample
      join thread using(utid)
      where not is_idle
    `);

    const store = assertExists(this.store);
    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const upid = it.upid;
      const threadName = it.threadName;
      const uri = `${getThreadUriPrefix(upid, utid)}_cpu_samples`;
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kinds: [CPU_PROFILE_TRACK_KIND],
          utid,
          ...(exists(upid) && {upid}),
        },
        renderer: createCpuProfileTrack(
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
      const slicesUri = `${uri}_slices`;
      const tableName = `slices_${slicesUri.replace(/[^a-zA-Z0-9]/g, '_')}`;
      await ctx.engine.query(`
        CREATE PERFETTO TABLE ${tableName} AS
        WITH samples AS (
          SELECT
            id AS sample_id,
            ts,
            LEAD(ts, 1, (SELECT end_ts FROM trace_bounds)) OVER (ORDER BY ts) - ts AS dur,
            callsite_id
          FROM cpu_profile_stack_sample
          WHERE utid = ${utid} AND callsite_id IS NOT NULL
        ),
        callstack_path AS (
          SELECT
            id AS callsite_id,
            id AS current_callsite_id,
            parent_id,
            frame_id,
            0 AS depth
          FROM stack_profile_callsite
          WHERE id IN (SELECT DISTINCT callsite_id FROM samples)

          UNION ALL

          SELECT
            p.callsite_id,
            c.id AS current_callsite_id,
            c.parent_id,
            c.frame_id,
            p.depth + 1 AS depth
          FROM callstack_path p
          JOIN stack_profile_callsite c ON p.parent_id = c.id
        ),
        path_with_max_depth AS (
          SELECT
            callsite_id,
            frame_id,
            depth,
            MAX(depth) OVER (PARTITION BY callsite_id) AS max_depth
          FROM callstack_path
        ),
        raw_slices AS (
          SELECT
            s.ts,
            s.dur,
            f.name,
            (p.max_depth - p.depth) AS depth,
            s.callsite_id AS callsiteId
          FROM samples s
          JOIN path_with_max_depth p USING (callsite_id)
          JOIN stack_profile_frame f ON p.frame_id = f.id
        ),
        islands AS (
          SELECT
            ts,
            dur,
            name,
            depth,
            callsiteId,
            CASE
              WHEN LAG(ts + dur) OVER (PARTITION BY depth, name ORDER BY ts) >= ts THEN 0
              ELSE 1
            END AS is_new_island
          FROM raw_slices
        ),
        island_ids AS (
          SELECT
            ts,
            dur,
            name,
            depth,
            callsiteId,
            SUM(is_new_island) OVER (PARTITION BY depth, name ORDER BY ts) AS island_id
          FROM islands
        )
        SELECT
          ROW_NUMBER() OVER (ORDER BY ts) AS id,
          ts,
          dur,
          name,
          depth,
          callsiteId
        FROM (
          SELECT
            MIN(ts) AS ts,
            MAX(ts + dur) - MIN(ts) AS dur,
            name,
            depth,
            MIN(callsiteId) AS callsiteId
          FROM island_ids
          GROUP BY depth, name, island_id
        )
      `);
      await ctx.engine.query(
        `CREATE PERFETTO INDEX ${tableName}_id ON ${tableName}(id);`,
      );
      ctx.tracks.registerTrack({
        uri: slicesUri,
        tags: {
          kinds: [CPU_PROFILE_TRACK_KIND],
          utid,
          ...(exists(upid) && {upid}),
        },
        renderer: await createCpuProfileSlicesTrack(
          ctx,
          slicesUri,
          tableName,
          utid,
          store.state.detailsPanelFlamegraphState,
          (state) => {
            store.edit((draft) => {
              draft.detailsPanelFlamegraphState = state;
            });
          },
        ),
      });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForThread(utid);
      const track = new TrackNode({
        uri,
        name: `${threadName} (CPU Stack Samples)`,
        sortOrder: -40,
      });
      group?.addChildInOrder(track);
      const slicesTrack = new TrackNode({
        uri: slicesUri,
        name: `${threadName} (CPU Callstack Slices)`,
        sortOrder: -39,
      });
      group?.addChildInOrder(slicesTrack);
    }

    ctx.selection.registerAreaSelectionTab(this.createAreaSelectionTab(ctx));

    ctx.onTraceReady.addListener(async () => {
      await selectCpuProfileCallsite(ctx);
    });
  }

  private createAreaSelectionTab(trace: Trace) {
    let previousSelection: AreaSelection | undefined;
    let flamegraphMetrics: ReadonlyArray<QueryFlamegraphMetric> | undefined;

    return {
      id: 'cpu_profile_flamegraph',
      name: 'CPU Profile Sample Flamegraph',
      render: (selection: AreaSelection) => {
        const changed =
          previousSelection === undefined ||
          !areaSelectionsEqual(previousSelection, selection);
        if (changed) {
          flamegraphMetrics = this.computeCpuProfileFlamegraph(selection);
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

  private computeCpuProfileFlamegraph(
    selection: AreaSelection,
  ): ReadonlyArray<QueryFlamegraphMetric> | undefined {
    const utids = [];
    for (const trackInfo of selection.tracks) {
      if (trackInfo?.tags?.kinds?.includes(CPU_PROFILE_TRACK_KIND)) {
        utids.push(trackInfo.tags?.utid);
      }
    }
    if (utids.length === 0) {
      return undefined;
    }
    const metrics = metricsFromTableOrSubquery({
      tableOrSubquery: `
      (
        select
          id,
          parent_id as parentId,
          name,
          mapping_name,
          source_file || ':' || line_number as source_location,
          self_count
        from _callstacks_for_callsites!((
          select p.callsite_id
          from cpu_profile_stack_sample p
          where p.ts >= ${selection.start}
            and p.ts <= ${selection.end}
            and p.utid in (${utids.join(',')})
        ))
      )
    `,
      tableMetrics: [
        {
          name: 'CPU Profile Samples',
          unit: '',
          columnName: 'self_count',
        },
      ],
      dependencySql: 'include perfetto module callstacks.stack_profile',
      unaggregatableProperties: [
        {name: 'mapping_name', displayName: 'Mapping'},
      ],
      aggregatableProperties: [
        {
          name: 'source_location',
          displayName: 'Source Location',
          mergeAggregation: 'ONE_OR_SUMMARY',
        },
      ],
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

async function selectCpuProfileCallsite(trace: Trace) {
  const profile = await assertExists(trace.engine).query(`
    select utid, upid
    from cpu_profile_stack_sample
    join thread using(utid)
    where callsite_id is not null and not is_idle
    order by ts desc
    limit 1
  `);
  if (profile.numRows() !== 1) return;
  const {utid, upid} = profile.firstRow({utid: NUM, upid: NUM_NULL});

  // Create an area selection over the first process with a perf samples track
  trace.selection.selectArea({
    start: trace.traceInfo.start,
    end: trace.traceInfo.end,
    trackUris: [`${getThreadUriPrefix(upid, utid)}_cpu_samples`],
  });
}

// Copyright (C) 2023 The Android Open Source Project
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
import {Trace} from '../../public/trace';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {PerfettoPlugin} from '../../public/plugin';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {TrackNode} from '../../public/workspace';
import {STR, LONG, LONG_NULL} from '../../trace_processor/query_result';
import {SourceDataset} from '../../trace_processor/dataset';
import {AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {Flamegraph, FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
  QueryFlamegraphWithMetrics,
} from '../../components/query_flamegraph';
import SupportPlugin from '../com.android.AndroidLongBatterySupport';
import {Store} from '../../base/store';
import {z} from 'zod';
import {assertExists} from '../../base/logging';

const DAY_EXPLORER_TRACK_KIND = 'day_explorer_counter_track';

const DAY_EXPLORER_PLUGIN_STATE_SCHEMA = z.object({
  areaSelectionFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
});

type DayExplorerPluginState = z.infer<typeof DAY_EXPLORER_PLUGIN_STATE_SCHEMA>;

export default class DayExplorerPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.DayExplorer';
  static readonly dependencies = [StandardGroupsPlugin, SupportPlugin];

  private store?: Store<DayExplorerPluginState>;

  private migrateDayExplorerPluginState(init: unknown): DayExplorerPluginState {
    const result = DAY_EXPLORER_PLUGIN_STATE_SCHEMA.safeParse(init);
    return result.data ?? {};
  }

  private support(ctx: Trace) {
    return ctx.plugins.getPlugin(SupportPlugin);
  }

  async addDayExplorerCounters(
    ctx: Trace,
    support: SupportPlugin,
    groupName: string,
    limit: number,
  ): Promise<void> {
    await ctx.engine.query(
      `INCLUDE PERFETTO MODULE
          google3.wireless.android.telemetry.trace_extractor.modules.day_explorer.perfetto_ui_blames`,
    );

    const group = support.getOrCreateGroup(ctx, groupName);
    await this.addDayExplorerRecursive(ctx, group, limit, -1n);
  }

  private async addDayExplorerRecursive(
    ctx: Trace,
    parent: TrackNode,
    limit: number,
    parentId: bigint,
  ): Promise<void> {
    const children = await ctx.engine.query(`
      SELECT track_id, display_name, cast(round(total_energy_uws / 3600000) as int) as energy_mwh
      FROM day_explorer_ui_hierarchy
      WHERE (${parentId} >= 0 AND parent_id = ${parentId})
         OR (${parentId} < 0 AND parent_id IS NULL)
      ORDER BY energy_mwh DESC
      LIMIT ${limit}
    `);

    const childIter = children.iter({
      track_id: LONG,
      display_name: STR,
      energy_mwh: LONG,
    });

    for (; childIter.valid(); childIter.next()) {
      const query = `
        SELECT ts, power_mw AS value
        FROM day_explorer_ui_hierarchy_per_ts
        WHERE track_id = ${childIter.track_id}
      `;
      const groupKey = `_day_explorer_ui_hierarchy_under_${parentId}`;
      const trackName = `${childIter.display_name} - ${childIter.energy_mwh}mWh`;
      const node = await this.createDayExplorerTrack(
        ctx,
        trackName,
        groupKey,
        query,
      );
      parent.addChildInOrder(node);
      await this.addDayExplorerRecursive(ctx, node, limit, childIter.track_id);
    }
  }

  private async createDayExplorerTrack(
    ctx: Trace,
    name: string,
    groupKey: string,
    query: string,
  ): Promise<TrackNode> {
    const uri = `/day_explorer_${uuidv4()}`;
    const renderer = await createQueryCounterTrack({
      trace: ctx,
      uri,
      data: {
        sqlSource: query,
      },
      columns: {
        ts: 'ts',
        value: 'value',
      },
      options: {
        yRangeSharingKey: groupKey,
      },
    });

    ctx.tracks.registerTrack({
      uri,
      renderer,
      tags: {
        kinds: [DAY_EXPLORER_TRACK_KIND],
      },
    });

    return new TrackNode({
      name,
      uri,
    });
  }

  private createDayExplorerFlameGraphPanel(trace: Trace) {
    let previousSelection: AreaSelection | undefined;
    let flameagraphWithMetrics: QueryFlamegraphWithMetrics | undefined;
    return {
      id: 'day_explorer_flamegraph_selection',
      name: 'Day Explorer Flamegraph',
      render: (selection: AreaSelection) => {
        const selectionChanged =
          previousSelection === undefined ||
          !areaSelectionsEqual(previousSelection, selection);
        previousSelection = selection;
        if (selectionChanged) {
          flameagraphWithMetrics = this.computeDayExplorerFlameGraph(
            trace,
            selection,
          );
        }
        if (flameagraphWithMetrics === undefined) {
          return undefined;
        }
        const store = assertExists(this.store);
        const {flamegraph, metrics} = flameagraphWithMetrics;
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

  private computeDayExplorerFlameGraph(
    trace: Trace,
    currentSelection: AreaSelection,
  ): QueryFlamegraphWithMetrics | undefined {
    // The flame graph will be shown when any day explorer track is in the area
    // selection. The selection is used to filter by time, but not by track. All
    // day explorer tracks are considered for the graph.
    let hasDayExplorer = false;
    for (const trackInfo of currentSelection.tracks) {
      if (trackInfo?.tags?.kinds?.includes(DAY_EXPLORER_TRACK_KIND)) {
        hasDayExplorer = true;
        break;
      }
    }
    if (!hasDayExplorer) {
      return undefined;
    }
    const metrics = metricsFromTableOrSubquery(
      `
        (
          WITH
            total_energy AS (
              SELECT track_id, parent_id, display_name, SUM(energy_uws) AS energy_uws
              FROM day_explorer_ui_hierarchy_per_ts
              WHERE ts >= ${currentSelection.start}
                AND ts <= ${currentSelection.end}
              GROUP BY 1, 2, 3
            ),
            with_child AS (
              SELECT
                *,
                (
                  SELECT IFNULL(SUM(energy_uws), 0)
                  FROM total_energy
                  WHERE parent_id = P.track_id
                ) AS child_energy
              FROM total_energy AS P
            )
          SELECT
            track_id AS id,
            parent_id AS parentId,
            display_name AS name,
            cast(round((energy_uws - child_energy) / 1000) as int) AS self_count
          FROM with_child
        )
      `,
      [
        {
          name: 'Energy mWs',
          unit: '',
          columnName: 'self_count',
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

  async addDayExplorerUsage(
    ctx: Trace,
    support: SupportPlugin,
    groupName: string,
  ): Promise<void> {
    const e = ctx.engine;

    await e.query(
      `INCLUDE PERFETTO MODULE
          google3.wireless.android.telemetry.trace_extractor.modules.day_explorer.perfetto_ui_blames`,
    );

    await support.addSliceTrack(
      ctx,
      'Day Explorer Device Usage',
      new SourceDataset({
        src: `
          SELECT
            ts,
            dur,
            usage as name
          FROM day_explorer_device_usage
        `,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
      groupName,
      false,
    );
  }

  async addDayExplorerCommand(
    ctx: Trace,
    support: SupportPlugin,
    features: Set<string>,
  ): Promise<void> {
    if (features.has('google3')) {
      ctx.commands.registerCommand({
        id: 'com.android.DayExplorerBlamesByCategory',
        name: 'Add tracks: Day Explorer',
        callback: async () => {
          const limitStr = await ctx.omnibox.prompt(
            'Maximum results per group',
          );
          const limit = Number(limitStr);
          if (!isFinite(limit) || limit <= 0) {
            alert('Positive number required');
            return;
          }
          await this.addDayExplorerUsage(ctx, support, 'Day Explorer');
          await this.addDayExplorerCounters(
            ctx,
            support,
            'Day Explorer',
            limit,
          );
        },
      });
    }
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.store = ctx.mountStore(DayExplorerPlugin.id, (init) =>
      this.migrateDayExplorerPluginState(init),
    );

    const support = this.support(ctx);
    const features = await support.features(ctx.engine);

    ctx.selection.registerAreaSelectionTab(
      this.createDayExplorerFlameGraphPanel(ctx),
    );

    if (features.has('google3')) {
      await this.addDayExplorerCommand(ctx, support, features);
    }
  }
}

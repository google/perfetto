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

import m from 'mithril';
import {uuidv4} from '../../base/uuid';
import type {Trace} from '../../public/trace';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import type {PerfettoPlugin} from '../../public/plugin';
import {CounterTrack} from '../../components/tracks/counter_track';
import {TrackNode} from '../../public/workspace';
import {STR, LONG, LONG_NULL} from '../../trace_processor/query_result';
import {SourceDataset} from '../../trace_processor/dataset';
import {type AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {
  TREE_EXPLORER_STATE_SCHEMA,
  updateTreeExplorerState,
} from '../../widgets/tree_explorer';
import {
  metricsFromTableOrSubquery,
  type TreeExplorerQueryMetric,
} from '../../components/tree_explorer_fetcher';
import {TreeExplorerPanel} from '../../components/tree_explorer_panel';
import SupportPlugin from '../com.android.AndroidLongBatterySupport';
import type {Store} from '../../base/store';
import {z} from 'zod';
import {ensureExists} from '../../base/assert';

const DAY_EXPLORER_TRACK_KIND = 'day_explorer_counter_track';

const DAY_EXPLORER_PLUGIN_STATE_SCHEMA = z.object({
  areaSelectionFlamegraphState: TREE_EXPLORER_STATE_SCHEMA.optional(),
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
        childIter.track_id,
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
    trackId: bigint,
  ): Promise<TrackNode> {
    const uri = `/day_explorer_${uuidv4()}`;
    const renderer = await CounterTrack.createMaterialized({
      trace: ctx,
      uri,
      sqlSource: query,
      yRangeSharingKey: groupKey,
    });

    ctx.tracks.registerTrack({
      uri,
      renderer,
      tags: {
        kinds: [DAY_EXPLORER_TRACK_KIND],
        trackId: Number(trackId),
      },
    });

    return new TrackNode({
      name,
      uri,
    });
  }

  private createDayExplorerFlameGraphPanel(trace: Trace) {
    let previousSelection: AreaSelection | undefined;
    let flamegraphMetrics: ReadonlyArray<TreeExplorerQueryMetric> | undefined;
    return {
      id: 'day_explorer_flamegraph_selection',
      name: 'Day Explorer Flamegraph',
      render: (selection: AreaSelection) => {
        const selectionChanged =
          previousSelection === undefined ||
          !areaSelectionsEqual(previousSelection, selection);
        previousSelection = selection;
        if (selectionChanged) {
          flamegraphMetrics = this.computeDayExplorerFlameGraph(selection);
        }
        if (flamegraphMetrics === undefined) {
          return undefined;
        }
        const store = ensureExists(this.store);
        return {
          isLoading: false,
          content: m(TreeExplorerPanel, {
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

  private computeDayExplorerFlameGraph(
    currentSelection: AreaSelection,
  ): ReadonlyArray<TreeExplorerQueryMetric> | undefined {
    // The flame graph will be shown when any day explorer track is in the area
    // selection. The selection is used to filter by time, and we filter the graph
    // to only include energy from the selected tracks and their recursive descendants.
    // If a physical track is selected, we exclude label roots to avoid double-counting.
    const selectedTrackIds: bigint[] = [];

    for (const trackInfo of currentSelection.tracks) {
      if (
        trackInfo?.tags?.kinds?.includes(DAY_EXPLORER_TRACK_KIND) &&
        trackInfo.tags.trackId !== undefined
      ) {
        const trackId = trackInfo.tags.trackId;
        if (typeof trackId === 'string' || typeof trackId === 'number') {
          selectedTrackIds.push(BigInt(trackId));
        }
      }
    }

    if (selectedTrackIds.length === 0) {
      return undefined;
    }

    const metrics = metricsFromTableOrSubquery({
      tableOrSubquery: `
        (
          WITH RECURSIVE
            selected_roots(track_id) AS (
              SELECT column1 FROM (VALUES ${selectedTrackIds.map((id) => `(${id})`).join(',')})
            ),
            descendants(track_id) AS (
              SELECT track_id FROM selected_roots
              UNION
              SELECT child.track_id
              FROM day_explorer_ui_hierarchy child
              JOIN descendants parent ON child.parent_id = parent.track_id
            ),
            total_energy AS (
              SELECT track_id, parent_id, display_name, SUM(energy_uws) AS energy_uws
              FROM day_explorer_ui_hierarchy_per_ts
              WHERE ts >= ${currentSelection.start}
                AND ts <= ${currentSelection.end}
                AND track_id IN (SELECT track_id FROM descendants)
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
            CASE WHEN parent_id IN (SELECT track_id FROM total_energy) THEN parent_id ELSE NULL END AS parentId,
            display_name AS name,
            cast(round((energy_uws - child_energy) / 1000) as int) AS self_count
          FROM with_child
        )
      `,
      tableMetrics: [
        {
          name: 'Energy',
          unit: 'mWs',
          columnName: 'self_count',
        },
      ],
      nameColumnLabel: 'Component',
    });
    const store = ensureExists(this.store);
    store.edit((draft) => {
      draft.areaSelectionFlamegraphState = updateTreeExplorerState(
        draft.areaSelectionFlamegraphState,
        metrics,
      );
    });
    return metrics;
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

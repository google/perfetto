// Copyright (C) 2026 The Android Open Source Project
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

import {sqliteString} from '../../base/string_utils';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode, type TrackNodeArgs} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import TrackEventPlugin from '../dev.perfetto.TrackEvent';
import {
  DeviceStateDetailsPanel,
  DeviceTraceDetailsPanel,
} from './details_panels';
import {
  type BreakdownColumn,
  buildDatasetSpec,
  DEVICE_STATE_SPEC,
  DEVICE_TRACE_BASE_SPEC,
  type DeviceStateTrackInfo,
  type DeviceTraceDatasetSpec,
  EXCLUDED_BREAKDOWN_ARGS,
  formatEnergyMwh,
  type PowerRailTrackInfo,
  selectColumnList,
  TABLE_NAME,
} from './schema';
import {
  createMultiTraceTimelineTab,
  MultiTraceTimelineSelectionAggregator,
} from './selection_tab';

const DEFAULT_BREAKDOWN_PATHS: ReadonlyArray<ReadonlyArray<string>> = [
  ['session_name', 'subscription_name', 'android_id'],
  ['android_id', 'session_name', 'subscription_name'],
];

const DEVICE_STATE_TRACK_ORDER: ReadonlyArray<string> = [
  'Screen state',
  'Charging',
  'Suspend / resume',
  'Doze light state',
  'Doze deep state',
  'Top app',
  'Default network',
  'Cellular connection',
  'NR 5G state',
  'Cellular strength',
  'Wifi supplicant state',
  'Wifi strength',
  'Thermal throttling',
  'Power save',
  'Folded state',
];

function createLazyTrackNode(
  ctx: Trace,
  args: Partial<TrackNodeArgs>,
  populate?: (node: TrackNode) => Promise<void> | void,
): TrackNode {
  if (populate === undefined) {
    return new TrackNode(args);
  }
  const placeholder = new TrackNode({name: 'Loading...'});
  let populated = false;
  const node = new TrackNode({
    ...args,
    onExpand: () => {
      if (populated) return;
      populated = true;
      const res = populate(node);
      if (res instanceof Promise) {
        void res.finally(() => {
          placeholder.remove();
          ctx.raf.scheduleFullRedraw();
        });
      } else {
        placeholder.remove();
      }
    },
  });
  node.addChildLast(placeholder);
  return node;
}

export default class MultiTraceTimelinePlugin implements PerfettoPlugin {
  static readonly id = 'com.android.MultiTraceTimeline';
  static readonly description =
    'Visualizes multi-trace intervals, concurrency breakdowns, power rails, and device states across devices and sessions.';
  static readonly dependencies = [TrackEventPlugin];

  private readonly counterTrackFilters = new Map<string, string>();
  private readonly batterySessionNames = new Set<string>();
  private breakdownColumns: BreakdownColumn[] = [];
  private datasetSpec: DeviceTraceDatasetSpec = DEVICE_TRACE_BASE_SPEC;
  private treeCount = 0;
  private fleetTotalTrack?: PowerRailTrackInfo;
  private fleetSubTracks: PowerRailTrackInfo[] = [];
  private readonly userTotalTracks = new Map<string, PowerRailTrackInfo>();
  private readonly userSubTracks = new Map<string, PowerRailTrackInfo[]>();
  private readonly userRailTracks = new Map<string, PowerRailTrackInfo[]>();
  private readonly userStateTracks = new Map<string, DeviceStateTrackInfo[]>();

  async onTraceLoad(ctx: Trace): Promise<void> {
    const check = await ctx.engine.query(`
      SELECT COUNT(*) AS cnt
      FROM slice
      WHERE category = 'device_trace'
      LIMIT 1
    `);
    if (check.firstRow({cnt: NUM}).cnt === 0) {
      return;
    }

    const argRes = await ctx.engine.query(`
      SELECT DISTINCT SUBSTR(a.key, 7) AS arg_name
      FROM (
        SELECT arg_set_id
        FROM slice
        WHERE category = 'device_trace'
        LIMIT 1000
      ) s
      JOIN args a USING (arg_set_id)
      WHERE a.key GLOB 'debug.*'
      ORDER BY arg_name
    `);
    const extraArgSelects: string[] = [];
    this.breakdownColumns = [];
    const argIt = argRes.iter({arg_name: STR});
    for (; argIt.valid(); argIt.next()) {
      const argName = argIt.arg_name;
      if (
        !EXCLUDED_BREAKDOWN_ARGS.has(argName) &&
        /^[a-zA-Z_][a-zA-Z0-9_]*$/.test(argName)
      ) {
        const sqlCol = `col_${argName}`;
        extraArgSelects.push(
          `,\n        COALESCE(CAST(EXTRACT_ARG(arg_set_id, 'debug.${argName}') AS TEXT), '') AS ${sqlCol}`,
        );
        this.breakdownColumns.push({label: argName, sqlCol});
      }
    }
    this.datasetSpec = buildDatasetSpec(this.breakdownColumns);

    const colByLabel = new Map(this.breakdownColumns.map((c) => [c.label, c]));
    const defaultTrees: BreakdownColumn[][] = [];
    const seenPaths = new Set<string>();
    for (const pathLabels of DEFAULT_BREAKDOWN_PATHS) {
      const cols = pathLabels
        .map((label) => colByLabel.get(label))
        .filter((c): c is BreakdownColumn => c !== undefined);
      const key = cols.map((c) => c.label).join('->');
      if (cols.length > 0 && !seenPaths.has(key)) {
        seenPaths.add(key);
        defaultTrees.push(cols);
      }
    }

    const indexStatements = defaultTrees
      .map(
        (cols, idx) =>
          `CREATE PERFETTO INDEX _dt_idx_${idx} ON ${TABLE_NAME}(${cols.map((c) => c.sqlCol).join(', ')});`,
      )
      .join('\n      ');

    await ctx.engine.query(`
      CREATE OR REPLACE PERFETTO TABLE ${TABLE_NAME} AS
      SELECT
        id,
        ts,
        COALESCE(
          CAST(EXTRACT_ARG(arg_set_id, 'debug.dur_ns') AS INT),
          IIF(dur > 0, dur, 1)
        ) AS dur,
        COALESCE(
          CAST(EXTRACT_ARG(arg_set_id, 'debug.armed_start_ts') AS INT),
          ts
        ) AS armed_start_ts,
        COALESCE(
          CAST(EXTRACT_ARG(arg_set_id, 'debug.armed_dur_ns') AS INT),
          CAST(EXTRACT_ARG(arg_set_id, 'debug.dur_ns') AS INT),
          IIF(dur > 0, dur, 1)
        ) AS armed_dur,
        name AS slice_label,
        COALESCE(EXTRACT_ARG(arg_set_id, 'debug.trace_uuid'), '') AS trace_uuid,
        COALESCE(CAST(EXTRACT_ARG(arg_set_id, 'debug.trace_size_bytes') AS INT), 0) AS trace_size_bytes,
        COALESCE(CAST(EXTRACT_ARG(arg_set_id, 'debug.original_trace_size_bytes') AS INT), 0) AS original_trace_size_bytes,
        COALESCE(CAST(EXTRACT_ARG(arg_set_id, 'debug.trace_dur_ns') AS INT), 0) AS trace_dur,
        COALESCE(CAST(EXTRACT_ARG(arg_set_id, 'debug.sched_dur_ns') AS INT), 0) AS sched_dur${extraArgSelects.join('')}
      FROM slice
      WHERE category = 'device_trace';
      ${indexStatements}
    `);

    this.batterySessionNames.clear();
    await this.loadPowerRailTracks(ctx);
    await this.loadDeviceStateTracks(ctx);
    await this.hideInternalTrackEventTracks(ctx);

    for (const cols of defaultTrees) {
      ctx.defaultWorkspace.addChildInOrder(
        this.createBreakdownTree(ctx, cols, false),
      );
    }

    ctx.selection.registerAreaSelectionTab(
      createMultiTraceTimelineTab(
        ctx,
        new MultiTraceTimelineSelectionAggregator(
          ctx,
          this.counterTrackFilters,
          this.breakdownColumns,
          this.datasetSpec,
        ),
      ),
    );

    ctx.commands.registerCommand({
      id: 'com.android.MultiTraceTimeline.AddCustomBreakdown',
      name: 'Multi-Trace Timeline: Add custom breakdown...',
      callback: async () => {
        await this.promptAndAddCustomBreakdown(ctx);
      },
    });
  }

  private async loadPowerRailTracks(ctx: Trace): Promise<void> {
    this.fleetTotalTrack = undefined;
    this.fleetSubTracks = [];
    this.userTotalTracks.clear();
    this.userSubTracks.clear();
    this.userRailTracks.clear();

    const res = await ctx.engine.query(`
      SELECT
        t.id AS track_id,
        COALESCE(EXTRACT_ARG(t.source_arg_set_id, 'description'), '') AS description
      FROM track AS t
      WHERE t.type GLOB '*counter*'
        AND EXTRACT_ARG(t.source_arg_set_id, 'description') GLOB 'dt_power|*'
    `);

    const it = res.iter({track_id: NUM, description: STR});
    for (; it.valid(); it.next()) {
      const parts = it.description.split('|');
      if (parts.length < 6 || parts[0] !== 'dt_power') continue;
      const kind = parts[1];
      const androidId = parts[2] ?? '';
      const subsystemName = parts[3] ?? '';
      const railName = parts[4] ?? '';
      const totalMwh = Number(parts[5]) || 0;
      const sessionName = parts[6]?.trim() ?? '';
      if (
        kind !== 'fleet_total' &&
        kind !== 'fleet_sub' &&
        kind !== 'user_total' &&
        kind !== 'user_sub' &&
        kind !== 'user_rail'
      ) {
        continue;
      }
      if (sessionName !== '') {
        this.batterySessionNames.add(sessionName);
      }
      const info: PowerRailTrackInfo = {
        trackId: it.track_id,
        kind,
        androidId,
        subsystemName,
        railName,
        totalMwh,
      };
      switch (kind) {
        case 'fleet_total':
          this.fleetTotalTrack = info;
          break;
        case 'fleet_sub':
          this.fleetSubTracks.push(info);
          break;
        case 'user_total':
          this.userTotalTracks.set(androidId, info);
          break;
        case 'user_sub': {
          let list = this.userSubTracks.get(androidId);
          if (!list) {
            list = [];
            this.userSubTracks.set(androidId, list);
          }
          list.push(info);
          break;
        }
        case 'user_rail': {
          let list = this.userRailTracks.get(androidId);
          if (!list) {
            list = [];
            this.userRailTracks.set(androidId, list);
          }
          list.push(info);
          break;
        }
      }
    }

    this.fleetSubTracks.sort((a, b) => b.totalMwh - a.totalMwh);
    for (const list of this.userSubTracks.values()) {
      list.sort((a, b) => b.totalMwh - a.totalMwh);
    }
    for (const list of this.userRailTracks.values()) {
      list.sort((a, b) => b.totalMwh - a.totalMwh);
    }
  }

  private async loadDeviceStateTracks(ctx: Trace): Promise<void> {
    this.userStateTracks.clear();
    const orderRank = new Map<string, number>(
      DEVICE_STATE_TRACK_ORDER.map((name, idx) => [name, idx]),
    );

    const res = await ctx.engine.query(`
      SELECT
        t.id AS track_id,
        COALESCE(EXTRACT_ARG(t.source_arg_set_id, 'description'), '') AS description
      FROM track AS t
      WHERE EXTRACT_ARG(t.source_arg_set_id, 'description') GLOB 'dt_state|*'
    `);

    const it = res.iter({track_id: NUM, description: STR});
    for (; it.valid(); it.next()) {
      const parts = it.description.split('|');
      if (parts.length < 4 || parts[0] !== 'dt_state') continue;
      const androidId = parts[1];
      const stateTrack = parts[2];
      const sliceCount = Number(parts[3]) || 0;
      const sessionName = parts[4]?.trim() ?? '';
      if (!androidId || !stateTrack || sliceCount <= 0) continue;
      if (sessionName !== '') {
        this.batterySessionNames.add(sessionName);
      }
      let list = this.userStateTracks.get(androidId);
      if (!list) {
        list = [];
        this.userStateTracks.set(androidId, list);
      }
      list.push({
        trackId: it.track_id,
        androidId,
        trackName: stateTrack,
        sliceCount,
      });
    }

    for (const list of this.userStateTracks.values()) {
      list.sort((a, b) => {
        const ra = orderRank.get(a.trackName) ?? 999;
        const rb = orderRank.get(b.trackName) ?? 999;
        return ra !== rb ? ra - rb : a.trackName.localeCompare(b.trackName);
      });
    }
  }

  private async hideInternalTrackEventTracks(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(`
      SELECT DISTINCT track_id
      FROM slice
      WHERE category = 'device_trace'
      UNION
      SELECT id AS track_id
      FROM track
      WHERE EXTRACT_ARG(source_arg_set_id, 'description') GLOB 'dt_*'
    `);
    const it = res.iter({track_id: NUM});
    for (; it.valid(); it.next()) {
      const node = ctx.defaultWorkspace.getTrackByUri(
        `/track_event_${it.track_id}`,
      );
      if (node !== undefined) {
        const parent = node.parent;
        node.remove();
        if (
          parent !== undefined &&
          parent.name === 'Global Track Events' &&
          parent.children.length === 0
        ) {
          parent.remove();
        }
      }
    }
  }

  private async promptAndAddCustomBreakdown(ctx: Trace): Promise<void> {
    if (this.breakdownColumns.length === 0) {
      return;
    }
    const picked: BreakdownColumn[] = [];

    const first = await ctx.omnibox.prompt(
      'Breakdown level 1: choose column...',
      {
        values: this.breakdownColumns,
        getName: (c) => c.label,
      },
    );
    if (first === undefined) {
      return;
    }
    picked.push(first);

    while (picked.length < this.breakdownColumns.length) {
      const pathStr = picked.map((c) => c.label).join(' -> ');
      const remaining = this.breakdownColumns.filter(
        (c) => !picked.some((p) => p.sqlCol === c.sqlCol),
      );
      type StepChoice =
        | {readonly kind: 'create'}
        | {readonly kind: 'column'; readonly col: BreakdownColumn};
      const choices: StepChoice[] = [
        {kind: 'create'},
        ...remaining.map((col): StepChoice => ({kind: 'column', col})),
      ];

      const next = await ctx.omnibox.prompt(
        `Breakdown (${pathStr}): choose next column or Create...`,
        {
          values: choices,
          getName: (choice) =>
            choice.kind === 'create'
              ? `Create breakdown (${pathStr})`
              : choice.col.label,
        },
      );
      if (next === undefined) {
        return;
      }
      if (next.kind === 'create') {
        break;
      }
      picked.push(next.col);
    }

    ctx.defaultWorkspace.addChildInOrder(
      this.createBreakdownTree(ctx, picked, true),
    );
  }

  private createBreakdownTree(
    ctx: Trace,
    cols: ReadonlyArray<BreakdownColumn>,
    removable: boolean,
  ): TrackNode {
    const pathStr = cols.map((c) => c.label).join(' -> ');
    const keyPrefix = `tree_${++this.treeCount}`;
    const rootUri = `${MultiTraceTimelinePlugin.id}#${keyPrefix}_root`;
    this.registerCounterTrack(ctx, rootUri, '1 = 1');
    return this.createBreakdownNode(
      ctx,
      {
        name: `Multi-Trace Timeline (${pathStr})`,
        uri: rootUri,
        removable,
      },
      cols,
      '1 = 1',
      keyPrefix,
      new Map(),
    );
  }

  private createBreakdownNode(
    ctx: Trace,
    nodeArgs: Partial<TrackNodeArgs>,
    remainingCols: ReadonlyArray<BreakdownColumn>,
    whereClause: string,
    keyPrefix: string,
    boundValues: ReadonlyMap<string, string>,
    userBatteryAttached = false,
    fleetBatteryAttached = false,
  ): TrackNode {
    const sessionVal = boundValues.get('session_name');
    const androidIdVal = boundValues.get('android_id');
    const isBatterySession =
      sessionVal !== undefined && this.batterySessionNames.has(sessionVal);
    const hasSessionColRemaining = remainingCols.some(
      (c) => c.label === 'session_name',
    );
    const hasUserBatteryTracks =
      !userBatteryAttached &&
      androidIdVal !== undefined &&
      (isBatterySession ||
        this.batterySessionNames.size === 0 ||
        (!boundValues.has('session_name') && !hasSessionColRemaining)) &&
      (this.userTotalTracks.has(androidIdVal) ||
        this.userSubTracks.has(androidIdVal) ||
        this.userRailTracks.has(androidIdVal) ||
        this.userStateTracks.has(androidIdVal));
    const hasFleetBatteryTracks =
      !fleetBatteryAttached &&
      ((isBatterySession && androidIdVal === undefined) ||
        (this.batterySessionNames.size === 0 && boundValues.size === 0)) &&
      (this.fleetTotalTrack !== undefined || this.fleetSubTracks.length > 0);

    if (
      remainingCols.length === 0 &&
      !hasUserBatteryTracks &&
      !hasFleetBatteryTracks
    ) {
      return new TrackNode(nodeArgs);
    }

    return createLazyTrackNode(ctx, nodeArgs, async (parentNode) => {
      const [col, ...restCols] = remainingCols;
      if (col !== undefined) {
        const isLeafLevel = restCols.length === 0;
        const res = await ctx.engine.query(`
          SELECT
            ${col.sqlCol} AS val,
            COUNT(*) AS trace_count,
            MAX(dur) AS max_dur
          FROM ${TABLE_NAME}
          WHERE ${whereClause}
          GROUP BY ${col.sqlCol}
          ORDER BY trace_count DESC, val ASC
        `);

        const it = res.iter({
          val: STR,
          trace_count: NUM,
          max_dur: LONG,
        });
        for (; it.valid(); it.next()) {
          const val = it.val;
          const count = it.trace_count;
          const displayVal = val || '(empty)';
          const childFilter = `${whereClause} AND ${col.sqlCol} = ${sqliteString(val)}`;
          const childKey = `${keyPrefix}:${col.label}:${val}`;
          const uri = `${MultiTraceTimelinePlugin.id}#${childKey}`;

          // Groups made only of instants have no concurrency to count, so show
          // them as slices at every level.
          if (
            isLeafLevel ||
            boundValues.has('android_id') ||
            it.max_dur === 0n
          ) {
            this.registerSliceTrack(ctx, uri, childFilter);
          } else {
            this.registerCounterTrack(ctx, uri, childFilter);
          }

          const childBound = new Map(boundValues);
          childBound.set(col.label, val);
          const childNode = this.createBreakdownNode(
            ctx,
            {name: `${displayVal} (${count})`, uri},
            restCols,
            childFilter,
            childKey,
            childBound,
            userBatteryAttached || hasUserBatteryTracks,
            fleetBatteryAttached || hasFleetBatteryTracks,
          );
          parentNode.addChildLast(childNode);
        }
      }

      if (hasFleetBatteryTracks) {
        this.appendFleetPowerRailsNode(
          ctx,
          parentNode,
          `${keyPrefix}:fleet_power`,
          whereClause,
        );
      }

      if (hasUserBatteryTracks && androidIdVal !== undefined) {
        this.appendUserPowerRailsNode(
          ctx,
          parentNode,
          androidIdVal,
          `${keyPrefix}:power_rails`,
          whereClause,
        );
        this.appendUserDeviceStatesNode(
          ctx,
          parentNode,
          androidIdVal,
          `${keyPrefix}:device_state`,
          whereClause,
        );
      }
    });
  }

  private appendFleetPowerRailsNode(
    ctx: Trace,
    parentNode: TrackNode,
    keyPrefix: string,
    selectionFilter: string,
  ): void {
    const fleetTotal = this.fleetTotalTrack;
    const fleetSubs = this.fleetSubTracks;
    if (fleetTotal === undefined && fleetSubs.length === 0) {
      return;
    }

    let groupUri: string | undefined;
    if (fleetTotal !== undefined) {
      groupUri = `${MultiTraceTimelinePlugin.id}#${keyPrefix}:total`;
      this.registerPowerCounterTrack(
        ctx,
        groupUri,
        fleetTotal.trackId,
        selectionFilter,
      );
    }

    const totalLabel =
      fleetTotal !== undefined
        ? `Power Rails — Fleet Total (${formatEnergyMwh(fleetTotal.totalMwh)})`
        : 'Power Rails — Fleet Subsystems (mW)';
    const groupNode = createLazyTrackNode(
      ctx,
      {
        name: totalLabel,
        uri: groupUri,
        isSummary: groupUri === undefined,
      },
      fleetSubs.length > 0
        ? (node) => {
            for (const sub of fleetSubs) {
              const subUri = `${MultiTraceTimelinePlugin.id}#${keyPrefix}:sub:${sub.subsystemName}`;
              this.registerPowerCounterTrack(
                ctx,
                subUri,
                sub.trackId,
                selectionFilter,
              );
              node.addChildLast(
                new TrackNode({
                  name: `${sub.subsystemName} (${formatEnergyMwh(sub.totalMwh)})`,
                  uri: subUri,
                }),
              );
            }
          }
        : undefined,
    );
    parentNode.addChildLast(groupNode);
  }

  private appendUserPowerRailsNode(
    ctx: Trace,
    parentNode: TrackNode,
    androidId: string,
    keyPrefix: string,
    selectionFilter: string,
  ): void {
    const userTotal = this.userTotalTracks.get(androidId);
    const userSubs = this.userSubTracks.get(androidId) ?? [];
    const userRails = this.userRailTracks.get(androidId) ?? [];
    if (
      userTotal === undefined &&
      userSubs.length === 0 &&
      userRails.length === 0
    ) {
      return;
    }

    let groupUri: string | undefined;
    if (userTotal !== undefined) {
      groupUri = `${MultiTraceTimelinePlugin.id}#${keyPrefix}:total`;
      this.registerPowerCounterTrack(
        ctx,
        groupUri,
        userTotal.trackId,
        selectionFilter,
      );
    }

    const groupTitle =
      userTotal !== undefined
        ? `Power Rails (mW) — Total ${formatEnergyMwh(userTotal.totalMwh)} (${userRails.length} rails)`
        : `Power Rails (mW) (${userRails.length} rails)`;

    const railsBySubsystem = new Map<string, PowerRailTrackInfo[]>();
    for (const rail of userRails) {
      let list = railsBySubsystem.get(rail.subsystemName);
      if (list === undefined) {
        list = [];
        railsBySubsystem.set(rail.subsystemName, list);
      }
      list.push(rail);
    }

    const hasChildren = userSubs.length > 0 || userRails.length > 0;
    const powerNode = createLazyTrackNode(
      ctx,
      {
        name: groupTitle,
        uri: groupUri,
        isSummary: groupUri === undefined,
      },
      hasChildren
        ? (node) => {
            for (const sub of userSubs) {
              const subUri = `${MultiTraceTimelinePlugin.id}#${keyPrefix}:sub:${sub.subsystemName}`;
              this.registerPowerCounterTrack(
                ctx,
                subUri,
                sub.trackId,
                selectionFilter,
              );
              const subRails = railsBySubsystem.get(sub.subsystemName) ?? [];
              const subNode = createLazyTrackNode(
                ctx,
                {
                  name: `Subsystem: ${sub.subsystemName} (${formatEnergyMwh(sub.totalMwh)}, ${subRails.length} rails)`,
                  uri: subUri,
                },
                subRails.length > 0
                  ? (subParent) => {
                      for (const rail of subRails) {
                        const railUri = `${MultiTraceTimelinePlugin.id}#${keyPrefix}:sub:${sub.subsystemName}:rail:${rail.railName}`;
                        this.registerPowerCounterTrack(
                          ctx,
                          railUri,
                          rail.trackId,
                          selectionFilter,
                        );
                        subParent.addChildLast(
                          new TrackNode({
                            name: `${rail.railName} (${formatEnergyMwh(rail.totalMwh)})`,
                            uri: railUri,
                          }),
                        );
                      }
                    }
                  : undefined,
              );
              node.addChildLast(subNode);
            }

            if (userRails.length > 0) {
              const allRailsNode = createLazyTrackNode(
                ctx,
                {
                  name: `All Individual Rails (${userRails.length})`,
                  isSummary: true,
                },
                (railsParent) => {
                  for (const rail of userRails) {
                    const railUri = `${MultiTraceTimelinePlugin.id}#${keyPrefix}:all_rails:${rail.railName}`;
                    this.registerPowerCounterTrack(
                      ctx,
                      railUri,
                      rail.trackId,
                      selectionFilter,
                    );
                    railsParent.addChildLast(
                      new TrackNode({
                        name: `${rail.railName} [${rail.subsystemName}] (${formatEnergyMwh(rail.totalMwh)})`,
                        uri: railUri,
                      }),
                    );
                  }
                },
              );
              node.addChildLast(allRailsNode);
            }
          }
        : undefined,
    );
    parentNode.addChildLast(powerNode);
  }

  private appendUserDeviceStatesNode(
    ctx: Trace,
    parentNode: TrackNode,
    androidId: string,
    keyPrefix: string,
    selectionFilter: string,
  ): void {
    const stateTracks = this.userStateTracks.get(androidId);
    if (stateTracks === undefined || stateTracks.length === 0) {
      return;
    }

    const statesNode = createLazyTrackNode(
      ctx,
      {
        name: `Device State (${stateTracks.length} tracks)`,
        isSummary: true,
      },
      (node) => {
        for (const st of stateTracks) {
          const stUri = `${MultiTraceTimelinePlugin.id}#${keyPrefix}:${st.trackName}`;
          this.registerDeviceStateSliceTrack(ctx, stUri, st, selectionFilter);
          node.addChildLast(
            new TrackNode({
              name: `${st.trackName} (${st.sliceCount})`,
              uri: stUri,
            }),
          );
        }
      },
    );
    parentNode.addChildLast(statesNode);
  }

  private registerPowerCounterTrack(
    ctx: Trace,
    uri: string,
    trackId: number,
    selectionFilter: string,
  ): void {
    this.counterTrackFilters.set(uri, selectionFilter);
    ctx.tracks.registerTrack({
      uri,
      renderer: CounterTrack.create({
        trace: ctx,
        uri,
        unit: 'mW',
        sqlSource: `
          SELECT ts, value
          FROM counter
          WHERE track_id = ${trackId}
          ORDER BY ts
        `,
      }),
    });
  }

  private registerDeviceStateSliceTrack(
    ctx: Trace,
    uri: string,
    info: DeviceStateTrackInfo,
    selectionFilter: string,
  ): void {
    this.counterTrackFilters.set(uri, selectionFilter);
    ctx.tracks.registerTrack({
      uri,
      renderer: SliceTrack.create({
        trace: ctx,
        uri,
        dataset: new SourceDataset({
          schema: DEVICE_STATE_SPEC,
          src: `
            SELECT id, ts, dur, depth, name
            FROM slice
            WHERE track_id = ${info.trackId}
          `,
        }),
        detailsPanel: () =>
          new DeviceStateDetailsPanel(ctx, info.androidId, info.trackName),
      }),
    });
  }

  private registerCounterTrack(
    ctx: Trace,
    uri: string,
    whereClause: string,
  ): void {
    this.counterTrackFilters.set(uri, whereClause);
    ctx.tracks.registerTrack({
      uri,
      renderer: CounterTrack.create({
        trace: ctx,
        uri,
        sqlSource: `
          SELECT
            ts,
            SUM(delta_sum) OVER (ORDER BY ts) AS value
          FROM (
            SELECT ts, SUM(delta) AS delta_sum
            FROM (
              SELECT ts, 1 AS delta
              FROM ${TABLE_NAME}
              WHERE ${whereClause} AND ts >= 0 AND dur > 0
              UNION ALL
              SELECT ts + dur AS ts, -1 AS delta
              FROM ${TABLE_NAME}
              WHERE ${whereClause} AND ts >= 0 AND dur > 0
            )
            GROUP BY ts
          )
          ORDER BY ts
        `,
      }),
    });
  }

  private registerSliceTrack(
    ctx: Trace,
    uri: string,
    whereClause: string,
  ): void {
    ctx.tracks.registerTrack({
      uri,
      renderer: SliceTrack.create({
        trace: ctx,
        uri,
        dataset: new SourceDataset({
          schema: this.datasetSpec,
          src: `
            SELECT
              ${selectColumnList(this.breakdownColumns, '+ts AS ts')}
            FROM ${TABLE_NAME}
            WHERE ${whereClause}
          `,
        }),
        detailsPanel: () =>
          new DeviceTraceDetailsPanel(
            ctx,
            this.breakdownColumns,
            this.datasetSpec,
          ),
      }),
    });
  }
}

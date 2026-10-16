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

import m from 'mithril';
import {AsyncMemo} from '../../base/async_memo';
import {Icons} from '../../base/semantic_icons';
import {sqliteString} from '../../base/string_utils';
import {Duration, type duration, Time, type time} from '../../base/time';
import {selectTracksAndGetDataset} from '../../components/aggregation_adapter';
import {
  BreakdownTrackAggType,
  BreakdownTracks,
} from '../../components/tracks/breakdown_tracks';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {PerfettoPlugin} from '../../public/plugin';
import type {
  AreaSelection,
  AreaSelectionTab,
  ContentWithLoadingFlag,
  TrackEventSelection,
} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {type Dataset, SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {Button} from '../../widgets/button';
import {Checkbox} from '../../widgets/checkbox';
import {DetailsShell} from '../../widgets/details_shell';
import {Grid, GridCell, GridHeaderCell} from '../../widgets/grid';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';

const TRACE_UUID_URL = 'http://go/trace-uuid/';
const TRACE_UUIDS_URL = 'http://go/trace-uuids/';
const TABLE_NAME = '_device_traces';

interface BreakdownColumn {
  readonly label: string;
  readonly sqlCol: string;
}

const EXCLUDED_BREAKDOWN_ARGS = new Set([
  'dur_ns',
  'armed_start_ts',
  'armed_dur_ns',
  'trace_uuid',
]);

const KNOWN_BREAKDOWN_COLUMNS: ReadonlyMap<string, BreakdownColumn> = new Map([
  ['session_name', {label: 'session_name', sqlCol: 'session_name'}],
  [
    'subscription_name',
    {label: 'subscription_name', sqlCol: 'subscription_name'},
  ],
  ['device_name', {label: 'device_name', sqlCol: 'device_name'}],
  ['build_id', {label: 'build_id', sqlCol: 'build_id'}],
  ['android_id', {label: 'android_id', sqlCol: 'raw_android_id'}],
  ['trigger_name', {label: 'trigger_name', sqlCol: 'raw_trigger_name'}],
]);

const DEVICE_TRACE_DATASET_SPEC = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  armed_start_ts: LONG,
  armed_dur: LONG,
  name: STR,
  session_name: STR,
  subscription_name: STR,
  trigger_name: STR,
  device_name: STR,
  build_id: STR,
  android_id: STR,
  trace_uuid: STR,
};

interface DeviceTraceRow {
  readonly id: number;
  readonly ts: time;
  readonly dur: duration;
  readonly armedStartTs: time;
  readonly armedDur: duration;
  readonly name: string;
  readonly sessionName: string;
  readonly subscriptionName: string;
  readonly triggerName: string;
  readonly deviceName: string;
  readonly buildId: string;
  readonly androidId: string;
  readonly traceUuid: string;
}

class DeviceTraceDetailsPanel implements TrackEventDetailsPanel {
  private row?: DeviceTraceRow;
  private isLoading = true;

  constructor(private readonly trace: Trace) {}

  async load(selection: TrackEventSelection): Promise<void> {
    this.isLoading = true;
    const res = await this.trace.engine.query(`
      SELECT
        id,
        ts,
        dur,
        armed_start_ts,
        armed_dur,
        slice_label AS name,
        session_name,
        subscription_name,
        raw_trigger_name AS trigger_name,
        device_name,
        raw_build_id AS build_id,
        raw_android_id AS android_id,
        trace_uuid
      FROM ${TABLE_NAME}
      WHERE id = ${selection.eventId}
      LIMIT 1
    `);
    const it = res.iter(DEVICE_TRACE_DATASET_SPEC);
    if (it.valid()) {
      this.row = {
        id: it.id,
        ts: Time.fromRaw(it.ts),
        dur: Duration.fromRaw(it.dur),
        armedStartTs: Time.fromRaw(it.armed_start_ts),
        armedDur: Duration.fromRaw(it.armed_dur),
        name: it.name,
        sessionName: it.session_name,
        subscriptionName: it.subscription_name,
        triggerName: it.trigger_name,
        deviceName: it.device_name,
        buildId: it.build_id,
        androidId: it.android_id,
        traceUuid: it.trace_uuid,
      };
    }
    this.isLoading = false;
  }

  render() {
    if (this.isLoading) {
      return m(DetailsShell, {
        title: 'Device Trace',
        description: 'Loading...',
      });
    }
    if (!this.row) {
      return m(DetailsShell, {
        title: 'Device Trace',
        description: 'Trace interval not found',
      });
    }
    const r = this.row;
    const href = `${TRACE_UUID_URL}${r.traceUuid}`;
    return m(
      DetailsShell,
      {
        title: 'Device Trace',
        description: r.name,
      },
      m(
        Section,
        {title: 'Trace Metadata'},
        m(
          Tree,
          m(TreeNode, {
            left: 'Trace UUID',
            right: r.traceUuid
              ? m(Anchor, {href, target: '_blank'}, r.traceUuid)
              : 'N/A',
          }),
          m(TreeNode, {left: 'Session', right: r.sessionName}),
          m(TreeNode, {left: 'Subscription', right: r.subscriptionName}),
          m(TreeNode, {left: 'Trigger', right: r.triggerName || '-'}),
          m(TreeNode, {left: 'Device', right: r.deviceName}),
          m(TreeNode, {left: 'Build ID', right: r.buildId}),
          m(TreeNode, {left: 'Android ID', right: r.androidId}),
          m(TreeNode, {
            left: 'Start',
            right: m(Timestamp, {trace: this.trace, ts: r.ts}),
          }),
          m(TreeNode, {
            left: 'Duration',
            right: m(DurationWidget, {trace: this.trace, dur: r.dur}),
          }),
          r.armedDur > r.dur &&
            m(TreeNode, {
              left: 'Armed Start',
              right: m(Timestamp, {trace: this.trace, ts: r.armedStartTs}),
            }),
          r.armedDur > r.dur &&
            m(TreeNode, {
              left: 'Armed Duration',
              right: m(DurationWidget, {trace: this.trace, dur: r.armedDur}),
            }),
        ),
      ),
    );
  }
}

class DeviceTracesSelectionTab implements AreaSelectionTab {
  readonly id = 'device_traces_selection';
  readonly name = 'Device traces';

  private readonly memo = new AsyncMemo<{
    rows: DeviceTraceRow[];
    selected: Set<string>;
  }>();

  constructor(
    private readonly trace: Trace,
    private readonly counterTrackFilters: ReadonlyMap<string, string>,
  ) {}

  render(selection: AreaSelection): ContentWithLoadingFlag | undefined {
    const dataset = selectTracksAndGetDataset(
      selection.tracks,
      DEVICE_TRACE_DATASET_SPEC,
    );
    const {data, isPending} = this.memo.use({
      key: {
        start: selection.start,
        end: selection.end,
        trackUris: selection.trackUris,
      },
      compute: async () => {
        const rows = await this.queryRows(dataset, selection);
        return {
          rows,
          selected: new Set(
            rows
              .slice(0, 50)
              .map((r) => r.traceUuid)
              .filter(Boolean),
          ),
        };
      },
    });

    return {
      isLoading: isPending,
      buttons: m(Button, {
        label: `Merge selected traces (${data?.selected.size ?? 0})`,
        icon: Icons.ExternalLink,
        disabled: data === undefined || data.selected.size === 0,
        onclick: () => {
          if (data !== undefined && data.selected.size > 0) {
            window.open(
              TRACE_UUIDS_URL + [...data.selected].join(','),
              '_blank',
            );
          }
        },
      }),
      content:
        data === undefined
          ? undefined
          : this.renderGrid(data.rows, data.selected),
    };
  }

  private async queryRows(
    dataset: Dataset | undefined,
    selection: AreaSelection,
  ): Promise<DeviceTraceRow[]> {
    const matchedCounterPredicates: string[] = [];
    for (const uri of selection.trackUris) {
      const pred = this.counterTrackFilters.get(uri);
      if (pred !== undefined) {
        matchedCounterPredicates.push(`(${pred})`);
      }
    }
    const counterWhere =
      matchedCounterPredicates.length > 0
        ? `WHERE ${matchedCounterPredicates.join(' OR ')}`
        : '';

    const src =
      dataset !== undefined
        ? `(${dataset.query(DEVICE_TRACE_DATASET_SPEC)})`
        : `(
            SELECT
              id,
              ts,
              dur,
              armed_start_ts,
              armed_dur,
              slice_label AS name,
              session_name,
              subscription_name,
              raw_trigger_name AS trigger_name,
              device_name,
              raw_build_id AS build_id,
              raw_android_id AS android_id,
              trace_uuid
            FROM ${TABLE_NAME}
            ${counterWhere}
          )`;
    const result = await this.trace.engine.query(`
      SELECT
        id,
        ts,
        dur,
        armed_start_ts,
        armed_dur,
        name,
        session_name,
        subscription_name,
        trigger_name,
        device_name,
        build_id,
        android_id,
        trace_uuid
      FROM ${src}
      WHERE ts < ${selection.end} AND ts + dur > ${selection.start}
      ORDER BY ts
      LIMIT 500
    `);
    const rows: DeviceTraceRow[] = [];
    const it = result.iter(DEVICE_TRACE_DATASET_SPEC);
    for (; it.valid(); it.next()) {
      rows.push({
        id: it.id,
        ts: Time.fromRaw(it.ts),
        dur: Duration.fromRaw(it.dur),
        armedStartTs: Time.fromRaw(it.armed_start_ts),
        armedDur: Duration.fromRaw(it.armed_dur),
        name: it.name,
        sessionName: it.session_name,
        subscriptionName: it.subscription_name,
        triggerName: it.trigger_name,
        deviceName: it.device_name,
        buildId: it.build_id,
        androidId: it.android_id,
        traceUuid: it.trace_uuid,
      });
    }
    return rows;
  }

  private renderGrid(
    rows: ReadonlyArray<DeviceTraceRow>,
    selected: Set<string>,
  ): m.Children {
    const allSelected = rows.length > 0 && selected.size === rows.length;
    return m(Grid, {
      columns: [
        {
          key: 'select',
          header: m(
            GridHeaderCell,
            m(Checkbox, {
              checked: allSelected,
              onchange: () => {
                if (allSelected) {
                  selected.clear();
                } else {
                  rows.forEach((r) => {
                    if (r.traceUuid) selected.add(r.traceUuid);
                  });
                }
              },
            }),
          ),
        },
        {key: 'uuid', header: m(GridHeaderCell, 'Trace UUID')},
        {key: 'session', header: m(GridHeaderCell, 'Session')},
        {key: 'subscription', header: m(GridHeaderCell, 'Subscription')},
        {key: 'trigger', header: m(GridHeaderCell, 'Trigger')},
        {key: 'device', header: m(GridHeaderCell, 'Device')},
        {key: 'build', header: m(GridHeaderCell, 'Build ID')},
        {key: 'android_id', header: m(GridHeaderCell, 'Android ID')},
        {key: 'ts', header: m(GridHeaderCell, 'Start')},
        {key: 'dur', header: m(GridHeaderCell, 'Duration')},
      ],
      rowData: rows.map((r) => [
        m(
          GridCell,
          m(Checkbox, {
            checked: selected.has(r.traceUuid),
            onchange: () => {
              if (selected.has(r.traceUuid)) {
                selected.delete(r.traceUuid);
              } else if (r.traceUuid) {
                selected.add(r.traceUuid);
              }
            },
          }),
        ),
        m(
          GridCell,
          m(
            Anchor,
            {href: TRACE_UUID_URL + r.traceUuid, target: '_blank'},
            r.traceUuid,
          ),
        ),
        m(GridCell, r.sessionName),
        m(GridCell, r.subscriptionName),
        m(GridCell, r.triggerName || '-'),
        m(GridCell, r.deviceName),
        m(GridCell, r.buildId),
        m(GridCell, r.androidId),
        m(GridCell, m(Timestamp, {trace: this.trace, ts: r.ts})),
        m(GridCell, m(DurationWidget, {trace: this.trace, dur: r.dur})),
      ]),
    });
  }
}

export default class DeviceTracesPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.DeviceTraces';
  private readonly counterTrackFilters = new Map<string, string>();
  private breakdownColumns: BreakdownColumn[] = [];
  private customBreakdownCount = 0;

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
    const discoveredArgs = new Set<string>();
    const argIt = argRes.iter({arg_name: STR});
    for (; argIt.valid(); argIt.next()) {
      const argName = argIt.arg_name;
      if (
        !EXCLUDED_BREAKDOWN_ARGS.has(argName) &&
        /^[a-zA-Z_][a-zA-Z0-9_]*$/.test(argName)
      ) {
        discoveredArgs.add(argName);
      }
    }

    const extraArgSelects: string[] = [];
    this.breakdownColumns = [];
    for (const [key, col] of KNOWN_BREAKDOWN_COLUMNS.entries()) {
      if (discoveredArgs.has(key)) {
        this.breakdownColumns.push(col);
      }
    }
    for (const argName of discoveredArgs) {
      if (!KNOWN_BREAKDOWN_COLUMNS.has(argName)) {
        const sqlCol = `dyn_${argName}`;
        extraArgSelects.push(
          `,\n        COALESCE(CAST(EXTRACT_ARG(arg_set_id, 'debug.${argName}') AS TEXT), '') AS ${sqlCol}`,
        );
        this.breakdownColumns.push({label: argName, sqlCol});
      }
    }

    // Prefix numeric-looking build_id so BreakdownTracks's toSqlValue() keeps
    // it quoted as a SQL string literal when filtering STRING columns.
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
        COALESCE(
          NULLIF(EXTRACT_ARG(arg_set_id, 'debug.session_name'), ''),
          'unknown_session'
        ) AS session_name,
        COALESCE(
          NULLIF(EXTRACT_ARG(arg_set_id, 'debug.subscription_name'), ''),
          NULLIF(EXTRACT_ARG(arg_set_id, 'debug.session_name'), ''),
          'unknown_subscription'
        ) AS subscription_name,
        COALESCE(
          NULLIF(EXTRACT_ARG(arg_set_id, 'debug.device_name'), ''),
          'unknown_device'
        ) AS device_name,
        'build ' || COALESCE(
          NULLIF(EXTRACT_ARG(arg_set_id, 'debug.build_id'), ''),
          'unknown'
        ) AS build_id,
        COALESCE(
          NULLIF(EXTRACT_ARG(arg_set_id, 'debug.build_id'), ''),
          'unknown'
        ) AS raw_build_id,
        COALESCE(
          NULLIF(EXTRACT_ARG(arg_set_id, 'debug.android_id'), ''),
          'unknown'
        ) AS raw_android_id,
        COALESCE(EXTRACT_ARG(arg_set_id, 'debug.trigger_name'), '') AS raw_trigger_name,
        COALESCE(
          NULLIF(EXTRACT_ARG(arg_set_id, 'debug.trigger_name'), ''),
          NULLIF(EXTRACT_ARG(arg_set_id, 'debug.subscription_name'), ''),
          name
        ) AS slice_label,
        COALESCE(EXTRACT_ARG(arg_set_id, 'debug.trace_uuid'), '') AS trace_uuid${extraArgSelects.join('')}
      FROM slice
      WHERE category = 'device_trace';

      CREATE PERFETTO INDEX _dt_session_sub_aid_idx
        ON ${TABLE_NAME}(session_name, subscription_name, raw_android_id);
      CREATE PERFETTO INDEX _dt_aid_session_sub_idx
        ON ${TABLE_NAME}(raw_android_id, session_name, subscription_name);
    `);

    // 1. session_name -> subscription_name -> android_id
    const sessionSubRoot = await this.createSessionSubAndroidTree(ctx);

    // 2. android_id -> session_name -> subscription_name
    const androidSessionSubRoot = this.createAndroidSessionSubTree(ctx);

    ctx.defaultWorkspace.addChildInOrder(sessionSubRoot);
    ctx.defaultWorkspace.addChildInOrder(androidSessionSubRoot);

    ctx.selection.registerAreaSelectionTab(
      new DeviceTracesSelectionTab(ctx, this.counterTrackFilters),
    );

    ctx.commands.registerCommand({
      id: 'com.android.DeviceTraces.AddCustomBreakdown',
      name: 'Device Traces: Add custom breakdown...',
      callback: async () => {
        await this.promptAndAddCustomBreakdown(ctx);
      },
    });
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

    const pathStr = picked.map((c) => c.label).join(' -> ');
    const customId = ++this.customBreakdownCount;
    const keyPrefix = `custom_${customId}`;

    const counterCols = picked.slice(0, -1);
    const firstAndroidIdx = counterCols.findIndex(
      (c) => c.sqlCol === 'raw_android_id',
    );
    const btCounterCols =
      firstAndroidIdx === -1
        ? counterCols
        : counterCols.slice(0, firstAndroidIdx);
    const tailCols = picked.slice(btCounterCols.length);

    let rootNode: TrackNode;
    if (btCounterCols.length > 0) {
      const bt = new BreakdownTracks({
        trace: ctx,
        trackTitle: `Device Traces (${pathStr})`,
        description: `Concurrent active device traces broken down by ${pathStr}.`,
        aggregationType: BreakdownTrackAggType.COUNT,
        aggregation: {
          columns: btCounterCols.map((c) => c.sqlCol),
          tsCol: 'ts',
          durCol: 'dur',
          tableName: TABLE_NAME,
        },
        sortTracks: true,
        detailsPanel: (trace: Trace) => new DeviceTraceDetailsPanel(trace),
      });
      rootNode = await bt.createTracks();
      rootNode.removable = true;
      if (rootNode.uri) {
        this.counterTrackFilters.set(rootNode.uri, '1 = 1');
      }
      this.attachTailToBreakdownTree(
        ctx,
        rootNode,
        btCounterCols,
        0,
        '1 = 1',
        keyPrefix,
        tailCols,
      );
    } else {
      const rootUri = `${DeviceTracesPlugin.id}#${keyPrefix}_root`;
      this.registerCounterTrack(ctx, rootUri, '1 = 1');
      rootNode = new TrackNode({
        name: `Device Traces (${pathStr})`,
        uri: rootUri,
        removable: true,
      });
      this.attachLazyCustomBreakdownLevels(
        ctx,
        rootNode,
        tailCols,
        '1 = 1',
        keyPrefix,
      );
    }

    ctx.defaultWorkspace.addChildInOrder(rootNode);
  }

  private attachTailToBreakdownTree(
    ctx: Trace,
    node: TrackNode,
    btCounterCols: ReadonlyArray<BreakdownColumn>,
    depth: number,
    whereClause: string,
    keyPrefix: string,
    tailCols: ReadonlyArray<BreakdownColumn>,
  ): void {
    if (depth === btCounterCols.length) {
      this.attachLazyCustomBreakdownLevels(
        ctx,
        node,
        tailCols,
        whereClause,
        keyPrefix,
      );
      return;
    }
    const col = btCounterCols[depth];
    for (const child of node.children) {
      const childFilter =
        whereClause === '1 = 1'
          ? `${col.sqlCol} = ${sqliteString(child.name)}`
          : `${whereClause} AND ${col.sqlCol} = ${sqliteString(child.name)}`;
      if (child.uri) {
        this.counterTrackFilters.set(child.uri, childFilter);
      }
      this.attachTailToBreakdownTree(
        ctx,
        child,
        btCounterCols,
        depth + 1,
        childFilter,
        `${keyPrefix}:${col.label}:${child.name}`,
        tailCols,
      );
    }
  }

  private attachLazyCustomBreakdownLevels(
    ctx: Trace,
    parentNode: TrackNode,
    remainingCols: ReadonlyArray<BreakdownColumn>,
    whereClause: string,
    keyPrefix: string,
  ): void {
    if (remainingCols.length === 0) {
      return;
    }
    const [col, ...restCols] = remainingCols;
    const isLeafLevel = restCols.length === 0;

    if (col.sqlCol === 'raw_android_id') {
      if (isLeafLevel) {
        this.attachLazyAndroidIdLanes(ctx, parentNode, whereClause, keyPrefix);
        return;
      }
      this.onFirstExpand(parentNode, async () => {
        const res = await ctx.engine.query(`
          SELECT
            raw_android_id AS android_id,
            device_name,
            raw_build_id AS build_id,
            COUNT(*) AS trace_count
          FROM ${TABLE_NAME}
          WHERE ${whereClause}
          GROUP BY raw_android_id
          ORDER BY trace_count DESC, raw_android_id ASC
        `);

        const it = res.iter({
          android_id: STR,
          device_name: STR,
          build_id: STR,
          trace_count: NUM,
        });
        for (; it.valid(); it.next()) {
          const androidId = it.android_id;
          const deviceName = it.device_name;
          const buildId = it.build_id;
          const count = it.trace_count;
          const childFilter = `${whereClause} AND raw_android_id = ${sqliteString(androidId)}`;
          const childKey = `${keyPrefix}:aid:${androidId}`;
          const uri = `${DeviceTracesPlugin.id}#${childKey}`;
          this.registerCounterTrack(ctx, uri, childFilter);
          const childNode = new TrackNode({
            name: `android_id ${androidId} [${deviceName} / ${buildId}] (${count})`,
            uri,
          });
          parentNode.addChildLast(childNode);
          this.attachLazyCustomBreakdownLevels(
            ctx,
            childNode,
            restCols,
            childFilter,
            childKey,
          );
        }
      });
      return;
    }

    this.onFirstExpand(parentNode, async () => {
      const res = await ctx.engine.query(`
        SELECT
          ${col.sqlCol} AS val,
          COUNT(*) AS trace_count
        FROM ${TABLE_NAME}
        WHERE ${whereClause}
        GROUP BY ${col.sqlCol}
        ORDER BY trace_count DESC, val ASC
      `);

      const it = res.iter({
        val: STR,
        trace_count: NUM,
      });
      for (; it.valid(); it.next()) {
        const val = it.val;
        const count = it.trace_count;
        const displayVal = val || '(empty)';
        const childFilter = `${whereClause} AND ${col.sqlCol} = ${sqliteString(val)}`;
        const childKey = `${keyPrefix}:${col.label}:${val}`;
        const uri = `${DeviceTracesPlugin.id}#${childKey}`;

        if (isLeafLevel) {
          this.registerSliceTrack(ctx, uri, childFilter);
        } else {
          this.registerCounterTrack(ctx, uri, childFilter);
        }

        const childNode = new TrackNode({
          name: `${displayVal} (${count})`,
          uri,
        });
        parentNode.addChildLast(childNode);

        if (!isLeafLevel) {
          this.attachLazyCustomBreakdownLevels(
            ctx,
            childNode,
            restCols,
            childFilter,
            childKey,
          );
        }
      }
    });
  }

  private async createSessionSubAndroidTree(ctx: Trace): Promise<TrackNode> {
    const bt = new BreakdownTracks({
      trace: ctx,
      trackTitle:
        'Device Traces (session_name -> subscription_name -> android_id)',
      description:
        'Concurrent active device traces broken down by session_name -> subscription_name -> android_id.',
      aggregationType: BreakdownTrackAggType.COUNT,
      aggregation: {
        columns: ['session_name', 'subscription_name'],
        tsCol: 'ts',
        durCol: 'dur',
        tableName: TABLE_NAME,
      },
      sortTracks: true,
      detailsPanel: (trace: Trace) => new DeviceTraceDetailsPanel(trace),
    });

    const rootNode = await bt.createTracks();
    if (rootNode.uri) {
      this.counterTrackFilters.set(rootNode.uri, '1 = 1');
    }

    for (const sessionNode of rootNode.children) {
      const sFilter = `session_name = ${sqliteString(sessionNode.name)}`;
      if (sessionNode.uri) {
        this.counterTrackFilters.set(sessionNode.uri, sFilter);
      }
      for (const subNode of sessionNode.children) {
        const subFilter = `${sFilter} AND subscription_name = ${sqliteString(subNode.name)}`;
        if (subNode.uri) {
          this.counterTrackFilters.set(subNode.uri, subFilter);
        }
        this.attachLazyAndroidIdLanes(
          ctx,
          subNode,
          subFilter,
          `ss:${sessionNode.name}:${subNode.name}`,
        );
      }
    }

    return rootNode;
  }

  private createAndroidSessionSubTree(ctx: Trace): TrackNode {
    const rootUri = `${DeviceTracesPlugin.id}#aid_root`;
    this.registerCounterTrack(ctx, rootUri, '1 = 1');
    const rootNode = new TrackNode({
      name: 'Device Traces (android_id -> session_name -> subscription_name)',
      uri: rootUri,
    });

    this.attachLazyAndroidSessionSubChildren(
      ctx,
      rootNode,
      '1 = 1',
      'aid_root',
    );
    return rootNode;
  }

  private attachLazyAndroidSessionSubChildren(
    ctx: Trace,
    parentNode: TrackNode,
    whereClause: string,
    keyPrefix: string,
  ): void {
    this.onFirstExpand(parentNode, async () => {
      const res = await ctx.engine.query(`
        SELECT
          raw_android_id AS android_id,
          device_name,
          COUNT(*) AS trace_count,
          COUNT(DISTINCT session_name) AS session_count
        FROM ${TABLE_NAME}
        WHERE ${whereClause}
        GROUP BY raw_android_id
        ORDER BY session_count DESC, trace_count DESC, raw_android_id ASC
      `);

      const it = res.iter({
        android_id: STR,
        device_name: STR,
        trace_count: NUM,
        session_count: NUM,
      });
      for (; it.valid(); it.next()) {
        const androidId = it.android_id;
        const deviceName = it.device_name;
        const traceCount = it.trace_count;
        const sessionCount = it.session_count;
        const aFilter = `${whereClause} AND raw_android_id = ${sqliteString(androidId)}`;
        const aUri = `${DeviceTracesPlugin.id}#${keyPrefix}:aid:${androidId}`;
        this.registerCounterTrack(ctx, aUri, aFilter);

        const aNode = new TrackNode({
          name: `android_id ${androidId} [${deviceName}] (${traceCount} traces, ${sessionCount} sessions)`,
          uri: aUri,
        });
        parentNode.addChildLast(aNode);

        this.attachLazySessionAndSubForAndroidId(
          ctx,
          aNode,
          aFilter,
          `${keyPrefix}:aid:${androidId}`,
        );
      }
    });
  }

  private attachLazySessionAndSubForAndroidId(
    ctx: Trace,
    androidNode: TrackNode,
    whereClause: string,
    keyPrefix: string,
  ): void {
    this.onFirstExpand(androidNode, async () => {
      const res = await ctx.engine.query(`
        SELECT
          session_name,
          subscription_name,
          COUNT(*) AS trace_count
        FROM ${TABLE_NAME}
        WHERE ${whereClause}
        GROUP BY session_name, subscription_name
        ORDER BY session_name ASC, trace_count DESC, subscription_name ASC
      `);

      const sessionNodes = new Map<string, {node: TrackNode; total: number}>();
      const subRows: Array<{
        sessionName: string;
        subName: string;
        count: number;
      }> = [];

      const it = res.iter({
        session_name: STR,
        subscription_name: STR,
        trace_count: NUM,
      });
      for (; it.valid(); it.next()) {
        subRows.push({
          sessionName: it.session_name,
          subName: it.subscription_name,
          count: it.trace_count,
        });
      }

      for (const {sessionName, count} of subRows) {
        const existing = sessionNodes.get(sessionName);
        if (existing) {
          existing.total += count;
        } else {
          const sFilter = `${whereClause} AND session_name = ${sqliteString(sessionName)}`;
          const sUri = `${DeviceTracesPlugin.id}#${keyPrefix}:s:${sessionName}`;
          this.registerSliceTrack(ctx, sUri, sFilter);
          const sNode = new TrackNode({
            name: sessionName,
            uri: sUri,
            isSummary: true,
          });
          sessionNodes.set(sessionName, {node: sNode, total: count});
          androidNode.addChildLast(sNode);
        }
      }

      for (const [sessionName, info] of sessionNodes.entries()) {
        info.node.name = `${sessionName} (${info.total})`;
      }

      for (const {sessionName, subName, count} of subRows) {
        const sessionInfo = sessionNodes.get(sessionName)!;
        const subFilter = `${whereClause} AND session_name = ${sqliteString(sessionName)} AND subscription_name = ${sqliteString(subName)}`;
        const subUri = `${DeviceTracesPlugin.id}#${keyPrefix}:s:${sessionName}:sub:${subName}`;
        this.registerSliceTrack(ctx, subUri, subFilter);
        sessionInfo.node.addChildLast(
          new TrackNode({
            name: `${subName} (${count})`,
            uri: subUri,
          }),
        );
      }
    });
  }

  private attachLazyAndroidIdLanes(
    ctx: Trace,
    parentNode: TrackNode,
    whereClause: string,
    keyPrefix: string,
  ): void {
    this.onFirstExpand(parentNode, async () => {
      const res = await ctx.engine.query(`
        SELECT
          raw_android_id AS android_id,
          device_name,
          raw_build_id AS build_id,
          COUNT(*) AS trace_count
        FROM ${TABLE_NAME}
        WHERE ${whereClause}
        GROUP BY raw_android_id
        ORDER BY trace_count DESC, raw_android_id ASC
      `);

      const it = res.iter({
        android_id: STR,
        device_name: STR,
        build_id: STR,
        trace_count: NUM,
      });
      for (; it.valid(); it.next()) {
        const androidId = it.android_id;
        const deviceName = it.device_name;
        const buildId = it.build_id;
        const traceCount = it.trace_count;
        const aFilter = `${whereClause} AND raw_android_id = ${sqliteString(androidId)}`;
        const uri = `${DeviceTracesPlugin.id}#${keyPrefix}:aid:${androidId}`;
        this.registerSliceTrack(ctx, uri, aFilter);
        parentNode.addChildLast(
          new TrackNode({
            name: `android_id ${androidId} [${deviceName} / ${buildId}] (${traceCount})`,
            uri,
          }),
        );
      }
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
          schema: DEVICE_TRACE_DATASET_SPEC,
          src: `
            SELECT
              id,
              +ts AS ts,
              dur,
              armed_start_ts,
              armed_dur,
              slice_label AS name,
              session_name,
              subscription_name,
              raw_trigger_name AS trigger_name,
              device_name,
              raw_build_id AS build_id,
              raw_android_id AS android_id,
              trace_uuid
            FROM ${TABLE_NAME}
            WHERE ${whereClause}
          `,
        }),
        detailsPanel: () => new DeviceTraceDetailsPanel(ctx),
      }),
    });
  }

  private onFirstExpand(node: TrackNode, populate: () => Promise<void>): void {
    const placeholder = new TrackNode({name: 'Loading...', headless: true});
    node.addChildLast(placeholder);

    let populated = false;
    const run = async () => {
      if (populated) return;
      populated = true;
      await populate();
      placeholder.remove();
    };

    const origExpand = node.expand.bind(node);
    node.expand = () => {
      origExpand();
      run();
    };
    const origToggle = node.toggleCollapsed.bind(node);
    node.toggleCollapsed = () => {
      origToggle();
      if (node.expanded) {
        run();
      }
    };
  }
}

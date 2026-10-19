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
import {
  type Aggregation,
  type Aggregator,
  createAggregationData,
  createAggregationTab,
  selectTracksAndGetDataset,
} from '../../components/aggregation_adapter';
import {formatDuration} from '../../components/time_utils';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {ColumnDef} from '../../components/widgets/datagrid/datagrid_schema';
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
import {
  type Dataset,
  type DatasetSchema,
  SourceDataset,
} from '../../trace_processor/dataset';
import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  type RowIterator,
  type SqlValue,
  STR,
} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {Anchor} from '../../widgets/anchor';
import {Button} from '../../widgets/button';
import {Checkbox} from '../../widgets/checkbox';
import {DetailsShell} from '../../widgets/details_shell';
import {Grid, GridCell, GridHeaderCell} from '../../widgets/grid';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import TrackEventPlugin from '../dev.perfetto.TrackEvent';

const TRACE_UUID_URL = 'http://go/trace-uuid/';
const TRACE_UUIDS_URL = 'http://go/trace-uuids/';
const TABLE_NAME = '_device_traces';
const BATTERY_SESSION_NAME = 'session_with_lightweight_battery_tracing';

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

interface BreakdownColumn {
  readonly label: string;
  readonly sqlCol: string;
}

interface PowerRailTrackInfo {
  readonly trackId: number;
  readonly kind:
    'fleet_total' | 'fleet_sub' | 'user_total' | 'user_sub' | 'user_rail';
  readonly androidId: string;
  readonly subsystemName: string;
  readonly railName: string;
  readonly totalMwh: number;
}

interface DeviceStateTrackInfo {
  readonly trackId: number;
  readonly androidId: string;
  readonly trackName: string;
  readonly sliceCount: number;
}

const EXCLUDED_BREAKDOWN_ARGS = new Set([
  'dur_ns',
  'armed_start_ts',
  'armed_dur_ns',
  'trace_uuid',
  'trace_size_bytes',
  'original_trace_size_bytes',
  'trace_dur_ns',
  'sched_dur_ns',
]);

function formatBytes(bytes: bigint): string {
  if (bytes <= 0n) {
    return '-';
  }
  const n = Number(bytes);
  const raw = `${bytes.toLocaleString()} B`;
  if (n < 1024) {
    return raw;
  }
  const units = ['KB', 'MB', 'GB', 'TB'];
  let val = n / 1024;
  let unitIdx = 0;
  while (val >= 1024 && unitIdx < units.length - 1) {
    val /= 1024;
    unitIdx++;
  }
  return `${val.toFixed(2)} ${units[unitIdx]} (${raw})`;
}

function formatEnergyMwh(mwh: number): string {
  if (!Number.isFinite(mwh) || mwh <= 0) {
    return '0 mWh';
  }
  if (mwh >= 1000) {
    return `${(mwh / 1000).toFixed(2)} Wh`;
  }
  return `${mwh.toFixed(1)} mWh`;
}

const DEVICE_TRACE_BASE_SPEC = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  armed_start_ts: LONG,
  armed_dur: LONG,
  name: STR,
  trace_uuid: STR,
  trace_size_bytes: LONG,
  original_trace_size_bytes: LONG,
  trace_dur: LONG,
  sched_dur: LONG,
};

type DeviceTraceDatasetSpec = DatasetSchema & typeof DEVICE_TRACE_BASE_SPEC;

function buildDatasetSpec(
  cols: ReadonlyArray<BreakdownColumn>,
): DeviceTraceDatasetSpec {
  const spec: DatasetSchema = {...DEVICE_TRACE_BASE_SPEC};
  for (const c of cols) {
    spec[c.sqlCol] = STR;
  }
  return spec as DeviceTraceDatasetSpec;
}

function selectColumnList(
  cols: ReadonlyArray<BreakdownColumn>,
  tsExpr = 'ts',
  nameExpr = 'slice_label AS name',
): string {
  return [
    'id',
    tsExpr,
    'dur',
    'armed_start_ts',
    'armed_dur',
    nameExpr,
    'trace_uuid',
    'trace_size_bytes',
    'original_trace_size_bytes',
    'trace_dur',
    'sched_dur',
    ...cols.map((c) => c.sqlCol),
  ].join(',\n');
}

interface DeviceTraceRow {
  readonly id: number;
  readonly ts: time;
  readonly dur: duration;
  readonly armedStartTs: time;
  readonly armedDur: duration;
  readonly name: string;
  readonly traceUuid: string;
  readonly traceSizeBytes: bigint;
  readonly originalTraceSizeBytes: bigint;
  readonly traceDur: duration;
  readonly schedDur: duration;
  readonly cols: ReadonlyMap<string, string>;
}

function parseDeviceTraceRow(
  it: RowIterator<typeof DEVICE_TRACE_BASE_SPEC>,
  breakdownColumns: ReadonlyArray<BreakdownColumn>,
): DeviceTraceRow {
  const cols = new Map<string, string>();
  for (const col of breakdownColumns) {
    const val = it.get(col.sqlCol);
    cols.set(col.sqlCol, typeof val === 'string' ? val : String(val ?? ''));
  }
  return {
    id: it.id,
    ts: Time.fromRaw(it.ts),
    dur: Duration.fromRaw(it.dur),
    armedStartTs: Time.fromRaw(it.armed_start_ts),
    armedDur: Duration.fromRaw(it.armed_dur),
    name: it.name,
    traceUuid: it.trace_uuid,
    traceSizeBytes: it.trace_size_bytes,
    originalTraceSizeBytes: it.original_trace_size_bytes,
    traceDur: Duration.fromRaw(it.trace_dur),
    schedDur: Duration.fromRaw(it.sched_dur),
    cols,
  };
}

class DeviceTraceDetailsPanel implements TrackEventDetailsPanel {
  private row?: DeviceTraceRow;
  private isLoading = true;

  constructor(
    private readonly trace: Trace,
    private readonly breakdownColumns: ReadonlyArray<BreakdownColumn>,
    private readonly datasetSpec: DeviceTraceDatasetSpec,
  ) {}

  async load(selection: TrackEventSelection): Promise<void> {
    this.isLoading = true;
    const res = await this.trace.engine.query(`
      SELECT
        ${selectColumnList(this.breakdownColumns)}
      FROM ${TABLE_NAME}
      WHERE id = ${selection.eventId}
      LIMIT 1
    `);
    const it = res.iter(this.datasetSpec);
    if (it.valid()) {
      this.row = parseDeviceTraceRow(it, this.breakdownColumns);
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
          ...this.breakdownColumns.map((col) =>
            m(TreeNode, {
              left: col.label,
              right: r.cols.get(col.sqlCol) || '-',
            }),
          ),
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
          m(TreeNode, {
            left: 'Trace Duration',
            right:
              r.traceDur > 0n
                ? m(DurationWidget, {trace: this.trace, dur: r.traceDur})
                : '-',
          }),
          m(TreeNode, {
            left: 'Sched Duration',
            right:
              r.schedDur > 0n
                ? m(DurationWidget, {trace: this.trace, dur: r.schedDur})
                : '-',
          }),
          m(TreeNode, {
            left: 'Uncompressed Size',
            right: formatBytes(r.traceSizeBytes),
          }),
          m(TreeNode, {
            left: 'Compressed Size',
            right: formatBytes(r.originalTraceSizeBytes),
          }),
        ),
      ),
    );
  }
}

const DEVICE_STATE_SPEC = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  depth: NUM,
  name: STR,
};

interface DeviceStateRow {
  readonly id: number;
  readonly ts: time;
  readonly dur: duration;
  readonly name: string;
}

class DeviceStateDetailsPanel implements TrackEventDetailsPanel {
  private row?: DeviceStateRow;
  private isLoading = true;

  constructor(
    private readonly trace: Trace,
    private readonly androidId: string,
    private readonly stateTrack: string,
  ) {}

  async load(selection: TrackEventSelection): Promise<void> {
    this.isLoading = true;
    const res = await this.trace.engine.query(`
      SELECT
        id,
        ts,
        dur,
        depth,
        name
      FROM slice
      WHERE id = ${selection.eventId}
      LIMIT 1
    `);
    const it = res.iter(DEVICE_STATE_SPEC);
    if (it.valid()) {
      this.row = {
        id: it.id,
        ts: Time.fromRaw(it.ts),
        dur: Duration.fromRaw(it.dur),
        name: it.name,
      };
    }
    this.isLoading = false;
  }

  render() {
    if (this.isLoading) {
      return m(DetailsShell, {
        title: 'Device State',
        description: 'Loading...',
      });
    }
    if (!this.row) {
      return m(DetailsShell, {
        title: 'Device State',
        description: 'State interval not found',
      });
    }
    const r = this.row;
    return m(
      DetailsShell,
      {
        title: `Device State: ${this.stateTrack}`,
        description: r.name,
      },
      m(
        Section,
        {title: 'State Interval'},
        m(
          Tree,
          m(TreeNode, {left: 'Track', right: this.stateTrack}),
          m(TreeNode, {left: 'State Value', right: r.name}),
          m(TreeNode, {left: 'android_id', right: this.androidId}),
          m(TreeNode, {
            left: 'Start',
            right: m(Timestamp, {trace: this.trace, ts: r.ts}),
          }),
          m(TreeNode, {
            left: 'Duration',
            right: m(DurationWidget, {trace: this.trace, dur: r.dur}),
          }),
        ),
      ),
    );
  }
}

function toBigInt(value: SqlValue): bigint | undefined {
  if (typeof value === 'bigint') {
    return value;
  }
  if (typeof value === 'number' && Number.isFinite(value)) {
    return BigInt(Math.round(value));
  }
  return undefined;
}

function buildSelectionSourceSql(
  dataset: Dataset | undefined,
  selection: AreaSelection,
  counterTrackFilters: ReadonlyMap<string, string>,
  breakdownColumns: ReadonlyArray<BreakdownColumn>,
  datasetSpec: DeviceTraceDatasetSpec,
): string {
  const matchedCounterPredicates: string[] = [];
  for (const uri of selection.trackUris) {
    const pred = counterTrackFilters.get(uri);
    if (pred !== undefined) {
      matchedCounterPredicates.push(`(${pred})`);
    }
  }
  const counterWhere =
    matchedCounterPredicates.length > 0
      ? `WHERE ${matchedCounterPredicates.join(' OR ')}`
      : '';

  return dataset !== undefined
    ? `(${dataset.query(datasetSpec)})`
    : `(
        SELECT
          ${selectColumnList(breakdownColumns)}
        FROM ${TABLE_NAME}
        ${counterWhere}
      )`;
}

class DeviceTracesSelectionAggregator implements Aggregator {
  readonly id = 'device_traces_selection';

  constructor(
    private readonly trace: Trace,
    private readonly counterTrackFilters: ReadonlyMap<string, string>,
    private readonly breakdownColumns: ReadonlyArray<BreakdownColumn>,
    private readonly datasetSpec: DeviceTraceDatasetSpec,
  ) {}

  probe(area: AreaSelection): Aggregation | undefined {
    const dataset = selectTracksAndGetDataset(area.tracks, this.datasetSpec);
    const hasCounterTrack = area.trackUris.some((uri) =>
      this.counterTrackFilters.has(uri),
    );
    if (dataset === undefined && !hasCounterTrack) {
      return undefined;
    }

    const src = buildSelectionSourceSql(
      dataset,
      area,
      this.counterTrackFilters,
      this.breakdownColumns,
      this.datasetSpec,
    );

    return {
      prepareData: async (engine: Engine) => {
        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT
              ${selectColumnList(this.breakdownColumns, 'ts', 'name')}
            FROM ${src}
            WHERE ts < ${area.end} AND ts + dur > ${area.start}
            ORDER BY ts
          `,
        });
        return createAggregationData(table);
      },
      getGridConfig: () => {
        const columns: Record<string, ColumnDef> = {
          trace_uuid: {
            title: 'Trace UUID',
            columnType: 'identifier',
            cellRenderer: (value) => {
              const uuid =
                typeof value === 'string' ? value : String(value ?? '');
              if (!uuid) return '-';
              return m(
                Anchor,
                {href: `${TRACE_UUID_URL}${uuid}`, target: '_blank'},
                uuid,
              );
            },
            cellFormatter: (value) =>
              typeof value === 'string' ? value : String(value ?? ''),
          },
        };
        for (const col of this.breakdownColumns) {
          columns[col.sqlCol] = {
            title: col.label,
            columnType: 'text',
          };
        }
        columns['ts'] = {
          title: 'Start',
          columnType: 'quantitative',
          cellRenderer: (value) => {
            const raw = toBigInt(value);
            if (raw === undefined) return String(value ?? '');
            return m(Timestamp, {trace: this.trace, ts: Time.fromRaw(raw)});
          },
        };
        columns['dur'] = {
          title: 'Duration',
          columnType: 'quantitative',
          cellRenderer: (value) => {
            const raw = toBigInt(value);
            if (raw === undefined) return String(value ?? '');
            return m(DurationWidget, {
              trace: this.trace,
              dur: Duration.fromRaw(raw),
            });
          },
          cellFormatter: (value) => {
            const raw = toBigInt(value);
            return raw !== undefined
              ? formatDuration(this.trace, Duration.fromRaw(raw))
              : String(value ?? '');
          },
        };
        columns['armed_start_ts'] = {
          title: 'Armed Start',
          columnType: 'quantitative',
          cellRenderer: (value) => {
            const raw = toBigInt(value);
            if (raw === undefined) return String(value ?? '');
            return m(Timestamp, {trace: this.trace, ts: Time.fromRaw(raw)});
          },
        };
        columns['armed_dur'] = {
          title: 'Armed Duration',
          columnType: 'quantitative',
          cellRenderer: (value) => {
            const raw = toBigInt(value);
            if (raw === undefined) return String(value ?? '');
            return m(DurationWidget, {
              trace: this.trace,
              dur: Duration.fromRaw(raw),
            });
          },
          cellFormatter: (value) => {
            const raw = toBigInt(value);
            return raw !== undefined
              ? formatDuration(this.trace, Duration.fromRaw(raw))
              : String(value ?? '');
          },
        };
        columns['trace_dur'] = {
          title: 'Trace Duration',
          columnType: 'quantitative',
          cellRenderer: (value) => {
            const raw = toBigInt(value);
            if (raw === undefined || raw <= 0n) return '-';
            return m(DurationWidget, {
              trace: this.trace,
              dur: Duration.fromRaw(raw),
            });
          },
          cellFormatter: (value) => {
            const raw = toBigInt(value);
            return raw !== undefined && raw > 0n
              ? formatDuration(this.trace, Duration.fromRaw(raw))
              : '-';
          },
        };
        columns['sched_dur'] = {
          title: 'Sched Duration',
          columnType: 'quantitative',
          cellRenderer: (value) => {
            const raw = toBigInt(value);
            if (raw === undefined || raw <= 0n) return '-';
            return m(DurationWidget, {
              trace: this.trace,
              dur: Duration.fromRaw(raw),
            });
          },
          cellFormatter: (value) => {
            const raw = toBigInt(value);
            return raw !== undefined && raw > 0n
              ? formatDuration(this.trace, Duration.fromRaw(raw))
              : '-';
          },
        };
        columns['trace_size_bytes'] = {
          title: 'Uncompressed Size',
          columnType: 'quantitative',
          cellRenderer: (value) => {
            const raw = toBigInt(value);
            return raw !== undefined ? formatBytes(raw) : '-';
          },
          cellFormatter: (value) => {
            const raw = toBigInt(value);
            return raw !== undefined ? formatBytes(raw) : '-';
          },
        };
        columns['original_trace_size_bytes'] = {
          title: 'Compressed Size',
          columnType: 'quantitative',
          cellRenderer: (value) => {
            const raw = toBigInt(value);
            return raw !== undefined ? formatBytes(raw) : '-';
          },
          cellFormatter: (value) => {
            const raw = toBigInt(value);
            return raw !== undefined ? formatBytes(raw) : '-';
          },
        };

        return {
          schema: columns,
          initialColumns: [
            {id: 'trace_uuid', field: 'trace_uuid'},
            ...this.breakdownColumns.map((col) => ({
              id: col.sqlCol,
              field: col.sqlCol,
            })),
            {id: 'ts', field: 'ts', sort: 'ASC'},
            {id: 'dur', field: 'dur'},
            {id: 'armed_start_ts', field: 'armed_start_ts'},
            {id: 'armed_dur', field: 'armed_dur'},
            {id: 'trace_dur', field: 'trace_dur'},
            {id: 'sched_dur', field: 'sched_dur'},
            {id: 'trace_size_bytes', field: 'trace_size_bytes'},
            {
              id: 'original_trace_size_bytes',
              field: 'original_trace_size_bytes',
            },
          ],
        };
      },
    };
  }

  getTabName(): string {
    return 'Device traces';
  }
}

class DeviceTracesMergeTab implements AreaSelectionTab {
  readonly id = 'device_traces_merge';
  readonly name = 'Merge traces';
  readonly priority = 100;

  private readonly memo = new AsyncMemo<{
    rows: DeviceTraceRow[];
    selected: Set<string>;
  }>();

  constructor(
    private readonly trace: Trace,
    private readonly counterTrackFilters: ReadonlyMap<string, string>,
    private readonly breakdownColumns: ReadonlyArray<BreakdownColumn>,
    private readonly datasetSpec: DeviceTraceDatasetSpec,
  ) {}

  render(selection: AreaSelection): ContentWithLoadingFlag | undefined {
    const dataset = selectTracksAndGetDataset(
      selection.tracks,
      this.datasetSpec,
    );
    const hasCounterTrack = selection.trackUris.some((uri) =>
      this.counterTrackFilters.has(uri),
    );
    if (dataset === undefined && !hasCounterTrack) {
      return undefined;
    }
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
    const src = buildSelectionSourceSql(
      dataset,
      selection,
      this.counterTrackFilters,
      this.breakdownColumns,
      this.datasetSpec,
    );
    const result = await this.trace.engine.query(`
      SELECT
        ${selectColumnList(this.breakdownColumns, 'ts', 'name')}
      FROM ${src}
      WHERE ts < ${selection.end} AND ts + dur > ${selection.start}
      ORDER BY ts
      LIMIT 500
    `);
    const rows: DeviceTraceRow[] = [];
    const it = result.iter(this.datasetSpec);
    for (; it.valid(); it.next()) {
      rows.push(parseDeviceTraceRow(it, this.breakdownColumns));
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
        ...this.breakdownColumns.map((col) => ({
          key: col.sqlCol,
          header: m(GridHeaderCell, col.label),
        })),
        {key: 'ts', header: m(GridHeaderCell, 'Start')},
        {key: 'dur', header: m(GridHeaderCell, 'Duration')},
        {key: 'trace_dur', header: m(GridHeaderCell, 'Trace Duration')},
        {key: 'sched_dur', header: m(GridHeaderCell, 'Sched Duration')},
        {
          key: 'trace_size_bytes',
          header: m(GridHeaderCell, 'Uncompressed Size'),
        },
        {
          key: 'original_trace_size_bytes',
          header: m(GridHeaderCell, 'Compressed Size'),
        },
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
        ...this.breakdownColumns.map((col) =>
          m(GridCell, r.cols.get(col.sqlCol) || '-'),
        ),
        m(GridCell, m(Timestamp, {trace: this.trace, ts: r.ts})),
        m(GridCell, m(DurationWidget, {trace: this.trace, dur: r.dur})),
        m(
          GridCell,
          r.traceDur > 0n
            ? m(DurationWidget, {trace: this.trace, dur: r.traceDur})
            : '-',
        ),
        m(
          GridCell,
          r.schedDur > 0n
            ? m(DurationWidget, {trace: this.trace, dur: r.schedDur})
            : '-',
        ),
        m(GridCell, formatBytes(r.traceSizeBytes)),
        m(GridCell, formatBytes(r.originalTraceSizeBytes)),
      ]),
    });
  }
}

export default class DeviceTracesPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.DeviceTraces';
  static readonly dependencies = [TrackEventPlugin];
  private readonly counterTrackFilters = new Map<string, string>();
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

    await this.loadPowerRailTracks(ctx);
    await this.loadDeviceStateTracks(ctx);
    await this.hideInternalTrackEventTracks(ctx);

    for (const cols of defaultTrees) {
      ctx.defaultWorkspace.addChildInOrder(
        this.createBreakdownTree(ctx, cols, false),
      );
    }

    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(
        ctx,
        new DeviceTracesSelectionAggregator(
          ctx,
          this.counterTrackFilters,
          this.breakdownColumns,
          this.datasetSpec,
        ),
        200,
      ),
    );
    ctx.selection.registerAreaSelectionTab(
      new DeviceTracesMergeTab(
        ctx,
        this.counterTrackFilters,
        this.breakdownColumns,
        this.datasetSpec,
      ),
    );

    ctx.commands.registerCommand({
      id: 'com.android.DeviceTraces.AddCustomBreakdown',
      name: 'Device Traces: Add custom breakdown...',
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

    const it = res.iter({
      track_id: NUM,
      description: STR,
    });
    for (; it.valid(); it.next()) {
      const parts = it.description.split('|');
      if (parts.length < 6 || parts[0] !== 'dt_power') {
        continue;
      }
      const kind = parts[1];
      const androidId = parts[2];
      const subsystemName = parts[3];
      const railName = parts[4];
      const totalMwh = Number(parts[5]) || 0;

      if (
        kind !== 'fleet_total' &&
        kind !== 'fleet_sub' &&
        kind !== 'user_total' &&
        kind !== 'user_sub' &&
        kind !== 'user_rail'
      ) {
        continue;
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
          if (list === undefined) {
            list = [];
            this.userSubTracks.set(androidId, list);
          }
          list.push(info);
          break;
        }
        case 'user_rail': {
          let list = this.userRailTracks.get(androidId);
          if (list === undefined) {
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

    const it = res.iter({
      track_id: NUM,
      description: STR,
    });
    for (; it.valid(); it.next()) {
      const parts = it.description.split('|');
      if (parts.length < 4 || parts[0] !== 'dt_state') {
        continue;
      }
      const androidId = parts[1];
      const stateTrack = parts[2];
      const sliceCount = Number(parts[3]) || 0;
      if (!androidId || !stateTrack || sliceCount <= 0) {
        continue;
      }
      let list = this.userStateTracks.get(androidId);
      if (list === undefined) {
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
        if (ra !== rb) return ra - rb;
        return a.trackName.localeCompare(b.trackName);
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
    const rootUri = `${DeviceTracesPlugin.id}#${keyPrefix}_root`;
    this.registerCounterTrack(ctx, rootUri, '1 = 1');
    const rootNode = new TrackNode({
      name: `Device Traces (${pathStr})`,
      uri: rootUri,
      removable,
      sortOrder: -10,
    });
    this.attachLazyBreakdownLevels(
      ctx,
      rootNode,
      cols,
      '1 = 1',
      keyPrefix,
      new Map<string, string>(),
    );
    return rootNode;
  }

  private attachLazyBreakdownLevels(
    ctx: Trace,
    parentNode: TrackNode,
    remainingCols: ReadonlyArray<BreakdownColumn>,
    whereClause: string,
    keyPrefix: string,
    boundValues: ReadonlyMap<string, string>,
  ): void {
    const sessionVal = boundValues.get('session_name');
    const androidIdVal = boundValues.get('android_id');
    const isBatterySession = sessionVal === BATTERY_SESSION_NAME;
    const hasUserBatteryTracks =
      remainingCols.length === 0 &&
      isBatterySession &&
      androidIdVal !== undefined &&
      (this.userTotalTracks.has(androidIdVal) ||
        this.userSubTracks.has(androidIdVal) ||
        this.userRailTracks.has(androidIdVal) ||
        this.userStateTracks.has(androidIdVal));
    const hasFleetBatteryTracks =
      isBatterySession &&
      androidIdVal === undefined &&
      (this.fleetTotalTrack !== undefined || this.fleetSubTracks.length > 0);

    if (remainingCols.length === 0 && !hasUserBatteryTracks) {
      return;
    }

    this.onFirstExpand(ctx, parentNode, async () => {
      if (hasFleetBatteryTracks && remainingCols.length > 0) {
        const nextCol = remainingCols[0];
        if (nextCol.label !== 'android_id') {
          this.appendFleetPowerRailsNode(
            ctx,
            parentNode,
            `${keyPrefix}:fleet_power`,
            whereClause,
          );
        }
      }

      if (remainingCols.length > 0) {
        const [col, ...restCols] = remainingCols;
        const isLeafLevel = restCols.length === 0;

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

          const childBound = new Map(boundValues);
          childBound.set(col.label, val);

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

          this.attachLazyBreakdownLevels(
            ctx,
            childNode,
            restCols,
            childFilter,
            childKey,
            childBound,
          );
        }
      }

      if (hasUserBatteryTracks && androidIdVal !== undefined) {
        const userFilter = `col_android_id = ${sqliteString(androidIdVal)}`;
        this.appendUserPowerRailsNode(
          ctx,
          parentNode,
          androidIdVal,
          `${keyPrefix}:power_rails`,
          userFilter,
        );
        this.appendUserDeviceStatesNode(
          ctx,
          parentNode,
          androidIdVal,
          `${keyPrefix}:device_state`,
          userFilter,
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
      groupUri = `${DeviceTracesPlugin.id}#${keyPrefix}:total`;
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
    const groupNode = new TrackNode({
      name: totalLabel,
      uri: groupUri,
      isSummary: groupUri === undefined,
    });
    parentNode.addChildLast(groupNode);

    if (fleetSubs.length > 0) {
      this.onFirstExpand(ctx, groupNode, async () => {
        for (const sub of fleetSubs) {
          const subUri = `${DeviceTracesPlugin.id}#${keyPrefix}:sub:${sub.subsystemName}`;
          this.registerPowerCounterTrack(
            ctx,
            subUri,
            sub.trackId,
            selectionFilter,
          );
          groupNode.addChildLast(
            new TrackNode({
              name: `${sub.subsystemName} (${formatEnergyMwh(sub.totalMwh)})`,
              uri: subUri,
            }),
          );
        }
      });
    }
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
      groupUri = `${DeviceTracesPlugin.id}#${keyPrefix}:total`;
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

    const powerNode = new TrackNode({
      name: groupTitle,
      uri: groupUri,
      isSummary: groupUri === undefined,
    });
    parentNode.addChildLast(powerNode);

    const railsBySubsystem = new Map<string, PowerRailTrackInfo[]>();
    for (const rail of userRails) {
      let list = railsBySubsystem.get(rail.subsystemName);
      if (list === undefined) {
        list = [];
        railsBySubsystem.set(rail.subsystemName, list);
      }
      list.push(rail);
    }

    this.onFirstExpand(ctx, powerNode, async () => {
      for (const sub of userSubs) {
        const subUri = `${DeviceTracesPlugin.id}#${keyPrefix}:sub:${sub.subsystemName}`;
        this.registerPowerCounterTrack(
          ctx,
          subUri,
          sub.trackId,
          selectionFilter,
        );
        const subRails = railsBySubsystem.get(sub.subsystemName) ?? [];
        const subNode = new TrackNode({
          name: `Subsystem: ${sub.subsystemName} (${formatEnergyMwh(sub.totalMwh)}, ${subRails.length} rails)`,
          uri: subUri,
        });
        powerNode.addChildLast(subNode);

        if (subRails.length > 0) {
          this.onFirstExpand(ctx, subNode, async () => {
            for (const rail of subRails) {
              const railUri = `${DeviceTracesPlugin.id}#${keyPrefix}:sub:${sub.subsystemName}:rail:${rail.railName}`;
              this.registerPowerCounterTrack(
                ctx,
                railUri,
                rail.trackId,
                selectionFilter,
              );
              subNode.addChildLast(
                new TrackNode({
                  name: `${rail.railName} (${formatEnergyMwh(rail.totalMwh)})`,
                  uri: railUri,
                }),
              );
            }
          });
        }
      }

      if (userRails.length > 0) {
        const allRailsNode = new TrackNode({
          name: `All Individual Rails (${userRails.length})`,
          isSummary: true,
        });
        powerNode.addChildLast(allRailsNode);

        this.onFirstExpand(ctx, allRailsNode, async () => {
          for (const rail of userRails) {
            const railUri = `${DeviceTracesPlugin.id}#${keyPrefix}:all_rails:${rail.railName}`;
            this.registerPowerCounterTrack(
              ctx,
              railUri,
              rail.trackId,
              selectionFilter,
            );
            allRailsNode.addChildLast(
              new TrackNode({
                name: `${rail.railName} [${rail.subsystemName}] (${formatEnergyMwh(rail.totalMwh)})`,
                uri: railUri,
              }),
            );
          }
        });
      }
    });
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

    const statesNode = new TrackNode({
      name: `Device State (${stateTracks.length} tracks)`,
      isSummary: true,
    });
    parentNode.addChildLast(statesNode);

    this.onFirstExpand(ctx, statesNode, async () => {
      for (const st of stateTracks) {
        const stUri = `${DeviceTracesPlugin.id}#${keyPrefix}:${st.trackName}`;
        this.registerDeviceStateSliceTrack(ctx, stUri, st, selectionFilter);
        statesNode.addChildLast(
          new TrackNode({
            name: `${st.trackName} (${st.sliceCount})`,
            uri: stUri,
          }),
        );
      }
    });
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
          SELECT
            ts,
            value
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
            SELECT
              id,
              ts,
              dur,
              depth,
              name
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

  private onFirstExpand(
    ctx: Trace,
    node: TrackNode,
    populate: () => Promise<void>,
  ): void {
    const placeholder = new TrackNode({name: 'Loading...'});
    node.addChildLast(placeholder);

    let populated = false;
    const run = async () => {
      if (populated) return;
      populated = true;
      await populate();
      placeholder.remove();
      ctx.raf.scheduleFullRedraw();
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

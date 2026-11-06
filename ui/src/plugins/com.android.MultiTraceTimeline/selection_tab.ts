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
import {Duration, Time, type time} from '../../base/time';
import {
  type Aggregation,
  type Aggregator,
  createAggregationData,
  createAggregationTab,
  type DataGridState,
  selectTracksAndGetDataset,
} from '../../components/aggregation_adapter';
import {formatDuration} from '../../components/time_utils';
import type {ColumnDef} from '../../components/widgets/datagrid/datagrid_schema';
import {filterToSql} from '../../components/widgets/datagrid/sql_utils';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {
  AreaSelection,
  AreaSelectionTab,
  ContentWithLoadingFlag,
} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {Dataset} from '../../trace_processor/dataset';
import type {Engine} from '../../trace_processor/engine';
import {STR} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {Anchor} from '../../widgets/anchor';
import {Button} from '../../widgets/button';
import {
  type BreakdownColumn,
  type DeviceTraceDatasetSpec,
  formatBytes,
  renderArgValue,
  selectColumnList,
  TABLE_NAME,
  toBigInt,
  TRACE_UUID_URL,
} from './schema';

const TRACE_UUIDS_URL = 'http://go/trace-uuids/';
const QUERY_TRACES_URL = 'http://go/query-traces/';
const DEVICE_TRACES_URL = 'http://go/device-traces/';
const POPUP_PING_INTERVAL_MS = 200;
const POPUP_TIMEOUT_MS = 2 * 60 * 1000;

const DEFAULT_QUERY_TRACES_SQL = `SELECT
  (SELECT (end_ts - start_ts) / 1000000 FROM trace_bounds) trace_dur_ms,
  (SELECT (MAX(ts) - MIN(ts)) / 1000000 FROM sched) sched_dur_ms,
  (SELECT int_value / 1000000 FROM metadata WHERE name='tracing_started_ns') time_since_boot_ms`;

function postPayloadToPopup(url: string, payload: unknown): void {
  const popup = window.open(url, '_blank');
  if (popup === null) return;

  const stop = () => {
    window.clearInterval(pingInterval);
    window.clearTimeout(timeoutId);
    window.removeEventListener('message', onMessage);
  };

  const onMessage = (e: MessageEvent) => {
    if (
      e.source !== popup ||
      e.data !== 'PONG' ||
      !e.origin.startsWith('https://')
    ) {
      return;
    }
    stop();
    popup.postMessage(payload, e.origin);
  };

  window.addEventListener('message', onMessage);
  const pingInterval = window.setInterval(() => {
    if (popup.closed) {
      stop();
      return;
    }
    popup.postMessage('PING', '*');
  }, POPUP_PING_INTERVAL_MS);
  const timeoutId = window.setTimeout(stop, POPUP_TIMEOUT_MS);
}

function openQueryTraces(traceUuids: ReadonlyArray<string>): void {
  if (traceUuids.length === 0) return;
  postPayloadToPopup(QUERY_TRACES_URL, {
    traceUuids: [...traceUuids],
    perfettoSql: DEFAULT_QUERY_TRACES_SQL,
  });
}

function openDeviceTraces(
  androidIds: ReadonlyArray<string>,
  startTs: time,
  endTs: time,
): void {
  if (androidIds.length === 0) return;
  postPayloadToPopup(DEVICE_TRACES_URL, {
    android_ids: [...androidIds],
    start_ts_ns: String(startTs),
    end_ts_ns: String(endTs),
  });
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
  const counterSql = `
    SELECT
      ${selectColumnList(breakdownColumns)}
    FROM ${TABLE_NAME}
    ${counterWhere}
  `;

  if (dataset !== undefined && matchedCounterPredicates.length > 0) {
    return `(${dataset.query(datasetSpec)} UNION ${counterSql})`;
  }
  return dataset !== undefined
    ? `(${dataset.query(datasetSpec)})`
    : `(${counterSql})`;
}

interface FilteredSelection {
  readonly traceUuids: ReadonlyArray<string>;
  readonly androidIds: ReadonlyArray<string>;
}

export class MultiTraceTimelineSelectionAggregator implements Aggregator {
  readonly id = 'multi_trace_timeline_selection';
  private currentTableName?: string;
  private unfilteredSelection: FilteredSelection = {
    traceUuids: [],
    androidIds: [],
  };

  constructor(
    private readonly trace: Trace,
    private readonly counterTrackFilters: ReadonlyMap<string, string>,
    private readonly breakdownColumns: ReadonlyArray<BreakdownColumn>,
    private readonly datasetSpec: DeviceTraceDatasetSpec,
  ) {}

  getCurrentTableName(): string | undefined {
    return this.currentTableName;
  }

  getUnfilteredSelection(): FilteredSelection {
    return this.unfilteredSelection;
  }

  async queryFilteredSelection(
    tableName: string,
    filterSqls: ReadonlyArray<string>,
  ): Promise<FilteredSelection> {
    const whereClause =
      filterSqls.length > 0 ? `WHERE ${filterSqls.join(' AND ')}` : '';
    const hasAndroidId = this.breakdownColumns.some(
      (c) => c.sqlCol === 'col_android_id',
    );
    const androidIdExpr = hasAndroidId
      ? 'col_android_id'
      : "'' AS col_android_id";
    const res = await this.trace.engine.query(`
      SELECT
        trace_uuid,
        ${androidIdExpr}
      FROM ${tableName}
      ${whereClause}
    `);
    const traceUuidSet = new Set<string>();
    const androidIdSet = new Set<string>();
    const it = res.iter({
      trace_uuid: STR,
      col_android_id: STR,
    });
    for (; it.valid(); it.next()) {
      const uuid = it.trace_uuid.trim();
      if (uuid) traceUuidSet.add(uuid);
      const androidId = it.col_android_id.trim();
      if (androidId) androidIdSet.add(androidId);
    }
    return {
      traceUuids: [...traceUuidSet],
      androidIds: [...androidIdSet],
    };
  }

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
        this.currentTableName = table.name;
        this.unfilteredSelection = await this.queryFilteredSelection(
          table.name,
          [],
        );
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
              if (!this.trace.isInternalUser) return uuid;
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
            cellRenderer: (value) => {
              const text =
                typeof value === 'string' ? value : String(value ?? '');
              return renderArgValue(this.trace, col.label, text, '');
            },
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
    return 'Multi-trace timeline';
  }
}

export function createMultiTraceTimelineTab(
  trace: Trace,
  aggregator: MultiTraceTimelineSelectionAggregator,
): AreaSelectionTab {
  const baseTab = createAggregationTab(trace, aggregator, 200);
  const filteredSlot = new AsyncMemo<FilteredSelection>();

  return {
    id: baseTab.id,
    name: baseTab.name,
    priority: baseTab.priority,
    render(selection: AreaSelection): ContentWithLoadingFlag | undefined {
      const res = baseTab.render(selection);
      if (res === undefined || res.isLoading || !trace.isInternalUser) {
        return res;
      }
      const tableName = aggregator.getCurrentTableName();
      const panelVnode = res.content as
        m.Vnode<{readonly dataGridState?: DataGridState}> | undefined;
      const filters = panelVnode?.attrs?.dataGridState?.filters ?? [];
      const filterSqls = filters.map((f) => filterToSql(f, f.field));

      const filtered =
        filterSqls.length === 0 || tableName === undefined
          ? aggregator.getUnfilteredSelection()
          : (filteredSlot.use({
              key: {tableName, filterSqls},
              compute: () =>
                aggregator.queryFilteredSelection(tableName, filterSqls),
            }).data ?? aggregator.getUnfilteredSelection());

      const {traceUuids, androidIds} = filtered;

      return {
        isLoading: false,
        content: res.content,
        buttons: [
          m(Button, {
            label: `Merge traces (${traceUuids.length})`,
            icon: Icons.ExternalLink,
            disabled: traceUuids.length === 0,
            onclick: () => {
              if (traceUuids.length > 0) {
                window.open(TRACE_UUIDS_URL + traceUuids.join(','), '_blank');
              }
            },
          }),
          m(Button, {
            label: `Query traces (${traceUuids.length})`,
            icon: Icons.ExternalLink,
            disabled: traceUuids.length === 0,
            onclick: () => {
              if (traceUuids.length > 0) {
                openQueryTraces(traceUuids);
              }
            },
          }),
          m(Button, {
            label: `All traces for devices (${androidIds.length})`,
            icon: Icons.ExternalLink,
            disabled: androidIds.length === 0,
            onclick: () => {
              if (androidIds.length > 0) {
                openDeviceTraces(androidIds, selection.start, selection.end);
              }
            },
          }),
          res.buttons,
        ],
      };
    },
  };
}

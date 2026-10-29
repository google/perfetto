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
import {Duration, type duration, Time, type time} from '../../base/time';
import {Timestamp} from '../../components/widgets/timestamp';
import type {Trace} from '../../public/trace';
import type {DatasetSchema} from '../../trace_processor/dataset';
import {
  LONG,
  NUM,
  type RowIterator,
  type SqlValue,
  STR,
} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';

export const TRACE_UUID_URL = 'http://go/trace-uuid/';
export const TABLE_NAME = '_device_traces';

export const EXCLUDED_BREAKDOWN_ARGS = new Set([
  'dur_ns',
  'armed_start_ts',
  'armed_dur_ns',
  'trace_uuid',
  'trace_size_bytes',
  'original_trace_size_bytes',
  'trace_dur_ns',
  'sched_dur_ns',
]);

export interface BreakdownColumn {
  readonly label: string;
  readonly sqlCol: string;
}

export interface PowerRailTrackInfo {
  readonly trackId: number;
  readonly kind:
    'fleet_total' | 'fleet_sub' | 'user_total' | 'user_sub' | 'user_rail';
  readonly androidId: string;
  readonly subsystemName: string;
  readonly railName: string;
  readonly totalMwh: number;
}

export interface DeviceStateTrackInfo {
  readonly trackId: number;
  readonly androidId: string;
  readonly trackName: string;
  readonly sliceCount: number;
}

export const DEVICE_STATE_SPEC = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  depth: NUM,
  name: STR,
};

export const DEVICE_TRACE_BASE_SPEC = {
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

export type DeviceTraceDatasetSpec = DatasetSchema &
  typeof DEVICE_TRACE_BASE_SPEC;

export interface DeviceTraceRow {
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

export function buildDatasetSpec(
  cols: ReadonlyArray<BreakdownColumn>,
): DeviceTraceDatasetSpec {
  const spec: DatasetSchema = {...DEVICE_TRACE_BASE_SPEC};
  for (const c of cols) {
    spec[c.sqlCol] = STR;
  }
  return spec as DeviceTraceDatasetSpec;
}

export function selectColumnList(
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

export function parseDeviceTraceRow(
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

export function formatEnergyMwh(mwh: number): string {
  if (!Number.isFinite(mwh) || mwh <= 0) {
    return '0 mWh';
  }
  if (mwh >= 1000) {
    return `${(mwh / 1000).toFixed(2)} Wh`;
  }
  return `${mwh.toFixed(1)} mWh`;
}

export function formatBytes(bytes: bigint): string {
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

export function toBigInt(value: SqlValue): bigint | undefined {
  if (typeof value === 'bigint') {
    return value;
  }
  if (typeof value === 'number' && Number.isFinite(value)) {
    return BigInt(Math.round(value));
  }
  return undefined;
}

export function renderArgValue(
  trace: Trace,
  label: string,
  value: string,
  emptyFallback = '-',
): m.Children {
  if (!value) return emptyFallback;
  if (/^https?:\/\//.test(value)) {
    return m(Anchor, {href: value, target: '_blank'}, value);
  }
  if (label.endsWith('_ts_ns') && /^\d+$/.test(value)) {
    const raw = BigInt(value);
    if (raw > 0n) {
      return m(Timestamp, {trace, ts: Time.fromRaw(raw)});
    }
  }
  return value;
}

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

import {Verdict, TRACE_VIEWER_BASE_URL} from '../models/types';

export interface ExportRow {
  trace_uuid: string;
  package_name: string;
  startup_dur: number;
  tab_name: string;
  verdict: string;
  link: string;
  [key: string]: unknown;
}

export interface ExportableTrace {
  trace_uuid: string;
  package_name: string;
  startup_dur: number;
  extra?: Record<string, unknown>;
}

const EXCLUDED_EXTRA = new Set([
  'slices',
  'quantized_sequence',
  'quantized_sequence_json',
  'quantized_sequence_base64',
]);

export function buildTraceLink(uuid: string, packageName?: string): string {
  if (!uuid) return '';
  let url = `${TRACE_VIEWER_BASE_URL}?uuid=${uuid}`;
  if (packageName) {
    url += `&query=${encodeURIComponent(`com.android.AndroidStartup.packageName=${packageName}`)}`;
  }
  return url;
}

function verdictLabel(v: Verdict | undefined): string {
  if (v === 'like') return 'positive';
  if (v === 'dislike') return 'negative';
  if (v === 'discard') return 'discarded';
  return 'pending';
}

export function traceExportRow(
  trace: ExportableTrace,
  traceKey: string,
  tabName: string,
  verdicts: Map<string, Verdict>,
): ExportRow {
  const row: ExportRow = {
    trace_uuid: trace.trace_uuid,
    package_name: trace.package_name,
    startup_dur: trace.startup_dur,
    tab_name: tabName,
    verdict: verdictLabel(verdicts.get(traceKey)),
    link: buildTraceLink(trace.trace_uuid, trace.package_name),
  };
  if (trace.extra) {
    for (const [k, v] of Object.entries(trace.extra)) {
      if (!EXCLUDED_EXTRA.has(k)) row[k] = v;
    }
  }
  return row;
}

const FIXED_COLS = [
  'trace_uuid',
  'package_name',
  'startup_dur',
  'tab_name',
  'verdict',
  'link',
];

function tsvEscape(v: unknown): string {
  if (v == null) return '';
  if (typeof v === 'object') {
    return JSON.stringify(v).replace(/[\t\n\r]/g, ' ');
  }
  return String(v).replace(/[\t\n\r]/g, ' ');
}

export function rowsToTsv(rows: ExportRow[]): string {
  if (rows.length === 0) return '';
  const extraCols = new Set<string>();
  for (const row of rows) {
    for (const k of Object.keys(row)) {
      if (!FIXED_COLS.includes(k)) extraCols.add(k);
    }
  }
  const cols = [...FIXED_COLS, ...[...extraCols].sort()];
  const header = cols.join('\t');
  const lines = rows.map((row) =>
    cols.map((c) => tsvEscape(row[c])).join('\t'),
  );
  return header + '\n' + lines.join('\n');
}

export function rowsToJson(rows: ExportRow[]): string {
  return JSON.stringify(rows, null, 2);
}

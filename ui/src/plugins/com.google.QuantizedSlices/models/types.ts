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

export interface Slice {
  ts: number;
  dur: number;
  name: string | null;
  state: string | null;
  depth: number | null;
  io_wait: number | null;
  blocked_function: string | null;
}

export interface MergedSlice extends Slice {
  tsRel: number;
  _merged: number;
}

export interface TraceEntry {
  trace_uuid: string;
  package_name: string;
  startup_dur: number;
  slices: Slice[];
  extra?: Record<string, unknown>;
}

export type Verdict = 'like' | 'dislike' | 'discard';

export type OverviewFilter =
  | 'all'
  | 'positive'
  | 'negative'
  | 'pending'
  | 'discarded';

export interface SortState {
  col: string;
  dir: 1 | -1;
}

export interface SummaryRow {
  label: string;
  short: string;
  dur: number;
  count: number;
  color: string;
  pct: number;
}

export interface ColumnConfig {
  trace_uuid: {aliases: string[]; fallback: {factory: () => string}};
  package_name: {aliases: string[]; fallback: {factory: () => string}};
  startup_dur: {aliases: string[]; fallback: {factory: () => number}};
  slices: {aliases: string[]; fallback: {factory: () => Slice[]}};
}

export const DEFAULT_COLUMN_CONFIG: ColumnConfig = {
  trace_uuid: {
    aliases: ['trace_uuid', 'uuid', 'id', 'trace_id', 'trace_address'],
    fallback: {factory: () => crypto.randomUUID()},
  },
  package_name: {
    aliases: [
      'package_name',
      'process_name',
      'process',
      'package',
      'pkg',
      'app',
    ],
    fallback: {factory: () => 'unknown'},
  },
  startup_dur: {
    aliases: [
      'startup_dur',
      'startup_dur_ms',
      'startup_duration',
      'dur',
      'duration',
      'total_dur',
      'startup_ms',
    ],
    fallback: {factory: () => 0},
  },
  slices: {
    aliases: [
      'slices',
      'quantized_sequence',
      'quantized_sequence_json',
      'json',
      'data',
      'trace_data',
      'base64',
      'thread_slices',
    ],
    fallback: {factory: () => []},
  },
};

export interface SliceFieldConfig {
  ts: {aliases: string[]; fallback: number};
  dur: {aliases: string[]; fallback: number};
  name: {aliases: string[]; fallback: string | null};
  state: {aliases: string[]; fallback: string | null};
  depth: {aliases: string[]; fallback: number | null};
  io_wait: {aliases: string[]; fallback: number | null};
  blocked_function: {aliases: string[]; fallback: string | null};
}

export const DEFAULT_SLICE_FIELD_CONFIG: SliceFieldConfig = {
  ts: {aliases: ['ts', 'timestamp', 'start', 'start_ts', 'begin'], fallback: 0},
  dur: {aliases: ['dur', 'duration', 'length'], fallback: 0},
  name: {aliases: ['name', 'slice_name', 'label', 'event'], fallback: null},
  state: {aliases: ['state', 'thread_state', 'sched_state'], fallback: null},
  depth: {aliases: ['depth', 'level', 'stack_depth'], fallback: null},
  io_wait: {aliases: ['io_wait', 'iowait', 'io'], fallback: null},
  blocked_function: {
    aliases: ['blocked_function', 'blocked_fn', 'blocked', 'wchan'],
    fallback: null,
  },
};

// Shared constants used by multiple components.
export const LONG_PKG_PREFIX =
  'com.redfin.android.core.activity.launch.deeplink.';

// Base URL for the trace viewer. Change this to point to a different viewer.
export const TRACE_VIEWER_BASE_URL =
  'https://apconsole.corp.google.com/link/perfetto/field_traces';

// Base URL for the Brush tool.
export const BRUSH_BASE_URL = 'https://brush.corp.google.com/';

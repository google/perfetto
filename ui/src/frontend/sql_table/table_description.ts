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


// Definition of the SQL table to be displayed in the SQL table widget,
// including the semantic definitions of the columns (e.g. timestamp
// column which requires special formatting). Also note that some of the
// columns require other columns for advanced display features (e.g. timestamp
// and duration taken together define "a time range", which can be used for
// additional filtering.

export type DisplayConfig =
    SliceIdDisplayConfig|Timestamp|Duration|ThreadDuration;

// Common properties for all columns.
interface SqlTableColumnBase {
  // Name of the column in the SQL table.
  name: string;
  // Display name of the column in the UI.
  title?: string;
}

export interface ArgSetIdColumn extends SqlTableColumnBase {
  type: 'arg_set_id';
}

export interface RegularSqlTableColumn extends SqlTableColumnBase {
  // Special rendering instructions for this column, including the list
  // of additional columns required for the rendering.
  display?: DisplayConfig;
  // Whether the column should be hidden by default.
  startsHidden?: boolean;
}

export type SqlTableColumn = RegularSqlTableColumn|ArgSetIdColumn;

export function startsHidden(c: SqlTableColumn): boolean {
  if (isArgSetIdColumn(c)) return true;
  return c.startsHidden ?? false;
}

export function isArgSetIdColumn(c: SqlTableColumn): c is ArgSetIdColumn {
  return (c as {type?: string}).type === 'arg_set_id';
}

export interface SqlTableDescription {
  readonly imports?: string[];
  name: string;
  displayName?: string;
  columns: SqlTableColumn[];
}

export function tableDisplayName(table: SqlTableDescription): string {
  return table.displayName ?? table.name;
}

// Additional columns needed to display the given column.
export function dependendentColumns(display?: DisplayConfig): string[] {
  switch (display?.type) {
    case 'slice_id':
      return [display.ts, display.dur, display.trackId];
    default:
      return [];
  }
}

// Column displaying ids into the `slice` table. Requires the ts, dur and
// track_id columns to be able to display the value, including the
// "go-to-slice-on-click" functionality.
export interface SliceIdDisplayConfig {
  type: 'slice_id';
  ts: string;
  dur: string;
  trackId: string;
}

// Column displaying timestamps.
interface Timestamp {
  type: 'timestamp';
}

// Column displaying durations.
export interface Duration {
  type: 'duration';
}

// Column displaying thread durations.
export interface ThreadDuration {
  type: 'thread_duration';
}

// Copyright (C) 2024 The Android Open Source Project
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

import {PerfettoSqlType} from '../../../../trace_processor/perfetto_sql_type';
import {TableColumn} from './table_column';

// Describes a column derived from another table via a join.
export interface DerivedColumn {
  readonly column: string;
  readonly source: {
    readonly table: string;
    readonly joinOn: Record<string, string>;
  };
}

// Raw column definition - just data, no rendering logic or Trace dependency.
export interface ColumnDefinition {
  readonly column: string | DerivedColumn;
  readonly type?: PerfettoSqlType;
  readonly startsHidden?: boolean;
}

// Raw table definition - just data, no rendering logic or Trace dependency.
// Use resolveTableDefinition() to convert to SqlTableDescription.
export interface SqlTableDefinition {
  readonly imports?: string[];
  readonly prefix?: string; // prefix for ctes
  readonly name: string;
  // In some cases, the name of the table we are querying is different from the
  // name of the table we want to display to the user -- typically because the
  // underlying table is wrapped into a view.
  readonly displayName?: string;
  readonly columns: ColumnDefinition[];
}

export interface SqlTableDescription {
  readonly imports?: string[];
  readonly prefix?: string; // prefix for ctes
  name: string;
  // In some cases, the name of the table we are querying is different from the name of the table we want to display to the user -- typically because the underlying table is wrapped into a view.
  displayName?: string;
  columns: TableColumn[];
}

// Copyright (C) 2025 The Android Open Source Project
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

import {SqlValue} from '../../../trace_processor/query_result';

export interface ColumnDefinition {
  readonly name: string;
}

export interface FilterValue {
  readonly column: string;
  readonly op: '=' | '!=' | '<' | '<=' | '>' | '>=' | 'glob';
  readonly value: SqlValue;
}

export interface FilterNull {
  readonly column: string;
  readonly op: 'is null' | 'is not null';
}

export type FilterDefinition = FilterValue | FilterNull;

export interface SortByColumn {
  readonly column: string;
  readonly direction: 'asc' | 'desc';
}

export interface Unsorted {
  readonly direction: 'unsorted';
}

export type SortBy = SortByColumn | Unsorted;

export interface DataSourceResult {
  readonly totalRows: number;
  readonly rowOffset: number;
  readonly rows: ReadonlyArray<RowDef>;
}

export type RowDef = {[key: string]: SqlValue};

export interface DataGridDataSource {
  readonly rows: DataSourceResult;
  notifyUpdate(
    sortBy: SortBy,
    filters: ReadonlyArray<FilterDefinition>,
    rowOffset: number,
    rowLimit: number,
  ): void;
}

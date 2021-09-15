// Copyright (C) 2021 The Android Open Source Project
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

import {Row} from './query_result';

export const AVAILABLE_TABLES = ['slice'];
export const AVAILABLE_AGGREGATIONS = ['COUNT', 'SUM', 'AVG', 'MIN', 'MAX'];
export const WHERE_FILTERS = ['slice.dur != -1'];

export interface AggregationAttrs {
  tableName: string;
  columnName: string;
  aggregation: string;
  order: string;
}

export interface PivotAttrs {
  tableName: string;
  columnName: string;
}

export interface TableAttrs {
  tableName: string;
  columns: string[];
}

export interface ColumnAttrs {
  name: string;
  index: number;
  tableName: string;
  columnName: string;
  aggregation?: string;
  order?: string;
}

export interface RowAttrs {
  row: Row;
  isExpanded: boolean;
  expandableColumn?: string;  // Column at which the row can be expanded.
  rows?:
      RowAttrs[];  // Contains the expanded rows, set after the row is expanded.
  whereFilter?: string;  // Where filter that joins the row with its parent.
  isLoadingQuery: boolean;
}

export interface SubQueryAttrs {
  rowIndices: number[];
  columnIdx: number;
  value: string;
}

export interface PivotTableQueryResponse {
  columns: ColumnAttrs[];
  error?: string;
  durationMs: number;
  rows: RowAttrs[];
}

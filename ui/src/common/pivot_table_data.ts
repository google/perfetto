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

export interface PivotTableQueryResponse {
  columns: ColumnAttrs[];
  rows: Row[];
  error?: string;
  durationMs: number;
}
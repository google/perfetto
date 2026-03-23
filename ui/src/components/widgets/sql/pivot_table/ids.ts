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

import {sqlColumnId} from '../table/sql_column';
import type {TableColumn} from '../table/table_column';
import type {Aggregation} from './aggregations';

// Unique identifier for a pivot column.
export function pivotId(p: TableColumn): string {
  return sqlColumnId(p.column);
}

// Unique identifier for an aggregation.
// The column is always included to prevent ID collisions: expanding a non-associative
// aggregation (e.g. average(dur) -> sum(dur) + count(dur)) must not produce a
// count(dur) that collides with the built-in count(1), otherwise values are
// accumulated twice, inflating counts 2x per row.
export function aggregationId(a: Aggregation): string {
  return `${a.op}(${sqlColumnId(a.column.column)})`;
}

// Human-readable label for an aggregation, used in column headers.
// Returns 'count' for the built-in count(1) instead of the full 'count(1)'.
export function aggregationLabel(a: Aggregation): string {
  const id = aggregationId(a);
  return id === 'count(1)' ? 'count' : id;
}

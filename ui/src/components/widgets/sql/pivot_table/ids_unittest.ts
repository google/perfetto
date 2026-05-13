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

import {SimpleColumn} from '../table/simple_column';
import {SqlExpression} from '../table/sql_column';
import {expandAggregations, type Aggregation} from './aggregations';
import {aggregationId, aggregationLabel} from './ids';

// The built-in count(1) aggregation, as constructed by PivotTableState.
function builtinCount(): Aggregation {
  return {
    op: 'count',
    column: new SimpleColumn(new SqlExpression(() => '1', [])),
  };
}

// A count aggregation over a named column, as produced by expandAggregations
// when decomposing average(dur).
function countOfDur(): Aggregation {
  return {
    op: 'count',
    column: new SimpleColumn('dur'),
  };
}

test('ids.aggregationId_builtin_count', () => {
  expect(aggregationId(builtinCount())).toBe('count(1)');
});

test('ids.aggregationId_count_of_column', () => {
  expect(aggregationId(countOfDur())).toBe('count(dur)');
});

// Regression test for https://github.com/google/perfetto/issues/4671.
// Adding average(dur) caused count to inflate 2x per row because
// expandAggregations produces count(dur) alongside the built-in count(1),
// and both used to share the same ID 'count'.
test('ids.aggregationId_no_collision_between_builtin_count_and_expanded_average', () => {
  const averageDur: Aggregation = {
    op: 'average',
    column: new SimpleColumn('dur'),
  };
  const expanded = expandAggregations([builtinCount(), averageDur]);
  const ids = expanded.map(aggregationId);
  // All expanded aggregation IDs must be unique.
  expect(new Set(ids).size).toBe(ids.length);
  // Built-in count(1) and the count derived from average(dur) must be distinct.
  expect(ids).toContain('count(1)');
  expect(ids).toContain('count(dur)');
});

test('ids.aggregationLabel_shows_count_for_builtin', () => {
  // The built-in count column should display as 'count', not 'count(1)'.
  expect(aggregationLabel(builtinCount())).toBe('count');
});

test('ids.aggregationLabel_shows_full_id_for_other_aggregations', () => {
  expect(aggregationLabel(countOfDur())).toBe('count(dur)');
  const sum: Aggregation = {op: 'sum', column: new SimpleColumn('dur')};
  expect(aggregationLabel(sum)).toBe('sum(dur)');
});

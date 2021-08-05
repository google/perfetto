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

import {
  AggregationAttrs,
  PivotAttrs,
  PivotTableQueryGenerator
} from './pivot_table_query_generator';

test('Generate query with pivots and aggregations', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: 'type'},
    {tableName: 'slice', columnName: 'id'}
  ];
  const selectedAggregations: AggregationAttrs[] = [
    {aggregation: 'SUM', tableName: 'slice', columnName: 'dur', order: 'DESC'}
  ];
  const expectedQuery = '\nSELECT\n' +
      'slice_type,\n' +
      '  slice_id,\n' +
      '  SUM_slice_dur_1,\n' +
      '  SUM_slice_dur_2\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice_type,\n' +
      '  slice_id,\n' +
      '  SUM(SUM_slice_dur) OVER (PARTITION BY slice_type) AS SUM_slice_dur_1' +
      ',\n' +
      '  SUM(SUM_slice_dur) OVER (PARTITION BY slice_type,  slice_id)' +
      ' AS SUM_slice_dur_2\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.type AS slice_type,\n' +
      '  slice.id AS slice_id,\n' +
      '  slice.dur AS SUM_slice_dur\n' +
      'FROM slice WHERE slice.dur != -1\n' +
      ')\n' +
      ')\n' +
      'GROUP BY 1,  2,  3,  4\n' +
      'ORDER BY 3 DESC,  4 DESC\n';
  expect(pivotTableQueryGenerator.generateQuery(
             selectedPivots, selectedAggregations))
      .toEqual(expectedQuery);
});

test('Generate query with pivots', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: 'type'},
    {tableName: 'slice', columnName: 'id'}
  ];
  const selectedAggregations: AggregationAttrs[] = [];
  const expectedQuery = '\nSELECT\n' +
      'slice_type,\n' +
      '  slice_id\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.type AS slice_type,\n' +
      '  slice.id AS slice_id\n' +
      'FROM slice WHERE slice.dur != -1\n' +
      ')\n' +
      'GROUP BY 1,  2\n';
  expect(pivotTableQueryGenerator.generateQuery(
             selectedPivots, selectedAggregations))
      .toEqual(expectedQuery);
});

test('Generate query with aggregations', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [];
  const selectedAggregations: AggregationAttrs[] = [
    {aggregation: 'SUM', tableName: 'slice', columnName: 'dur', order: 'DESC'},
    {aggregation: 'MAX', tableName: 'slice', columnName: 'dur', order: 'ASC'}
  ];
  const expectedQuery = '\nSELECT\n' +
      'SUM_slice_dur,\n' +
      '  MAX_slice_dur\n' +
      'FROM (\n' +
      'SELECT\n' +
      'SUM(SUM_slice_dur) AS SUM_slice_dur,\n' +
      '  MAX(MAX_slice_dur) AS MAX_slice_dur\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.dur AS SUM_slice_dur,\n' +
      '  slice.dur AS MAX_slice_dur\n' +
      'FROM slice WHERE slice.dur != -1\n' +
      ')\n' +
      ')\n' +
      'GROUP BY 1,  2\n' +
      'ORDER BY 1 DESC,  2 ASC\n';
  expect(pivotTableQueryGenerator.generateQuery(
             selectedPivots, selectedAggregations))
      .toEqual(expectedQuery);
});
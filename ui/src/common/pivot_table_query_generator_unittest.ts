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
  WHERE_FILTERS,
} from './pivot_table_data';
import {PivotTableQueryGenerator} from './pivot_table_query_generator';

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
      '"slice type",\n' +
      '  "slice id",\n' +
      '  "slice dur (SUM)"\n' +
      'FROM (\n' +
      'SELECT\n' +
      '"slice type",\n' +
      '  "slice id",\n' +
      '  SUM("slice dur (SUM)") OVER (PARTITION BY "slice type",  "slice id")' +
      ' AS "slice dur (SUM)"\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.type AS "slice type",\n' +
      '  slice.id AS "slice id",\n' +
      '  slice.dur AS "slice dur (SUM)"\n' +
      'FROM slice\n' +
      'WHERE\n' +
      'slice.dur != -1\n' +
      ')\n' +
      ')\n' +
      'GROUP BY 1,  2,  3\n' +
      'ORDER BY 3 DESC\n';
  expect(pivotTableQueryGenerator.generateQuery(
             selectedPivots, selectedAggregations, WHERE_FILTERS))
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
      '"slice type",\n' +
      '  "slice id"\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.type AS "slice type",\n' +
      '  slice.id AS "slice id"\n' +
      'FROM slice\n' +
      'WHERE\n' +
      'slice.dur != -1\n' +
      ')\n' +
      'GROUP BY 1,  2\n';
  expect(pivotTableQueryGenerator.generateQuery(
             selectedPivots, selectedAggregations, WHERE_FILTERS))
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
      '"slice dur (SUM)",\n' +
      '  "slice dur (MAX)"\n' +
      'FROM (\n' +
      'SELECT\n' +
      'SUM("slice dur (SUM)") AS "slice dur (SUM)",\n' +
      '  MAX("slice dur (MAX)") AS "slice dur (MAX)"\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.dur AS "slice dur (SUM)",\n' +
      '  slice.dur AS "slice dur (MAX)"\n' +
      'FROM slice\n' +
      'WHERE\n' +
      'slice.dur != -1\n' +
      ')\n' +
      ')\n' +
      'GROUP BY 1,  2\n' +
      'ORDER BY 1 DESC,  2 ASC\n';
  expect(pivotTableQueryGenerator.generateQuery(
             selectedPivots, selectedAggregations, WHERE_FILTERS))
      .toEqual(expectedQuery);
});
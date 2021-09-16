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
  SLICE_STACK_COLUMN,
  WHERE_FILTERS,
} from './pivot_table_common';
import {PivotTableQueryGenerator} from './pivot_table_query_generator';

const TABLES = ['slice'];

test('Generate query with pivots and aggregations', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: 'type', isStackPivot: false},
    {tableName: 'slice', columnName: 'id', isStackPivot: false}
  ];
  const selectedAggregations: AggregationAttrs[] = [
    {aggregation: 'SUM', tableName: 'slice', columnName: 'dur', order: 'DESC'}
  ];
  const expectedQuery = '\nSELECT\n' +
      '"slice type",\n' +
      '  "slice id",\n' +
      '  "slice dur (SUM)",\n' +
      '  "slice dur (SUM) (total)"\n' +
      'FROM (\n' +
      'SELECT\n' +
      '"slice type",\n' +
      '  "slice id",\n' +
      '  SUM("slice dur (SUM)") OVER () AS "slice dur (SUM) (total)",\n' +
      '  SUM("slice dur (SUM)") OVER (PARTITION BY "slice type",  "slice id")' +
      ' AS "slice dur (SUM)"\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.type AS "slice type",\n' +
      '  slice.id AS "slice id",\n' +
      '  slice.dur AS "slice dur (SUM)"\n' +
      'FROM\n' +
      'slice\n' +
      'WHERE\n' +
      'slice.dur != -1\n' +
      ')\n' +
      ')\n' +
      'GROUP BY "slice type",  "slice id",  "slice dur (SUM)"\n' +
      'ORDER BY "slice dur (SUM)" DESC\n';
  expect(pivotTableQueryGenerator.generateQuery(
             selectedPivots, selectedAggregations, WHERE_FILTERS, TABLES))
      .toEqual(expectedQuery);
});

test('Generate query with pivots', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: 'type', isStackPivot: false},
    {tableName: 'slice', columnName: 'id', isStackPivot: false}
  ];
  const selectedAggregations: AggregationAttrs[] = [];
  const expectedQuery = '\nSELECT\n' +
      '"slice type",\n' +
      '  "slice id"\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.type AS "slice type",\n' +
      '  slice.id AS "slice id"\n' +
      'FROM\n' +
      'slice\n' +
      'WHERE\n' +
      'slice.dur != -1\n' +
      ')\n' +
      'GROUP BY "slice type",  "slice id"\n';
  expect(pivotTableQueryGenerator.generateQuery(
             selectedPivots, selectedAggregations, WHERE_FILTERS, TABLES))
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
      'FROM\n' +
      'slice\n' +
      'WHERE\n' +
      'slice.dur != -1\n' +
      ')\n' +
      ')\n' +
      'GROUP BY "slice dur (SUM)",  "slice dur (MAX)"\n' +
      'ORDER BY "slice dur (SUM)" DESC,  "slice dur (MAX)" ASC\n';
  expect(pivotTableQueryGenerator.generateQuery(
             selectedPivots, selectedAggregations, WHERE_FILTERS, TABLES))
      .toEqual(expectedQuery);
});

test('Generate a query with stack pivot', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: SLICE_STACK_COLUMN, isStackPivot: true},
    {tableName: 'slice', columnName: 'category', isStackPivot: false}
  ];
  const selectedAggregations: AggregationAttrs[] = [
    {aggregation: 'COUNT', tableName: 'slice', columnName: 'id', order: 'DESC'},
  ];
  const expectedQuery = '\nSELECT\n' +
      '"slice name (stack)",\n' +
      '  "slice depth (hidden)",\n' +
      '  "slice stack_id (hidden)",\n' +
      '  "slice parent_stack_id (hidden)",\n' +
      '  "slice category",\n' +
      '  "slice id (COUNT)",\n' +
      '  "slice id (COUNT) (total)"\n' +
      'FROM (\n' +
      'SELECT\n' +
      '"slice name (stack)",\n' +
      '  "slice depth (hidden)",\n' +
      '  "slice stack_id (hidden)",\n' +
      '  "slice parent_stack_id (hidden)",\n' +
      '  "slice category",\n' +
      '  COUNT("slice id (COUNT)") OVER () AS "slice id (COUNT) (total)",\n' +
      '  COUNT("slice id (COUNT)") OVER (PARTITION BY' +
      ' "slice stack_id (hidden)",  "slice category") AS "slice id (COUNT)"\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.name AS "slice name (stack)",\n' +
      '  slice.depth AS "slice depth (hidden)",\n' +
      '  slice.stack_id AS "slice stack_id (hidden)",\n' +
      '  slice.parent_stack_id AS "slice parent_stack_id (hidden)",\n' +
      '  slice.category AS "slice category",\n' +
      '  slice.id AS "slice id (COUNT)"\n' +
      'FROM\n' +
      'slice\n' +
      'WHERE\n' +
      'slice.dur != -1\n' +
      ')\n' +
      ')\n' +
      'GROUP BY "slice name (stack)",  "slice depth (hidden)",  ' +
      '"slice stack_id (hidden)",  "slice parent_stack_id (hidden)",  ' +
      '"slice category",  "slice id (COUNT)"\n' +
      'ORDER BY "slice id (COUNT)" DESC\n';
  expect(pivotTableQueryGenerator.generateQuery(
             selectedPivots, selectedAggregations, WHERE_FILTERS, TABLES))
      .toEqual(expectedQuery);
});

test('Generate a descendant stack query', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: SLICE_STACK_COLUMN, isStackPivot: true},
  ];
  const selectedAggregations: AggregationAttrs[] = [];
  const expectedQuery = '\nSELECT\n' +
      '"slice name (stack)",\n' +
      '  "slice depth (hidden)",\n' +
      '  "slice stack_id (hidden)",\n' +
      '  "slice parent_stack_id (hidden)"\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.name AS "slice name (stack)",\n' +
      '  slice.depth AS "slice depth (hidden)",\n' +
      '  slice.stack_id AS "slice stack_id (hidden)",\n' +
      '  slice.parent_stack_id AS "slice parent_stack_id (hidden)"\n' +
      'FROM\n' +
      'descendant_slice_by_stack(stack_id) AS slice\n' +
      'WHERE\n' +
      'slice.dur != -1\n' +
      ')\n' +
      'GROUP BY "slice name (stack)",  "slice depth (hidden)",  ' +
      '"slice stack_id (hidden)",  "slice parent_stack_id (hidden)"\n' +
      'ORDER BY "slice depth (hidden)" ASC\n';

  const table = ['descendant_slice_by_stack(stack_id) AS slice'];
  expect(pivotTableQueryGenerator.generateStackQuery(
             selectedPivots,
             selectedAggregations,
             WHERE_FILTERS,
             table,
             /* stack_id = */ 'stack_id'))
      .toEqual(expectedQuery);
});

test('Generate a descendant stack query with another pivot', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: SLICE_STACK_COLUMN, isStackPivot: true},
    {tableName: 'slice', columnName: 'category', isStackPivot: false}
  ];
  const selectedAggregations: AggregationAttrs[] = [];
  const expectedQuery = '\nSELECT\n' +
      '"slice name (stack)",\n' +
      '  "slice depth (hidden)",\n' +
      '  "slice stack_id (hidden)",\n' +
      '  "slice parent_stack_id (hidden)",\n' +
      '  "slice category"\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.name AS "slice name (stack)",\n' +
      '  slice.depth AS "slice depth (hidden)",\n' +
      '  slice.stack_id AS "slice stack_id (hidden)",\n' +
      '  slice.parent_stack_id AS "slice parent_stack_id (hidden)",\n' +
      '  slice.category AS "slice category"\n' +
      'FROM\n' +
      'slice\n' +
      'WHERE\n' +
      'slice.dur != -1 AND\n' +
      '  slice.stack_id = stack_id\n' +
      ')\n' +
      'GROUP BY "slice name (stack)",  "slice depth (hidden)",  ' +
      '"slice stack_id (hidden)",  "slice parent_stack_id (hidden)",  ' +
      '"slice category"\n' +
      ' UNION ALL \n' +
      'SELECT\n' +
      '"slice name (stack)",\n' +
      '  "slice depth (hidden)",\n' +
      '  "slice stack_id (hidden)",\n' +
      '  "slice parent_stack_id (hidden)",\n' +
      '  "slice category"\n' +
      'FROM (\n' +
      'SELECT\n' +
      'slice.name AS "slice name (stack)",\n' +
      '  slice.depth AS "slice depth (hidden)",\n' +
      '  slice.stack_id AS "slice stack_id (hidden)",\n' +
      '  slice.parent_stack_id AS "slice parent_stack_id (hidden)",\n' +
      '  slice.category AS "slice category"\n' +
      'FROM\n' +
      'descendant_slice_by_stack(stack_id) AS slice\n' +
      'WHERE\n' +
      'slice.dur != -1\n' +
      ')\n' +
      'GROUP BY "slice name (stack)",  "slice depth (hidden)",  ' +
      '"slice stack_id (hidden)",  "slice parent_stack_id (hidden)",  ' +
      '"slice category"\n' +
      'ORDER BY "slice depth (hidden)" ASC\n';

  const table = ['descendant_slice_by_stack(stack_id) AS slice'];
  expect(pivotTableQueryGenerator.generateStackQuery(
             selectedPivots,
             selectedAggregations,
             WHERE_FILTERS,
             table,
             /* stack_id = */ 'stack_id'))
      .toEqual(expectedQuery);
});

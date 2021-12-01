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

// Normalize query for comparison by replacing repeated whitespace characters
// with a single space.
function normalize(s: string): string {
  return s.replace(/\s+/g, ' ');
}

function expectQueryEqual(actual: string, expected: string) {
  expect(normalize(actual)).toEqual(normalize(expected));
}

test('Generate query with pivots and aggregations', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: 'type', isStackPivot: false},
    {tableName: 'slice', columnName: 'id', isStackPivot: false}
  ];
  const selectedAggregations: AggregationAttrs[] = [
    {aggregation: 'SUM', tableName: 'slice', columnName: 'dur', order: 'DESC'}
  ];
  const expectedQuery = `
    SELECT
      "slice type",
      "slice id",
      "slice dur (SUM)",
      "slice dur (SUM) (total)"
    FROM (
      SELECT
        "slice type",
        "slice id",
        SUM("slice dur (SUM)") OVER () AS "slice dur (SUM) (total)",
        SUM("slice dur (SUM)")
          OVER (PARTITION BY "slice type",  "slice id")
          AS "slice dur (SUM)"
      FROM (
        SELECT
          slice.type AS "slice type",
          slice.id AS "slice id",
          slice.dur AS "slice dur (SUM)"
        FROM
          slice
        WHERE
          slice.dur != -1
      )
    )
    GROUP BY "slice type",  "slice id",  "slice dur (SUM)"
    ORDER BY "slice dur (SUM)" DESC
  `;
  expectQueryEqual(
      pivotTableQueryGenerator.generateQuery(
          selectedPivots, selectedAggregations, WHERE_FILTERS, TABLES),
      expectedQuery);
});

test('Generate query with pivots', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: 'type', isStackPivot: false},
    {tableName: 'slice', columnName: 'id', isStackPivot: false}
  ];
  const selectedAggregations: AggregationAttrs[] = [];
  const expectedQuery = `
    SELECT
      "slice type",
      "slice id"
    FROM (
      SELECT
        slice.type AS "slice type",
        slice.id AS "slice id"
      FROM
        slice
      WHERE
        slice.dur != -1
    )
    GROUP BY "slice type",  "slice id"
  `;
  expectQueryEqual(
      pivotTableQueryGenerator.generateQuery(
          selectedPivots, selectedAggregations, WHERE_FILTERS, TABLES),
      expectedQuery);
});

test('Generate query with aggregations', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [];
  const selectedAggregations: AggregationAttrs[] = [
    {aggregation: 'SUM', tableName: 'slice', columnName: 'dur', order: 'DESC'},
    {aggregation: 'MAX', tableName: 'slice', columnName: 'dur', order: 'ASC'}
  ];
  const expectedQuery = `
    SELECT
      "slice dur (SUM)",
      "slice dur (MAX)"
    FROM (
      SELECT
        SUM("slice dur (SUM)") AS "slice dur (SUM)",
        MAX("slice dur (MAX)") AS "slice dur (MAX)"
      FROM (
        SELECT
          slice.dur AS "slice dur (SUM)",
          slice.dur AS "slice dur (MAX)"
        FROM
          slice
        WHERE
          slice.dur != -1
      )
    )
    GROUP BY "slice dur (SUM)",  "slice dur (MAX)"
    ORDER BY "slice dur (SUM)" DESC,  "slice dur (MAX)" ASC
  `;
  expectQueryEqual(
      pivotTableQueryGenerator.generateQuery(
          selectedPivots, selectedAggregations, WHERE_FILTERS, TABLES),
      expectedQuery);
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
  const expectedQuery = `
    SELECT
      "slice name (stack)",
      "slice depth (hidden)",
      "slice stack_id (hidden)",
      "slice parent_stack_id (hidden)",
      "slice category",
      "slice id (COUNT)",
      "slice id (COUNT) (total)"
    FROM (
      SELECT
        "slice name (stack)",
        "slice depth (hidden)",
        "slice stack_id (hidden)",
        "slice parent_stack_id (hidden)",
        "slice category",
        COUNT("slice id (COUNT)") OVER () AS "slice id (COUNT) (total)",
        COUNT("slice id (COUNT)")
          OVER (PARTITION BY "slice stack_id (hidden)",  "slice category")
          AS "slice id (COUNT)"
      FROM (
        SELECT
          slice.name AS "slice name (stack)",
          slice.depth AS "slice depth (hidden)",
          slice.stack_id AS "slice stack_id (hidden)",
          slice.parent_stack_id AS "slice parent_stack_id (hidden)",
          slice.category AS "slice category",
          slice.id AS "slice id (COUNT)"
        FROM
          slice
        WHERE
          slice.dur != -1
      )
    )
    GROUP BY "slice name (stack)",
             "slice depth (hidden)",
             "slice stack_id (hidden)",
             "slice parent_stack_id (hidden)",
             "slice category",
             "slice id (COUNT)"
    ORDER BY "slice id (COUNT)" DESC
  `;
  expectQueryEqual(
      pivotTableQueryGenerator.generateQuery(
          selectedPivots, selectedAggregations, WHERE_FILTERS, TABLES),
      expectedQuery);
});

test('Generate a descendant stack query', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: SLICE_STACK_COLUMN, isStackPivot: true},
  ];
  const selectedAggregations: AggregationAttrs[] = [];
  const expectedQuery = `
    SELECT
      "slice name (stack)",
      "slice depth (hidden)",
      "slice stack_id (hidden)",
      "slice parent_stack_id (hidden)"
    FROM (
      SELECT
        slice.name AS "slice name (stack)",
        slice.depth AS "slice depth (hidden)",
        slice.stack_id AS "slice stack_id (hidden)",
        slice.parent_stack_id AS "slice parent_stack_id (hidden)"
      FROM
        descendant_slice_by_stack(stack_id) AS slice
      WHERE
        slice.dur != -1
    )
    GROUP BY "slice name (stack)",
             "slice depth (hidden)",
             "slice stack_id (hidden)",
             "slice parent_stack_id (hidden)"
    ORDER BY "slice depth (hidden)" ASC
  `;

  const table = ['descendant_slice_by_stack(stack_id) AS slice'];
  expectQueryEqual(
      pivotTableQueryGenerator.generateStackQuery(
          selectedPivots,
          selectedAggregations,
          WHERE_FILTERS,
          table,
          /* stack_id = */ 'stack_id'),
      expectedQuery);
});

test('Generate a descendant stack query with another pivot', () => {
  const pivotTableQueryGenerator = new PivotTableQueryGenerator();
  const selectedPivots: PivotAttrs[] = [
    {tableName: 'slice', columnName: SLICE_STACK_COLUMN, isStackPivot: true},
    {tableName: 'slice', columnName: 'category', isStackPivot: false}
  ];
  const selectedAggregations: AggregationAttrs[] = [];
  const expectedQuery = `
    SELECT
      "slice name (stack)",
      "slice depth (hidden)",
      "slice stack_id (hidden)",
      "slice parent_stack_id (hidden)",
      "slice category"
    FROM (
      SELECT
        slice.name AS "slice name (stack)",
        slice.depth AS "slice depth (hidden)",
        slice.stack_id AS "slice stack_id (hidden)",
        slice.parent_stack_id AS "slice parent_stack_id (hidden)",
        slice.category AS "slice category"
      FROM
        slice
      WHERE
        slice.dur != -1 AND
        slice.stack_id = stack_id
    )
    GROUP BY "slice name (stack)",
             "slice depth (hidden)",
             "slice stack_id (hidden)",
             "slice parent_stack_id (hidden)",
             "slice category"
    UNION ALL
    SELECT
      "slice name (stack)",
      "slice depth (hidden)",
      "slice stack_id (hidden)",
      "slice parent_stack_id (hidden)",
      "slice category"
    FROM (
      SELECT
        slice.name AS "slice name (stack)",
        slice.depth AS "slice depth (hidden)",
        slice.stack_id AS "slice stack_id (hidden)",
        slice.parent_stack_id AS "slice parent_stack_id (hidden)",
        slice.category AS "slice category"
      FROM
        descendant_slice_by_stack(stack_id) AS slice
      WHERE
        slice.dur != -1
    )
    GROUP BY "slice name (stack)",
             "slice depth (hidden)",
             "slice stack_id (hidden)",
             "slice parent_stack_id (hidden)",
             "slice category"
    ORDER BY "slice depth (hidden)" ASC
  `;

  const table = ['descendant_slice_by_stack(stack_id) AS slice'];
  expectQueryEqual(
      pivotTableQueryGenerator.generateStackQuery(
          selectedPivots,
          selectedAggregations,
          WHERE_FILTERS,
          table,
          /* stack_id = */ 'stack_id'),
      expectedQuery);
});

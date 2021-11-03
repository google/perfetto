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

import {SLICE_STACK_COLUMN, TableAttrs} from '../common/pivot_table_common';
import {PivotTableHelper} from './pivot_table_helper';

const AVAILABLE_COLUMNS: TableAttrs[] =
    [{tableName: 'slice', columns: ['id', 'type', 'dur', SLICE_STACK_COLUMN]}];
const ID_COL_IDX = 0;
const TYPE_COL_IDX = 1;
const DUR_COL_IDX = 2;
const STACK_COL_IDX = 3;

const AVAILABLE_AGGREGATIONS = ['SUM', 'AVG'];
const SUM_AGG_IDX = 0;
const AVG_AGG_IDX = 1;

function createNewHelper() {
  return new PivotTableHelper(
      'pivotTable', AVAILABLE_COLUMNS, AVAILABLE_AGGREGATIONS, [], []);
}

test('Update selected pivots based on selected indices', () => {
  const helper = createNewHelper();
  helper.setSelectedPivotTableColumnIndex(ID_COL_IDX);

  helper.updatePivotTableColumnOnSelectedIndex();
  expect(helper.selectedPivots).toEqual([
    {tableName: 'slice', columnName: 'id', isStackPivot: false}
  ]);

  helper.updatePivotTableColumnOnSelectedIndex();
  expect(helper.selectedPivots).toEqual([]);
});

test('Update selected aggregations based on selected indices', () => {
  const helper = createNewHelper();
  helper.togglePivotSelection();
  helper.setSelectedPivotTableColumnIndex(DUR_COL_IDX);
  helper.setSelectedPivotTableAggregationIndex(SUM_AGG_IDX);

  helper.updatePivotTableColumnOnSelectedIndex();
  expect(helper.selectedAggregations).toEqual([
    {tableName: 'slice', columnName: 'dur', aggregation: 'SUM', order: 'DESC'}
  ]);

  helper.updatePivotTableColumnOnSelectedIndex();
  expect(helper.selectedAggregations).toEqual([]);
});

test('Change aggregation sorting based on aggregation index', () => {
  const helper = createNewHelper();
  helper.togglePivotSelection();
  helper.setSelectedPivotTableColumnIndex(DUR_COL_IDX);
  helper.setSelectedPivotTableAggregationIndex(SUM_AGG_IDX);
  helper.updatePivotTableColumnOnSelectedIndex();

  expect(helper.selectedAggregations).toEqual([
    {tableName: 'slice', columnName: 'dur', aggregation: 'SUM', order: 'DESC'}
  ]);
  helper.togglePivotTableAggregationSorting(0);
  expect(helper.selectedAggregations).toEqual([
    {tableName: 'slice', columnName: 'dur', aggregation: 'SUM', order: 'ASC'}
  ]);
});

test(
    'Changing aggregation sorting with invalid index results in an error',
    () => {
      const helper = createNewHelper();
      expect(() => helper.togglePivotTableAggregationSorting(1))
          .toThrow('Column index "1" is out of bounds.');
    });

test('Reorder columns based on target and destination indices', () => {
  const helper = createNewHelper();
  helper.setSelectedPivotTableColumnIndex(ID_COL_IDX);
  helper.updatePivotTableColumnOnSelectedIndex();
  helper.setSelectedPivotTableColumnIndex(TYPE_COL_IDX);
  helper.updatePivotTableColumnOnSelectedIndex();

  expect(helper.selectedPivots).toEqual([
    {tableName: 'slice', columnName: 'id', isStackPivot: false},
    {tableName: 'slice', columnName: 'type', isStackPivot: false}
  ]);
  helper.reorderPivotTableDraggedColumn(true, 0, 1);
  expect(helper.selectedPivots).toEqual([
    {tableName: 'slice', columnName: 'type', isStackPivot: false},
    {tableName: 'slice', columnName: 'id', isStackPivot: false}
  ]);
});

test('Reordering columns with invalid indices results in an error', () => {
  const helper = createNewHelper();
  expect(() => helper.reorderPivotTableDraggedColumn(true, 0, 1))
      .toThrow('Target column index "0" out of bounds.');
});

test('Select column based on attributes', () => {
  const helper = createNewHelper();
  helper.selectPivotTableColumn({
    tableName: 'slice',
    columnName: 'dur',
    aggregation: 'AVG',
    order: 'DESC'
  });
  expect(helper.isPivot).toEqual(false);
  expect(helper.selectedColumnIndex).toEqual(DUR_COL_IDX);
  expect(helper.selectedAggregationIndex).toEqual(AVG_AGG_IDX);
});

test('Selecting a column with invalid attributes results in an error', () => {
  const helper = createNewHelper();
  expect(
      () => helper.selectPivotTableColumn(
          {tableName: 'foo', columnName: 'bar', isStackPivot: false}))
      .toThrow('Selected column "foo bar" not found in availableColumns.');
});

test('Selecting stack column sets isStackPivot', () => {
  const helper = createNewHelper();
  helper.setSelectedPivotTableColumnIndex(STACK_COL_IDX);

  helper.updatePivotTableColumnOnSelectedIndex();
  expect(helper.selectedPivots).toEqual([
    {tableName: 'slice', columnName: SLICE_STACK_COLUMN, isStackPivot: true}
  ]);
});

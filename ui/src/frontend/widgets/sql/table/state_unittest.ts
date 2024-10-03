// Copyright (C) 2024 The Android Open Source Project
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

import {createFakeTraceImpl} from '../../../../core/fake_trace_impl';
import {tableColumnId} from './column';
import {SqlTableState} from './state';
import {SqlTableDescription} from './table_description';
import {
  ArgSetColumnSet,
  StandardColumn,
  TimestampColumn,
} from './well_known_columns';

const idColumn = new StandardColumn('id');
const nameColumn = new StandardColumn('name', {title: 'Name'});
const tsColumn = new TimestampColumn('ts', {
  title: 'Timestamp',
  startsHidden: true,
});

const table: SqlTableDescription = {
  name: 'table',
  displayName: 'Table',
  columns: [idColumn, nameColumn, tsColumn, new ArgSetColumnSet('arg_set_id')],
};

test('sqlTableState: columnManupulation', () => {
  const trace = createFakeTraceImpl({allowQueries: true});
  const state = new SqlTableState(trace, table);

  // The initial set of columns should include "id" and "name",
  // but not "ts" (as it is marked as startsHidden) and not "arg_set_id"
  // (as it is a special column).
  expect(state.getSelectedColumns().map((c) => tableColumnId(c))).toEqual([
    'id',
    'name',
  ]);

  state.addColumn(tsColumn, 0);

  expect(state.getSelectedColumns().map((c) => tableColumnId(c))).toEqual([
    'id',
    'ts',
    'name',
  ]);

  state.hideColumnAtIndex(0);

  expect(state.getSelectedColumns().map((c) => tableColumnId(c))).toEqual([
    'ts',
    'name',
  ]);
});

test('sqlTableState: sortedColumns', () => {
  const trace = createFakeTraceImpl({allowQueries: true});
  const state = new SqlTableState(trace, table);

  // Verify that we have two columns: "id" and "name" and
  // save references to them.
  expect(state.getSelectedColumns().map((c) => tableColumnId(c))).toEqual([
    'id',
    'name',
  ]);

  // Sort by name column and verify that it is sorted by.
  state.sortBy({
    column: nameColumn,
    direction: 'ASC',
  });
  expect(state.isSortedBy(idColumn)).toBe(undefined);
  expect(state.isSortedBy(nameColumn)).toBe('ASC');

  // Sort by the same column in the opposite direction.
  state.sortBy({
    column: nameColumn,
    direction: 'DESC',
  });
  expect(state.isSortedBy(idColumn)).toBe(undefined);
  expect(state.isSortedBy(nameColumn)).toBe('DESC');

  // Sort by the id column.
  state.sortBy({
    column: idColumn,
    direction: 'ASC',
  });
  expect(state.isSortedBy(idColumn)).toBe('ASC');
  expect(state.isSortedBy(nameColumn)).toBe(undefined);

  // When the column is hidden, it should no longer be sorted by
  // and we should fall back to the previously sorted by column.
  state.hideColumnAtIndex(0);
  expect(state.isSortedBy(nameColumn)).toBe('DESC');

  // Remove the sorting and verify that we are no sorted by.
  state.unsort();
  expect(state.isSortedBy(nameColumn)).toBe(undefined);
});

// Clean up repeated whitespaces to allow for easier testing.
function normalize(s: string): string {
  return s.replace(/\s+/g, ' ').trim();
}

test('sqlTableState: sqlStatement', () => {
  const trace = createFakeTraceImpl({allowQueries: true});
  const state = new SqlTableState(trace, table);

  // Check the generated SQL statement.
  expect(normalize(state.getCurrentRequest().query)).toBe(
    'SELECT table_0.id AS id, table_0.name AS name FROM table AS table_0 LIMIT 101 OFFSET 0',
  );
});

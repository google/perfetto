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
import {tableColumnId} from './table_column';
import {SqlTableState} from './state';
import {SqlTableDescription} from './table_description';
import {createTableColumn} from './columns';
import {PerfettoSqlTypes} from '../../../../trace_processor/perfetto_sql_type';

const trace = createFakeTraceImpl({allowQueries: true});
const idColumn = createTableColumn({trace, column: 'id', type: undefined});
const nameColumn = createTableColumn({trace, column: 'name', type: undefined});
const tsColumn = createTableColumn({
  trace,
  column: 'ts',
  type: PerfettoSqlTypes.TIMESTAMP,
});

const table: SqlTableDescription = {
  name: 'table',
  displayName: 'Table',
  columns: [idColumn, nameColumn, tsColumn],
};

test('sqlTableState: columnManupulation', () => {
  const state = new SqlTableState(trace, table);

  state.addColumn(tsColumn, 0);

  expect(state.getSelectedColumns().map((c) => tableColumnId(c))).toEqual([
    'id',
    'ts',
    'name',
    'ts',
  ]);

  state.hideColumnAtIndex(0);

  expect(state.getSelectedColumns().map((c) => tableColumnId(c))).toEqual([
    'ts',
    'name',
    'ts',
  ]);
});

test('sqlTableState: sortedColumns', () => {
  const trace = createFakeTraceImpl({allowQueries: true});
  const state = new SqlTableState(trace, table);

  // Verify that we have three columns: "id", "name" and "ts" and save
  // references to them.
  expect(state.getSelectedColumns().map((c) => tableColumnId(c))).toEqual([
    'id',
    'name',
    'ts',
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
  state.sortBy({column: nameColumn, direction: undefined});
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
    'SELECT table_0.id AS id, table_0.name AS name, table_0.ts AS ts FROM table AS table_0 LIMIT 101 OFFSET 0',
  );
});

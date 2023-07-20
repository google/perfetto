// Copyright (C) 2022 The Android Open Source Project
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

import {Engine, EngineProxy} from '../../common/engine';

import {Column} from './column';
import {SqlTableState} from './state';
import {SqlTableDescription} from './table_description';

const table: SqlTableDescription = {
  name: 'table',
  displayName: 'Table',
  columns: [
    {
      name: 'id',
    },
    {
      name: 'name',
      title: 'Name',
    },
    {
      name: 'ts',
      display: {
        type: 'timestamp',
      },
      startsHidden: true,
    },
    {
      name: 'arg_set_id',
      type: 'arg_set_id',
      title: 'Arg',
    },
  ],
};

class FakeEngine extends Engine {
  id: string = 'TestEngine';

  rpcSendRequestBytes(_data: Uint8Array) {}
}

test('sqlTableState: columnManupulation', () => {
  const engine = new EngineProxy(new FakeEngine(), 'test');
  const state = new SqlTableState(engine, table);

  const idColumn = {
    alias: 'id',
    expression: 'id',
    title: 'id',
  };
  const nameColumn = {
    alias: 'name',
    expression: 'name',
    title: 'Name',
  };
  const tsColumn: Column = {
    alias: 'ts',
    expression: 'ts',
    title: 'ts',
    display: {
      type: 'timestamp',
    },
  };

  // The initial set of columns should include "id" and "name",
  // but not "ts" (as it is marked as startsHidden) and not "arg_set_id"
  // (as it is a special column).
  expect(state.getSelectedColumns()).toEqual([
    idColumn,
    nameColumn,
  ]);

  state.addColumn(tsColumn, 0);

  expect(state.getSelectedColumns()).toEqual([
    idColumn,
    tsColumn,
    nameColumn,
  ]);

  state.hideColumnAtIndex(0);

  expect(state.getSelectedColumns()).toEqual([
    tsColumn,
    nameColumn,
  ]);
});

test('sqlTableState: sortedColumns', () => {
  const engine = new EngineProxy(new FakeEngine(), 'test');
  const state = new SqlTableState(engine, table);

  // Verify that we have two columns: "id" and "name" and
  // save references to them.
  expect(state.getSelectedColumns().length).toBe(2);
  const idColumn = state.getSelectedColumns()[0];
  expect(idColumn.alias).toBe('id');
  const nameColumn = state.getSelectedColumns()[1];
  expect(nameColumn.alias).toBe('name');

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
  const engine = new EngineProxy(new FakeEngine(), 'test');
  const state = new SqlTableState(engine, table);

  // Check the generated SQL statement.
  expect(normalize(state.buildSqlSelectStatement().selectStatement))
      .toBe('SELECT id as id, name as name FROM table');
});

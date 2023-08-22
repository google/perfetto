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

import {
  argColumn,
  Column,
  columnFromSqlTableColumn,
  formatSqlProjection,
  sqlProjectionsForColumn,
} from './column';
import {
  ArgSetIdColumn,
  SqlTableColumn,
  SqlTableDescription,
} from './table_description';

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
    },
    {
      name: 'arg_set_id',
      type: 'arg_set_id',
      title: 'Arg',
    },
  ],
};

test('fromSqlTableColumn', () => {
  expect(columnFromSqlTableColumn(table.columns[0])).toEqual({
    expression: 'id',
    alias: 'id',
    title: 'id',
  });

  expect(columnFromSqlTableColumn(table.columns[1])).toEqual({
    expression: 'name',
    alias: 'name',
    title: 'Name',
  });

  expect(columnFromSqlTableColumn(table.columns[2])).toEqual({
    expression: 'ts',
    alias: 'ts',
    title: 'ts',
    display: {
      type: 'timestamp',
    },
  });

  expect(argColumn(table.columns[3] as ArgSetIdColumn, 'foo.bar')).toEqual({
    expression: 'extract_arg(arg_set_id, \'foo.bar\')',
    alias: '_arg_arg_set_id_foo_bar',
    title: 'Arg foo.bar',
  });
});

function formatSqlProjectionsForColumn(c: Column): string {
  return sqlProjectionsForColumn(c).map(formatSqlProjection).join(', ');
}

test('sqlProjections', () => {
  const format = (c: SqlTableColumn) =>
      formatSqlProjectionsForColumn(columnFromSqlTableColumn(c));

  expect(format(table.columns[0])).toEqual('id as id');
  expect(format(table.columns[1])).toEqual('name as name');
  expect(format(table.columns[2])).toEqual('ts as ts');
});

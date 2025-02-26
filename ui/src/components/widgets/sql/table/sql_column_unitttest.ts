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

import {sqlColumnId, SqlExpression} from './sql_column';

test('sql_column_id.basic', () => {
  // Straightforward case: just a column selection.
  expect(sqlColumnId('utid')).toBe('utid');
});

test('sql_column_id.single_join', () => {
  expect(
    sqlColumnId({
      column: 'bar',
      source: {
        table: 'foo',
        joinOn: {
          foo_id: 'id',
        },
      },
    }),
  ).toBe('foo[foo_id=id].bar');
});

test('sql_column_id.double_join', () => {
  expect(
    sqlColumnId({
      column: 'abc',
      source: {
        table: 'alphabet',
        joinOn: {
          abc_id: {
            column: 'bar',
            source: {
              table: 'foo',
              joinOn: {
                foo_id: 'id',
              },
            },
          },
        },
      },
    }),
  ).toBe('alphabet[abc_id=foo[foo_id=id].bar].abc');
});

test('sql_column_id.join_on_id', () => {
  // Special case: joins on `id` should be simplified.
  expect(
    sqlColumnId({
      column: 'name',
      source: {
        table: 'foo',
        joinOn: {
          id: 'foo_id',
        },
      },
    }),
  ).toBe('foo[foo_id].name');
});

test('sql_column_id.nested_join_on_id', () => {
  // Special case: joins on `id` should be simplified in nested joins.
  expect(
    sqlColumnId({
      column: 'name',
      source: {
        table: 'foo',
        joinOn: {
          id: {
            column: 'foo_id',
            source: {
              table: 'bar',
              joinOn: {
                x: 'y',
              },
            },
          },
        },
      },
    }),
  ).toBe('foo[bar[x=y].foo_id].name');
});

test('sql_column_id.simplied_join', () => {
  // Special case: if both sides of the join are the same, only one can be shown.
  expect(
    sqlColumnId({
      column: 'name',
      source: {
        table: 'foo',
        joinOn: {
          x: 'y',
          z: 'z',
        },
      },
    }),
  ).toBe('foo[x=y, z].name');
});

test('sql_column_id.expression_without_id', () => {
  expect(
    sqlColumnId(
      new SqlExpression((cols) => `(${cols.join('&')})`, ['a', 'b', 'c']),
    ),
  ).toBe('a&b&c');
});

test('sql_column_id.expression_with_id', () => {
  expect(
    sqlColumnId(
      new SqlExpression((cols) => `(${cols.join('&')})`, ['a', 'b', 'c'], 'id'),
    ),
  ).toBe('id');
});

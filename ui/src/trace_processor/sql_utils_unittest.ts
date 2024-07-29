// Copyright (C) 2023 The Android Open Source Project
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

import {constraintsToQueryPrefix, constraintsToQuerySuffix} from './sql_utils';

// Clean up repeated whitespaces to allow for easier testing.
function normalize(s: string): string {
  return s.replace(/\s+/g, ' ');
}

test('constraintsToQueryPrefix: empty', () => {
  expect(normalize(constraintsToQueryPrefix({}))).toEqual('');
});

test('constraintsToQueryPrefix: one CTE', () => {
  expect(
    normalize(
      constraintsToQueryPrefix({
        commonTableExpressions: {foo: 'select * from bar'},
      }),
    ),
  ).toEqual('WITH foo AS (select * from bar)');
});

test('constraintsToQueryPrefix: one CTE', () => {
  expect(
    normalize(
      constraintsToQueryPrefix({
        commonTableExpressions: {
          foo1: 'select * from bar1',
          foo2: 'select * from bar2',
        },
      }),
    ),
  ).toEqual('WITH foo1 AS (select * from bar1), foo2 AS (select * from bar2)');
});

test('constraintsToQuerySuffix: where', () => {
  expect(
    normalize(
      constraintsToQuerySuffix({
        filters: ['ts > 1000', 'dur != 0'],
      }),
    ),
  ).toEqual('WHERE ts > 1000 and dur != 0');
});

test('constraintsToQuerySuffix: order by', () => {
  expect(
    normalize(
      constraintsToQuerySuffix({
        orderBy: [
          {fieldName: 'name'},
          {fieldName: 'count', direction: 'DESC'},
          undefined,
          'value',
        ],
      }),
    ),
  ).toEqual('ORDER BY name, count DESC, value');
});

test('constraintsToQuerySuffix: limit', () => {
  expect(normalize(constraintsToQuerySuffix({limit: 3}))).toEqual('LIMIT 3');
});

test('constraintsToQuerySuffix: group by', () => {
  expect(
    normalize(
      constraintsToQuerySuffix({
        groupBy: ['foo', undefined, 'bar'],
      }),
    ),
  ).toEqual('GROUP BY foo, bar');
});

test('constraintsToQuerySuffix: all', () => {
  expect(
    normalize(
      constraintsToQuerySuffix({
        filters: ['id != 1'],
        groupBy: ['track_id'],
        orderBy: [{fieldName: 'ts'}],
        limit: 1,
      }),
    ),
  ).toEqual('WHERE id != 1 GROUP BY track_id ORDER BY ts LIMIT 1');
});

test('constraintsToQuerySuffix: all undefined', () => {
  expect(
    normalize(
      constraintsToQuerySuffix({
        filters: [undefined],
        orderBy: [undefined, undefined],
        groupBy: [undefined, undefined],
      }),
    ),
  ).toEqual('');
});

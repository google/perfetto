// Copyright (C) 2026 The Android Open Source Project
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

import {beforeEach, describe, expect, test} from 'vitest';
import {
  traceFilterState,
  traceColumnsState,
  traceOrderByState,
  traceQueryColumnsState,
  effectiveQueryColumns,
} from './trace_selection_state';
import {
  linkColumnFirst,
  linkNameFirst,
  groupResultColumns,
  resolveResultColumns,
} from './column_order';
import type {Filter} from '../../components/widgets/datagrid/model';

beforeEach(() => {
  localStorage.clear();
});

describe('traceFilterState', () => {
  test('defaults to an empty list', () => {
    expect(traceFilterState.get()).toEqual([]);
  });

  test('round-trips a set of chips', () => {
    const filters: Filter[] = [
      {field: 'file_name', op: 'glob', value: '*.pftrace'},
      {field: 'size_bytes', op: '>', value: '100'},
    ];
    traceFilterState.set(filters);
    expect(traceFilterState.get()).toEqual(filters);
  });

  test('clear() empties the list', () => {
    traceFilterState.set([{field: 'a', op: '=', value: '1'}]);
    traceFilterState.clear();
    expect(traceFilterState.get()).toEqual([]);
  });

  test('a malformed stored value reads back as empty', () => {
    localStorage.setItem('bigtraceTraceFilters', '{"filters":"not-an-array"}');
    expect(traceFilterState.get()).toEqual([]);
  });
});

describe('traceColumnsState', () => {
  const schema = [
    {name: 'file_name', defaultVisible: true},
    {name: 'size_bytes', defaultVisible: true},
    {name: 'device_name', defaultVisible: false},
  ];

  test('defaults to null (use schema defaults)', () => {
    expect(traceColumnsState.get()).toBeNull();
  });

  test('round-trips an explicit selection', () => {
    traceColumnsState.set(['file_name', 'device_name']);
    expect(traceColumnsState.get()).toEqual(['file_name', 'device_name']);
  });

  test('an empty selection collapses to the null default', () => {
    traceColumnsState.set([]);
    expect(traceColumnsState.get()).toBeNull();
  });

  test('clear() reverts to the null default', () => {
    traceColumnsState.set(['file_name']);
    traceColumnsState.clear();
    expect(traceColumnsState.get()).toBeNull();
  });

  test('effective() returns the defaultVisible columns when unset', () => {
    expect(traceColumnsState.effective(schema)).toEqual([
      'file_name',
      'size_bytes',
    ]);
  });

  test('effective() intersects an explicit selection with the live schema', () => {
    // 'gone' is stale (not in schema) and drops; order follows the selection.
    traceColumnsState.set(['device_name', 'gone', 'file_name']);
    expect(traceColumnsState.effective(schema)).toEqual([
      'device_name',
      'file_name',
    ]);
  });

  test('effective() hoists a link column to the front', () => {
    expect(
      traceColumnsState.effective([
        {name: 'file_name', defaultVisible: true},
        {name: 'link', defaultVisible: true},
      ]),
    ).toEqual(['link', 'file_name']);
  });
});

describe('traceOrderByState', () => {
  test('defaults to the empty string', () => {
    expect(traceOrderByState.get()).toBe('');
  });

  test('round-trips an AIP-132 ordering string', () => {
    traceOrderByState.set('size_bytes desc');
    expect(traceOrderByState.get()).toBe('size_bytes desc');
  });

  test('clear() resets to the empty string', () => {
    traceOrderByState.set('file_name asc');
    traceOrderByState.clear();
    expect(traceOrderByState.get()).toBe('');
  });

  test('a non-string stored value reads back as empty', () => {
    localStorage.setItem('bigtraceTraceOrderBy', '{"orderBy":42}');
    expect(traceOrderByState.get()).toBe('');
  });
});

describe('traceQueryColumnsState', () => {
  test('defaults to null (unchosen → attach defaultVisible)', () => {
    expect(traceQueryColumnsState.get()).toBeNull();
  });

  test('round-trips a selection', () => {
    traceQueryColumnsState.set(['device_name', 'android_id']);
    expect(traceQueryColumnsState.get()).toEqual(['device_name', 'android_id']);
  });

  test('preserves an explicit empty list as "attach nothing" (not null)', () => {
    // Unlike traceColumnsState, [] must NOT collapse to null, else "attach
    // nothing" is unexpressible.
    traceQueryColumnsState.set([]);
    expect(traceQueryColumnsState.get()).toEqual([]);
  });

  test('drops non-string entries from a malformed write', () => {
    localStorage.setItem(
      'bigtraceTraceQueryColumns',
      '{"chosen":["device_name",7,null,"android_id"]}',
    );
    expect(traceQueryColumnsState.get()).toEqual(['device_name', 'android_id']);
  });

  test('a non-array stored value reads back as null', () => {
    localStorage.setItem('bigtraceTraceQueryColumns', '{"chosen":"nope"}');
    expect(traceQueryColumnsState.get()).toBeNull();
  });

  test('clear() reverts to the null default', () => {
    traceQueryColumnsState.set(['device_name']);
    traceQueryColumnsState.clear();
    expect(traceQueryColumnsState.get()).toBeNull();
  });
});

describe('effectiveQueryColumns', () => {
  const schema = [
    {name: 'file_name', defaultVisible: true},
    {name: 'size_bytes', defaultVisible: true},
    {name: 'device_name', defaultVisible: false},
  ];

  test('null (unchosen) resolves to the defaultVisible columns', () => {
    expect(effectiveQueryColumns(null, schema)).toEqual([
      'file_name',
      'size_bytes',
    ]);
  });

  test('an explicit selection is intersected with the live schema', () => {
    // 'gone' is stale and drops; order follows the selection; device_name is
    // kept though not defaultVisible because it was explicitly chosen.
    expect(
      effectiveQueryColumns(['device_name', 'gone', 'file_name'], schema),
    ).toEqual(['device_name', 'file_name']);
  });

  test('an explicit empty list stays empty (attach nothing)', () => {
    expect(effectiveQueryColumns([], schema)).toEqual([]);
  });

  test('hoists a link column to the front of the defaults', () => {
    const withLink = [
      {name: 'file_name', defaultVisible: true},
      {name: 'link', defaultVisible: true},
      {name: 'size_bytes', defaultVisible: true},
    ];
    expect(effectiveQueryColumns(null, withLink)).toEqual([
      'link',
      'file_name',
      'size_bytes',
    ]);
  });
});

describe('linkColumnFirst (link leads everywhere)', () => {
  test('hoists link from the middle, preserving the rest in order', () => {
    expect(linkNameFirst(['a', 'link', 'b', 'c'])).toEqual([
      'link',
      'a',
      'b',
      'c',
    ]);
  });

  test('is a no-op when link is absent', () => {
    expect(linkNameFirst(['a', 'b'])).toEqual(['a', 'b']);
  });

  test('is a no-op when link is already first', () => {
    expect(linkNameFirst(['link', 'a', 'b'])).toEqual(['link', 'a', 'b']);
  });

  test('keys objects by name', () => {
    const cols = [{name: 'a'}, {name: 'link'}, {name: 'b'}];
    expect(linkColumnFirst(cols, (c) => c.name)).toEqual([
      {name: 'link'},
      {name: 'a'},
      {name: 'b'},
    ]);
  });
});

describe('groupResultColumns', () => {
  test('orders link, then result columns, then _-metadata at the end', () => {
    expect(groupResultColumns(['_b', 'name', 'link', '_a', 'dur'])).toEqual([
      'link',
      'name',
      'dur',
      '_b',
      '_a',
    ]);
  });

  test('is stable within each group and a no-op without _ or link', () => {
    expect(groupResultColumns(['name', 'dur', 'ts'])).toEqual([
      'name',
      'dur',
      'ts',
    ]);
  });
});

describe('resolveResultColumns', () => {
  test('null (unchosen) shows every available column', () => {
    const available = ['name', 'dur', 'device_name'];
    expect(resolveResultColumns(null, available)).toEqual(available);
  });

  test('intersects an explicit selection with the live columns', () => {
    // 'gone' is stale and drops; order follows the selection.
    expect(
      resolveResultColumns(
        ['device_name', 'gone', 'name'],
        ['name', 'dur', 'device_name'],
      ),
    ).toEqual(['device_name', 'name']);
  });

  test('falls back to show-all when every entry is stale', () => {
    // A schema change between queries shouldn't strand an empty grid.
    expect(resolveResultColumns(['old_a', 'old_b'], ['name', 'dur'])).toEqual([
      'name',
      'dur',
    ]);
  });

  test('hoists a link column to the front', () => {
    expect(resolveResultColumns(null, ['name', 'link', 'dur'])).toEqual([
      'link',
      'name',
      'dur',
    ]);
  });

  test('groups _-prefixed columns after the result columns', () => {
    expect(resolveResultColumns(null, ['name', '_meta', 'dur', '_x'])).toEqual([
      'name',
      'dur',
      '_meta',
      '_x',
    ]);
  });

  test('orders link first, then results, then _-metadata', () => {
    expect(resolveResultColumns(null, ['_m', 'link', 'name'])).toEqual([
      'link',
      'name',
      '_m',
    ]);
  });

  test('groups within an explicit selection too', () => {
    expect(
      resolveResultColumns(['_meta', 'name', 'dur'], ['name', 'dur', '_meta']),
    ).toEqual(['name', 'dur', '_meta']);
  });
});

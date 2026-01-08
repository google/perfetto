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

import {Row} from '../../../trace_processor/query_result';
import {InMemoryDataSource} from './in_memory_data_source';
import {Column, Filter} from './model';

describe('InMemoryDataSource', () => {
  const sampleData: ReadonlyArray<Row> = [
    {
      id: 1,
      name: 'Alice',
      value: 100,
      active: 1,
      tag: 'A',
      blob: new Uint8Array([1, 2]),
    },
    {
      id: 2,
      name: 'Bob',
      value: 200,
      active: 0,
      tag: 'B',
      blob: new Uint8Array([3, 4, 5]),
    },
    {id: 3, name: 'Charlie', value: 150, active: 1, tag: 'A', blob: null},
    {
      id: 4,
      name: 'David',
      value: null,
      active: 0,
      tag: 'C',
      blob: new Uint8Array([6]),
    },
    {
      id: 5,
      name: 'Eve',
      value: 100,
      active: 1,
      tag: 'B',
      blob: new Uint8Array([7, 8, 9, 0]),
    },
    {
      id: 6,
      name: 'Mallory',
      value: 300n,
      active: 0,
      tag: 'C',
      blob: new Uint8Array([0]),
    },
    {
      id: 7,
      name: 'Trent',
      value: 250n,
      active: 1,
      tag: 'A',
      blob: new Uint8Array([1, 1]),
    },
  ];

  let dataSource: InMemoryDataSource;

  beforeEach(() => {
    dataSource = new InMemoryDataSource([...sampleData]); // Use a copy for each test
  });

  test('initialization', () => {
    const queryResult = dataSource.getRows({});
    expect(queryResult.isLoading).toBe(false);
    const result = queryResult.result!;
    expect(result.offset).toBe(0);
    expect(result.totalRows).toBe(sampleData.length);
    expect(result.rows).toEqual(sampleData);
  });

  describe('filtering', () => {
    test('equality filter', () => {
      const filters: Filter[] = [{field: 'name', op: '=', value: 'Alice'}];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(1);
      expect(result.rows[0].name).toBe('Alice');
    });

    test('inequality filter', () => {
      const filters: Filter[] = [{field: 'active', op: '!=', value: 1}];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(3); // Bob, David, Mallory
      result.rows.forEach((row) => expect(row.active).toBe(0));
    });

    test('less than filter', () => {
      const filters: Filter[] = [{field: 'value', op: '<', value: 150}];
      const result = dataSource.getRows({filters}).result!;
      // David (null), Alice (100), Eve (100)
      expect(result.totalRows).toBe(3);
      expect(result.rows.map((r) => r.id).sort()).toEqual([1, 4, 5]);
    });

    test('less than or equal filter', () => {
      const filters: Filter[] = [{field: 'value', op: '<=', value: 150}];
      const result = dataSource.getRows({filters}).result!;
      // David (null), Alice (100), Charlie (150), Eve (100)
      expect(result.totalRows).toBe(4);
      expect(result.rows.map((r) => r.id).sort()).toEqual([1, 3, 4, 5]);
    });

    test('greater than filter', () => {
      const filters: Filter[] = [{field: 'value', op: '>', value: 200}];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(2); // Mallory (300n), Trent (250n)
      expect(result.rows.map((r) => r.id).sort()).toEqual([6, 7]);
    });

    test('greater than or equal filter with bigint', () => {
      const filters: Filter[] = [{field: 'value', op: '>=', value: 250n}];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(2); // Mallory, Trent
      expect(result.rows.map((r) => r.id).sort()).toEqual([6, 7]);
    });

    test('is null filter', () => {
      const filters: Filter[] = [{field: 'value', op: 'is null'}];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(1);
      expect(result.rows[0].id).toBe(4); // David
    });

    test('is not null filter', () => {
      const filters: Filter[] = [{field: 'blob', op: 'is not null'}];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(6); // All except Charlie
      expect(result.rows.find((r) => r.id === 3)).toBeUndefined();
    });

    test('glob filter', () => {
      const filters: Filter[] = [{field: 'name', op: 'glob', value: 'A*e'}];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(1);
      expect(result.rows[0].name).toBe('Alice');
    });

    test('glob filter with ?', () => {
      const filters: Filter[] = [{field: 'name', op: 'glob', value: 'B?b'}];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(1);
      expect(result.rows[0].name).toBe('Bob');
    });

    test('multiple filters', () => {
      const filters: Filter[] = [
        {field: 'active', op: '=', value: 1},
        {field: 'tag', op: '=', value: 'A'},
      ];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(3); // Alice, Charlie, Trent
      result.rows.forEach((row) => {
        expect(row.active).toBe(1);
        expect(row.tag).toBe('A');
      });
    });

    test('no matching rows filter', () => {
      const filters: Filter[] = [
        {field: 'name', op: '=', value: 'NonExistent'},
      ];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(0);
      expect(result.rows.length).toBe(0);
    });

    test('in filter', () => {
      const filters: Filter[] = [{field: 'tag', op: 'in', value: ['A', 'B']}];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(5); // Alice, Bob, Charlie, Eve, Trent
      result.rows.forEach((row) => {
        expect(['A', 'B']).toContain(row.tag);
      });
    });

    test('not in filter', () => {
      const filters: Filter[] = [
        {field: 'tag', op: 'not in', value: ['A', 'B']},
      ];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(2); // David, Mallory
      result.rows.forEach((row) => {
        expect(row.tag).toBe('C');
      });
    });

    test('not glob filter', () => {
      const filters: Filter[] = [{field: 'name', op: 'not glob', value: 'A*'}];
      const result = dataSource.getRows({filters}).result!;
      expect(result.totalRows).toBe(6); // All except Alice
      expect(result.rows.find((r) => r.name === 'Alice')).toBeUndefined();
    });
  });

  describe('sorting', () => {
    test('sort by string ascending', () => {
      const columns: Column[] = [{field: 'name', sort: 'ASC'}];
      const result = dataSource.getRows({columns}).result!;
      expect(result.rows.map((r) => r.name)).toEqual([
        'Alice',
        'Bob',
        'Charlie',
        'David',
        'Eve',
        'Mallory',
        'Trent',
      ]);
    });

    test('sort by string descending', () => {
      const columns: Column[] = [{field: 'name', sort: 'DESC'}];
      const result = dataSource.getRows({columns}).result!;
      expect(result.rows.map((r) => r.name)).toEqual([
        'Trent',
        'Mallory',
        'Eve',
        'David',
        'Charlie',
        'Bob',
        'Alice',
      ]);
    });

    test('sort by number ascending (includes nulls)', () => {
      const columns: Column[] = [{field: 'value', sort: 'ASC'}];
      const result = dataSource.getRows({columns}).result!;
      // Nulls first, then 100, 100, 150, 200, 250n, 300n
      expect(result.rows.map((r) => r.id)).toEqual([4, 1, 5, 3, 2, 7, 6]);
    });

    test('sort by number descending (includes nulls and bigint)', () => {
      const columns: Column[] = [{field: 'value', sort: 'DESC'}];
      const result = dataSource.getRows({columns}).result!;
      // 300n, 250n, 200, 150, 100, 100, Nulls last
      expect(result.rows.map((r) => r.id)).toEqual([6, 7, 2, 3, 1, 5, 4]);
    });

    test('sort by boolean ascending', () => {
      const columns: Column[] = [{field: 'active', sort: 'ASC'}]; // 0 then 1
      const result = dataSource.getRows({columns}).result!;
      expect(result.rows.map((r) => r.active)).toEqual([0, 0, 0, 1, 1, 1, 1]);
    });

    test('sort by Uint8Array ascending (by length)', () => {
      const columns: Column[] = [{field: 'blob', sort: 'ASC'}];
      const result = dataSource.getRows({columns}).result!;
      // null (Charlie, id:3), len 1 (David id:4, Mallory id:6), len 2 (Alice id:1, Trent id:7), len 3 (Bob id:2), len 4 (Eve id:5)
      // Original order for same length: David before Mallory, Alice before Trent.
      expect(result.rows.map((r) => r.id)).toEqual([3, 4, 6, 1, 7, 2, 5]);
    });

    test('sort by Uint8Array descending (by length)', () => {
      const columns: Column[] = [{field: 'blob', sort: 'DESC'}];
      const result = dataSource.getRows({columns}).result!;
      // len 4, len 3, len 2, len 2, len 1, len 0, null
      expect(result.rows.map((r) => r.id)).toEqual([5, 2, 1, 7, 4, 6, 3]);
    });

    test('unsorted returns original order', () => {
      const result = dataSource.getRows({}).result!;
      // Should be in original order
      expect(result.rows.map((r) => r.id)).toEqual(sampleData.map((r) => r.id));
    });
  });

  describe('combined filtering and sorting', () => {
    test('filter then sort', () => {
      const filters: Filter[] = [{field: 'active', op: '=', value: 1}];
      const columns: Column[] = [{field: 'value', sort: 'DESC'}];
      const result = dataSource.getRows({columns, filters}).result!;
      // Active: Alice (100), Charlie (150), Eve (100), Trent (250n)
      // Sorted by value desc: Trent, Charlie, Alice, Eve (Alice/Eve order by original due to stable sort on value)
      expect(result.rows.map((r) => r.id)).toEqual([7, 3, 1, 5]);
      result.rows.forEach((row) => expect(row.active).toBe(1));
    });
  });

  describe('pagination', () => {
    test('offset and limit', () => {
      const columns: Column[] = [{field: 'id', sort: 'ASC'}];
      const result = dataSource.getRows({
        columns,
        pagination: {offset: 2, limit: 3},
      }).result!;
      expect(result.totalRows).toBe(7); // Total is still 7
      expect(result.offset).toBe(2);
      expect(result.rows.length).toBe(3);
      expect(result.rows.map((r) => r.id)).toEqual([3, 4, 5]);
    });

    test('offset beyond data', () => {
      const result = dataSource.getRows({
        pagination: {offset: 100, limit: 10},
      }).result!;
      expect(result.totalRows).toBe(7);
      expect(result.rows.length).toBe(0);
    });
  });

  describe('pivot mode', () => {
    test('basic pivot with count', () => {
      const result = dataSource.getRows({
        pivot: {
          groupBy: [{field: 'tag'}],
          aggregates: [{function: 'COUNT'}],
        },
      }).result!;
      // 3 unique tags: A, B, C
      expect(result.totalRows).toBe(3);
      const tagCounts = new Map(result.rows.map((r) => [r.tag, r.__count__]));
      expect(tagCounts.get('A')).toBe(3); // Alice, Charlie, Trent
      expect(tagCounts.get('B')).toBe(2); // Bob, Eve
      expect(tagCounts.get('C')).toBe(2); // David, Mallory
    });

    test('pivot with sum aggregate', () => {
      const result = dataSource.getRows({
        pivot: {
          groupBy: [{field: 'active'}],
          aggregates: [{function: 'SUM', field: 'value'}],
        },
      }).result!;
      expect(result.totalRows).toBe(2); // active: 0 and 1
      const activeSums = new Map(result.rows.map((r) => [r.active, r.value]));
      // active=0: Bob (200) + David (null) + Mallory (300n) = 500
      // active=1: Alice (100) + Charlie (150) + Eve (100) + Trent (250n) = 600
      expect(activeSums.get(0)).toBe(500);
      expect(activeSums.get(1)).toBe(600);
    });
  });

  describe('distinct values', () => {
    test('returns distinct values sorted', () => {
      const result = dataSource.getDistinctValues('tag');
      expect(result.isLoading).toBe(false);
      expect(result.result).toEqual(['A', 'B', 'C']);
    });

    test('handles null values', () => {
      const result = dataSource.getDistinctValues('blob');
      expect(result.isLoading).toBe(false);
      // null should come first
      expect(result.result![0]).toBe(null);
    });
  });

  test('empty data source', () => {
    const emptyDataSource = new InMemoryDataSource([]);
    const queryResult = emptyDataSource.getRows({});
    expect(queryResult.isLoading).toBe(false);
    const result = queryResult.result!;
    expect(result.offset).toBe(0);
    expect(result.totalRows).toBe(0);
    expect(result.rows).toEqual([]);

    const filteredResult = emptyDataSource.getRows({
      columns: [{field: 'id', sort: 'DESC'}],
      filters: [{field: 'name', op: '=', value: 'test'}],
    }).result!;
    expect(filteredResult.totalRows).toBe(0);
    expect(filteredResult.rows).toEqual([]);
  });
});

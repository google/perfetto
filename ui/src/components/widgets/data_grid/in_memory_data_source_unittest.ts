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

import {InMemoryDataSource} from './in_memory_data_source';
import {FilterDefinition, RowDef, SortBy} from './common';

describe('InMemoryDataSource', () => {
  const sampleData: ReadonlyArray<RowDef> = [
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
    const result = dataSource.rows;
    expect(result.rowOffset).toBe(0);
    expect(result.totalRows).toBe(sampleData.length);
    expect(result.rows).toEqual(sampleData);
  });

  describe('filtering', () => {
    test('equality filter', () => {
      const filters: FilterDefinition[] = [
        {column: 'name', op: '=', value: 'Alice'},
      ];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      expect(result.totalRows).toBe(1);
      expect(result.rows[0].name).toBe('Alice');
    });

    test('inequality filter', () => {
      const filters: FilterDefinition[] = [
        {column: 'active', op: '!=', value: 1},
      ];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      expect(result.totalRows).toBe(3); // Bob, David, Mallory
      result.rows.forEach((row) => expect(row.active).toBe(0));
    });

    test('less than filter', () => {
      const filters: FilterDefinition[] = [
        {column: 'value', op: '<', value: 150},
      ];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      // David (null), Alice (100), Eve (100)
      expect(result.totalRows).toBe(3);
      expect(result.rows.map((r) => r.id).sort()).toEqual([1, 4, 5]);
    });

    test('less than or equal filter', () => {
      const filters: FilterDefinition[] = [
        {column: 'value', op: '<=', value: 150},
      ];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      // David (null), Alice (100), Charlie (150), Eve (100)
      expect(result.totalRows).toBe(4);
      expect(result.rows.map((r) => r.id).sort()).toEqual([1, 3, 4, 5]);
    });

    test('greater than filter', () => {
      const filters: FilterDefinition[] = [
        {column: 'value', op: '>', value: 200},
      ];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      expect(result.totalRows).toBe(2); // Mallory (300n), Trent (250n)
      expect(result.rows.map((r) => r.id).sort()).toEqual([6, 7]);
    });

    test('greater than or equal filter with bigint', () => {
      const filters: FilterDefinition[] = [
        {column: 'value', op: '>=', value: 250n},
      ];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      expect(result.totalRows).toBe(2); // Mallory, Trent
      expect(result.rows.map((r) => r.id).sort()).toEqual([6, 7]);
    });

    test('is null filter', () => {
      const filters: FilterDefinition[] = [{column: 'value', op: 'is null'}];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      expect(result.totalRows).toBe(1);
      expect(result.rows[0].id).toBe(4); // David
    });

    test('is not null filter', () => {
      const filters: FilterDefinition[] = [{column: 'blob', op: 'is not null'}];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      expect(result.totalRows).toBe(6); // All except Charlie
      expect(result.rows.find((r) => r.id === 3)).toBeUndefined();
    });

    test('glob filter', () => {
      const filters: FilterDefinition[] = [
        {column: 'name', op: 'glob', value: 'A*e'},
      ];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      expect(result.totalRows).toBe(1);
      expect(result.rows[0].name).toBe('Alice');
    });

    test('glob filter with ?', () => {
      const filters: FilterDefinition[] = [
        {column: 'name', op: 'glob', value: 'B?b'},
      ];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      expect(result.totalRows).toBe(1);
      expect(result.rows[0].name).toBe('Bob');
    });

    test('multiple filters', () => {
      const filters: FilterDefinition[] = [
        {column: 'active', op: '=', value: 1},
        {column: 'tag', op: '=', value: 'A'},
      ];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      expect(result.totalRows).toBe(3); // Alice, Charlie, Trent
      result.rows.forEach((row) => {
        expect(row.active).toBe(1);
        expect(row.tag).toBe('A');
      });
    });

    test('no matching rows filter', () => {
      const filters: FilterDefinition[] = [
        {column: 'name', op: '=', value: 'NonExistent'},
      ];
      dataSource.notifyUpdate({direction: 'unsorted'}, filters);
      const result = dataSource.rows;
      expect(result.totalRows).toBe(0);
      expect(result.rows.length).toBe(0);
    });
  });

  describe('sorting', () => {
    test('sort by string ascending', () => {
      const sortBy: SortBy = {column: 'name', direction: 'asc'};
      dataSource.notifyUpdate(sortBy, []);
      const result = dataSource.rows;
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
      const sortBy: SortBy = {column: 'name', direction: 'desc'};
      dataSource.notifyUpdate(sortBy, []);
      const result = dataSource.rows;
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
      const sortBy: SortBy = {column: 'value', direction: 'asc'};
      dataSource.notifyUpdate(sortBy, []);
      const result = dataSource.rows;
      // Nulls first, then 100, 100, 150, 200, 250n, 300n
      expect(result.rows.map((r) => r.id)).toEqual([4, 1, 5, 3, 2, 7, 6]);
    });

    test('sort by number descending (includes nulls and bigint)', () => {
      const sortBy: SortBy = {column: 'value', direction: 'desc'};
      dataSource.notifyUpdate(sortBy, []);
      const result = dataSource.rows;
      // 300n, 250n, 200, 150, 100, 100, Nulls last
      expect(result.rows.map((r) => r.id)).toEqual([6, 7, 2, 3, 1, 5, 4]);
    });

    test('sort by boolean ascending', () => {
      const sortBy: SortBy = {column: 'active', direction: 'asc'}; // 0 then 1
      dataSource.notifyUpdate(sortBy, []);
      const result = dataSource.rows;
      expect(result.rows.map((r) => r.active)).toEqual([0, 0, 0, 1, 1, 1, 1]);
    });

    test('sort by Uint8Array ascending (by length)', () => {
      const sortBy: SortBy = {column: 'blob', direction: 'asc'};
      dataSource.notifyUpdate(sortBy, []);
      const result = dataSource.rows;
      // null (Charlie, id:3), len 1 (David id:4, Mallory id:6), len 2 (Alice id:1, Trent id:7), len 3 (Bob id:2), len 4 (Eve id:5)
      // Original order for same length: David before Mallory, Alice before Trent.
      expect(result.rows.map((r) => r.id)).toEqual([3, 4, 6, 1, 7, 2, 5]);
    });

    test('sort by Uint8Array descending (by length)', () => {
      const sortBy: SortBy = {column: 'blob', direction: 'desc'};
      dataSource.notifyUpdate(sortBy, []);
      const result = dataSource.rows;
      // len 4, len 3, len 2, len 2, len 1, len 0, null
      expect(result.rows.map((r) => r.id)).toEqual([5, 2, 1, 7, 4, 6, 3]);
    });

    test('unsorted', () => {
      const sortBy: SortBy = {direction: 'unsorted'};
      // Apply some sort first
      dataSource.notifyUpdate({column: 'name', direction: 'asc'}, []);
      // Then unsort
      dataSource.notifyUpdate(sortBy, []);
      const result = dataSource.rows;
      // Should revert to original order if no filters applied
      expect(result.rows.map((r) => r.id)).toEqual(sampleData.map((r) => r.id));
    });
  });

  describe('combined filtering and sorting', () => {
    test('filter then sort', () => {
      const filters: FilterDefinition[] = [
        {column: 'active', op: '=', value: 1},
      ];
      const sortBy: SortBy = {column: 'value', direction: 'desc'};
      dataSource.notifyUpdate(sortBy, filters);
      const result = dataSource.rows;
      // Active: Alice (100), Charlie (150), Eve (100), Trent (250n)
      // Sorted by value desc: Trent, Charlie, Alice, Eve (Alice/Eve order by original due to stable sort on value)
      expect(result.rows.map((r) => r.id)).toEqual([7, 3, 1, 5]);
      result.rows.forEach((row) => expect(row.active).toBe(1));
    });
  });

  describe('caching behavior', () => {
    test('data is not reprocessed if sortBy and filters are identical', () => {
      const filters: FilterDefinition[] = [
        {column: 'tag', op: '=', value: 'A'},
      ];
      const sortBy: SortBy = {column: 'name', direction: 'asc'};

      dataSource.notifyUpdate(sortBy, filters);
      const result1 = dataSource.rows.rows; // Access internal array

      // Spy on internal methods if possible, or check object identity
      // For this test, we'll check if the returned array reference is the same
      dataSource.notifyUpdate(sortBy, filters); // Identical call
      const result2 = dataSource.rows.rows;

      expect(result1).toBe(result2); // Should be the same array instance due to caching
    });

    test('data is reprocessed if sortBy changes', () => {
      const filters: FilterDefinition[] = [
        {column: 'tag', op: '=', value: 'A'},
      ];
      const sortBy1: SortBy = {column: 'name', direction: 'asc'};
      const sortBy2: SortBy = {column: 'name', direction: 'desc'};

      dataSource.notifyUpdate(sortBy1, filters);
      const result1 = dataSource.rows.rows;

      dataSource.notifyUpdate(sortBy2, filters); // Different sort
      const result2 = dataSource.rows.rows;

      expect(result1).not.toBe(result2);
      expect(result1.map((r) => r.id)).not.toEqual(result2.map((r) => r.id));
    });

    test('data is reprocessed if filters change', () => {
      const filters1: FilterDefinition[] = [
        {column: 'tag', op: '=', value: 'A'},
      ];
      const filters2: FilterDefinition[] = [
        {column: 'tag', op: '=', value: 'B'},
      ];
      const sortBy: SortBy = {column: 'name', direction: 'asc'};

      dataSource.notifyUpdate(sortBy, filters1);
      const result1 = dataSource.rows.rows;

      dataSource.notifyUpdate(sortBy, filters2); // Different filters
      const result2 = dataSource.rows.rows;

      expect(result1).not.toBe(result2);
      expect(result1.map((r) => r.id)).not.toEqual(result2.map((r) => r.id));
    });

    test('data is reprocessed if filter value changes (Uint8Array)', () => {
      const filters1: FilterDefinition[] = [
        {column: 'blob', op: '=', value: new Uint8Array([1, 2])},
      ];
      const filters2: FilterDefinition[] = [
        {column: 'blob', op: '=', value: new Uint8Array([3, 4, 5])},
      ];
      const sortBy: SortBy = {direction: 'unsorted'};

      dataSource.notifyUpdate(sortBy, filters1);
      const result1 = dataSource.rows.rows;
      expect(result1.length).toBe(1);
      expect(result1[0].id).toBe(1);

      dataSource.notifyUpdate(sortBy, filters2);
      const result2 = dataSource.rows.rows;
      expect(result2.length).toBe(1);
      expect(result2[0].id).toBe(2);

      expect(result1).not.toBe(result2);
    });
  });

  test('empty data source', () => {
    const emptyDataSource = new InMemoryDataSource([]);
    const result = emptyDataSource.rows;
    expect(result.rowOffset).toBe(0);
    expect(result.totalRows).toBe(0);
    expect(result.rows).toEqual([]);

    emptyDataSource.notifyUpdate({column: 'id', direction: 'asc'}, [
      {column: 'name', op: '=', value: 'test'},
    ]);
    const resultAfterUpdate = emptyDataSource.rows;
    expect(resultAfterUpdate.totalRows).toBe(0);
    expect(resultAfterUpdate.rows).toEqual([]);
  });
});

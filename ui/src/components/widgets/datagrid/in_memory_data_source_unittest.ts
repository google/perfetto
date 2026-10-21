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

import type {Row} from '../../../trace_processor/query_result';
import type {FlatModel} from './data_source';
import {InMemoryDataSource} from './in_memory_data_source';
import type {Filter, QuickFilter} from './model';

describe('InMemoryDataSource', () => {
  const sampleData: ReadonlyArray<Row> = [
    {
      'id': 1,
      'name': 'Alice',
      'value': 100,
      'active': 1,
      'tag': 'A',
      'blob': new Uint8Array([1, 2]),
      'foo.bar': 'foo.bar value 1',
    },
    {
      'id': 2,
      'name': 'Bob',
      'value': 200,
      'active': 0,
      'tag': 'B',
      'blob': new Uint8Array([3, 4, 5]),
      'foo.bar': 'foo.bar value 2',
    },
    {
      'id': 3,
      'name': 'Charlie',
      'value': 150,
      'active': 1,
      'tag': 'A',
      'blob': null,
      'foo.bar': 'foo.bar value 3',
    },
    {
      'id': 4,
      'name': 'David',
      'value': null,
      'active': 0,
      'tag': 'C',
      'blob': new Uint8Array([6]),
      'foo.bar': 'foo.bar value 1',
    },
    {
      'id': 5,
      'name': 'Eve',
      'value': 100,
      'active': 1,
      'tag': 'B',
      'blob': new Uint8Array([7, 8, 9, 0]),
      'foo.bar': 'foo.bar value 2',
    },
    {
      'id': 6,
      'name': 'Mallory',
      'value': 300n,
      'active': 0,
      'tag': 'C',
      'blob': new Uint8Array([0]),
      'foo.bar': 'foo.bar value 3',
    },
    {
      'id': 7,
      'name': 'Trent',
      'value': 250n,
      'active': 1,
      'tag': 'A',
      'blob': new Uint8Array([1, 1]),
      'foo.bar': 'foo.bar value 4',
    },
  ];

  // Default columns that include all fields (using field name as alias)
  const defaultColumns: FlatModel['columns'] = [
    {field: 'id', alias: 'id'},
    {field: 'name', alias: 'name'},
    {field: 'value', alias: 'value'},
    {field: 'active', alias: 'active'},
    {field: 'tag', alias: 'tag'},
    {field: 'blob', alias: 'blob'},
  ];

  function makeModel(options?: {
    columns?: FlatModel['columns'];
    filters?: readonly Filter[];
    quickFilter?: QuickFilter;
    sort?: FlatModel['sort'];
  }): FlatModel {
    return {
      mode: 'flat',
      columns: options?.columns ?? defaultColumns,
      filters: options?.filters,
      quickFilter: options?.quickFilter,
      sort: options?.sort,
    };
  }

  let dataSource: InMemoryDataSource;

  beforeEach(() => {
    dataSource = new InMemoryDataSource([...sampleData]); // Use a copy for each test
  });

  test('initialization', () => {
    const result = dataSource.useRows(makeModel());
    expect(result.rowOffset).toBe(0);
    expect(result.totalRows).toBe(sampleData.length);
    expect(result.rows?.map((r) => r.id)).toEqual(sampleData.map((r) => r.id));
  });

  describe('filtering', () => {
    test('equality filter', () => {
      const filters: Filter[] = [{field: 'name', op: '=', value: 'Alice'}];
      const result = dataSource.useRows(makeModel({filters}));
      expect(result.totalRows).toBe(1);
      expect(result.rows?.[0]!.name).toBe('Alice');
    });

    test('inequality filter', () => {
      const filters: Filter[] = [{field: 'active', op: '!=', value: 1}];
      const result = dataSource.useRows(makeModel({filters}));
      expect(result.totalRows).toBe(3); // Bob, David, Mallory
      result.rows?.forEach((row) => expect(row.active).toBe(0));
    });

    test('less than filter', () => {
      const filters: Filter[] = [{field: 'value', op: '<', value: 150}];
      const result = dataSource.useRows(makeModel({filters}));
      // David (null), Alice (100), Eve (100)
      expect(result.totalRows).toBe(3);
      expect(result.rows?.map((r) => r.id).sort()).toEqual([1, 4, 5]);
    });

    test('less than or equal filter', () => {
      const filters: Filter[] = [{field: 'value', op: '<=', value: 150}];
      const result = dataSource.useRows(makeModel({filters}));
      // David (null), Alice (100), Charlie (150), Eve (100)
      expect(result.totalRows).toBe(4);
      expect(result.rows?.map((r) => r.id).sort()).toEqual([1, 3, 4, 5]);
    });

    test('greater than filter', () => {
      const filters: Filter[] = [{field: 'value', op: '>', value: 200}];
      const result = dataSource.useRows(makeModel({filters}));
      expect(result.totalRows).toBe(2); // Mallory (300n), Trent (250n)
      expect(result.rows?.map((r) => r.id).sort()).toEqual([6, 7]);
    });

    test('greater than or equal filter with bigint', () => {
      const filters: Filter[] = [{field: 'value', op: '>=', value: 250n}];
      const result = dataSource.useRows(makeModel({filters}));
      expect(result.totalRows).toBe(2); // Mallory, Trent
      expect(result.rows?.map((r) => r.id).sort()).toEqual([6, 7]);
    });

    test('is null filter', () => {
      const filters: Filter[] = [{field: 'value', op: 'is null'}];
      const result = dataSource.useRows(makeModel({filters}));
      expect(result.totalRows).toBe(1);
      expect(result.rows?.[0]!.id).toBe(4); // David
    });

    test('is not null filter', () => {
      const filters: Filter[] = [{field: 'blob', op: 'is not null'}];
      const result = dataSource.useRows(makeModel({filters}));
      expect(result.totalRows).toBe(6); // All except Charlie
      expect(result.rows?.find((r) => r.id === 3)).toBeUndefined();
    });

    test('glob filter', () => {
      const filters: Filter[] = [{field: 'name', op: 'glob', value: 'A*e'}];
      const result = dataSource.useRows(makeModel({filters}));
      expect(result.totalRows).toBe(1);
      expect(result.rows?.[0]!.name).toBe('Alice');
    });

    test('glob filter with ?', () => {
      const filters: Filter[] = [{field: 'name', op: 'glob', value: 'B?b'}];
      const result = dataSource.useRows(makeModel({filters}));
      expect(result.totalRows).toBe(1);
      expect(result.rows?.[0]!.name).toBe('Bob');
    });

    test('multiple filters', () => {
      const filters: Filter[] = [
        {field: 'active', op: '=', value: 1},
        {field: 'tag', op: '=', value: 'A'},
      ];
      const result = dataSource.useRows(makeModel({filters}));
      expect(result.totalRows).toBe(3); // Alice, Charlie, Trent
      result.rows?.forEach((row) => {
        expect(row.active).toBe(1);
        expect(row.tag).toBe('A');
      });
    });

    test('no matching rows filter', () => {
      const filters: Filter[] = [
        {field: 'name', op: '=', value: 'NonExistent'},
      ];
      const result = dataSource.useRows(makeModel({filters}));
      expect(result.totalRows).toBe(0);
      expect(result.rows?.length).toBe(0);
    });
  });

  describe('quick filter', () => {
    const ids = (model: FlatModel) =>
      dataSource.useRows(model).rows?.map((r) => r.id);

    test('keeps rows where any field matches, ignoring case', () => {
      const quickFilter = {text: 'AL', fields: ['name', 'tag']};
      // Alice (name), Mallory (name).
      expect(ids(makeModel({quickFilter}))).toEqual([1, 6]);
    });

    test('only looks in the given fields', () => {
      const quickFilter = {text: 'a', fields: ['tag']};
      expect(ids(makeModel({quickFilter}))).toEqual([1, 3, 7]);
    });

    test('never matches a field that is missing from the row', () => {
      // A missing value must not be treated as the text "undefined".
      const quickFilter = {text: 'undefined', fields: ['missing']};
      expect(ids(makeModel({quickFilter}))).toEqual([]);
    });

    test('can look in a field with a dot in the name', () => {
      // 'foo..bar' is the escaped path for the 'foo.bar' key.
      const quickFilter = {text: 'value 3', fields: ['foo..bar']};
      // Charlie, Mallory.
      expect(ids(makeModel({quickFilter}))).toEqual([3, 6]);
    });

    test('empty text keeps all rows', () => {
      const quickFilter = {text: '', fields: ['name']};
      expect(dataSource.useRows(makeModel({quickFilter})).totalRows).toBe(
        sampleData.length,
      );
    });

    test('is combined with filters using AND', () => {
      const filters: Filter[] = [{field: 'active', op: '=', value: 1}];
      const quickFilter = {text: 'e', fields: ['name']};
      // Alice, Charlie, Eve, Trent have an 'e' and are active.
      expect(ids(makeModel({filters, quickFilter}))).toEqual([1, 3, 5, 7]);
    });

    test('changing the quick filter text recomputes rows', () => {
      const fields = ['name'];
      expect(ids(makeModel({quickFilter: {text: 'bob', fields}}))).toEqual([2]);
      expect(ids(makeModel({quickFilter: {text: 'eve', fields}}))).toEqual([5]);
    });
  });

  describe('sorting', () => {
    test('sort by string ascending', () => {
      const result = dataSource.useRows(
        makeModel({sort: {alias: 'name', direction: 'ASC'}}),
      );
      expect(result.rows?.map((r) => r.name)).toEqual([
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
      const result = dataSource.useRows(
        makeModel({sort: {alias: 'name', direction: 'DESC'}}),
      );
      expect(result.rows?.map((r) => r.name)).toEqual([
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
      const result = dataSource.useRows(
        makeModel({sort: {alias: 'value', direction: 'ASC'}}),
      );
      // Nulls first, then 100, 100, 150, 200, 250n, 300n
      expect(result.rows?.map((r) => r.id)).toEqual([4, 1, 5, 3, 2, 7, 6]);
    });

    test('sort by number descending (includes nulls and bigint)', () => {
      const result = dataSource.useRows(
        makeModel({sort: {alias: 'value', direction: 'DESC'}}),
      );
      // 300n, 250n, 200, 150, 100, 100, Nulls last
      expect(result.rows?.map((r) => r.id)).toEqual([6, 7, 2, 3, 1, 5, 4]);
    });

    test('sort by boolean ascending', () => {
      const result = dataSource.useRows(
        makeModel({sort: {alias: 'active', direction: 'ASC'}}),
      );
      expect(result.rows?.map((r) => r.active)).toEqual([0, 0, 0, 1, 1, 1, 1]);
    });

    test('sort by Uint8Array ascending (by length)', () => {
      const result = dataSource.useRows(
        makeModel({sort: {alias: 'blob', direction: 'ASC'}}),
      );
      // null (Charlie, id:3), len 1 (David id:4, Mallory id:6), len 2 (Alice id:1, Trent id:7), len 3 (Bob id:2), len 4 (Eve id:5)
      // Original order for same length: David before Mallory, Alice before Trent.
      expect(result.rows?.map((r) => r.id)).toEqual([3, 4, 6, 1, 7, 2, 5]);
    });

    test('sort by Uint8Array descending (by length)', () => {
      const result = dataSource.useRows(
        makeModel({sort: {alias: 'blob', direction: 'DESC'}}),
      );
      // len 4, len 3, len 2, len 2, len 1, len 0, null
      expect(result.rows?.map((r) => r.id)).toEqual([5, 2, 1, 7, 4, 6, 3]);
    });

    test('unsorted', () => {
      // Apply some sort first
      dataSource.useRows(makeModel({sort: {alias: 'name', direction: 'ASC'}}));
      // Then unsort
      const result = dataSource.useRows(makeModel());
      // Should revert to original order if no filters applied
      expect(result.rows?.map((r) => r.id)).toEqual(
        sampleData.map((r) => r.id),
      );
    });
  });

  describe('combined filtering and sorting', () => {
    test('filter then sort', () => {
      const filters: Filter[] = [{field: 'active', op: '=', value: 1}];
      const result = dataSource.useRows(
        makeModel({filters, sort: {alias: 'value', direction: 'DESC'}}),
      );
      // Active: Alice (100), Charlie (150), Eve (100), Trent (250n)
      // Sorted by value desc: Trent, Charlie, Alice, Eve (Alice/Eve order by original due to stable sort on value)
      expect(result.rows?.map((r) => r.id)).toEqual([7, 3, 1, 5]);
      result.rows?.forEach((row) => expect(row.active).toBe(1));
    });
  });

  describe('aggregate summaries', () => {
    function summarize(model: FlatModel) {
      dataSource.useRows(model);
      const result = dataSource.useAggregateSummaries(model);
      expect(result.isPending).toBe(false);
      return result.data;
    }

    test('returns an empty row when no columns have aggregates', () => {
      expect(summarize(makeModel())).toEqual({});
    });

    test('computes aggregates over numeric values, ignoring nulls', () => {
      // Values: 100, 200, 150, null, 100, 300n, 250n
      const summaries = summarize(
        makeModel({
          columns: [
            {field: 'value', alias: 'sum', aggregate: 'SUM'},
            {field: 'value', alias: 'avg', aggregate: 'AVG'},
            {field: 'value', alias: 'min', aggregate: 'MIN'},
            {field: 'value', alias: 'max', aggregate: 'MAX'},
            {field: 'value', alias: 'distinct', aggregate: 'COUNT_DISTINCT'},
            {field: 'value', alias: 'p25', aggregate: 'P25'},
            {field: 'value', alias: 'p50', aggregate: 'P50'},
          ],
        }),
      );
      expect(summaries).toEqual({
        sum: 1100,
        avg: 1100 / 6,
        min: 100,
        max: 300n,
        distinct: 5,
        p25: 112.5,
        p50: 175,
      });
    });

    test('computes aggregates over string values', () => {
      const summaries = summarize(
        makeModel({
          columns: [
            {field: 'name', alias: 'any', aggregate: 'ANY'},
            {field: 'name', alias: 'min', aggregate: 'MIN'},
            {field: 'name', alias: 'max', aggregate: 'MAX'},
            {field: 'tag', alias: 'distinct', aggregate: 'COUNT_DISTINCT'},
          ],
        }),
      );
      expect(summaries).toEqual({
        any: 'Alice',
        min: 'Alice',
        max: 'Trent',
        distinct: 3,
      });
    });

    test('keeps bigint precision when summing only bigints', () => {
      // Tag C: David (null), Mallory (300n)
      const summaries = summarize(
        makeModel({
          columns: [{field: 'value', alias: 'sum', aggregate: 'SUM'}],
          filters: [{field: 'tag', op: '=', value: 'C'}],
        }),
      );
      expect(summaries).toEqual({sum: 300n});
    });

    test('returns null (or 0 for COUNT_DISTINCT) when all values are null', () => {
      const summaries = summarize(
        makeModel({
          columns: [
            {field: 'value', alias: 'sum', aggregate: 'SUM'},
            {field: 'value', alias: 'avg', aggregate: 'AVG'},
            {field: 'value', alias: 'max', aggregate: 'MAX'},
            {field: 'value', alias: 'p50', aggregate: 'P50'},
            {field: 'value', alias: 'distinct', aggregate: 'COUNT_DISTINCT'},
          ],
          filters: [{field: 'name', op: '=', value: 'David'}],
        }),
      );
      expect(summaries).toEqual({
        sum: null,
        avg: null,
        max: null,
        p50: null,
        distinct: 0,
      });
    });

    test('aggregates only over filtered rows', () => {
      const columns: FlatModel['columns'] = [
        {field: 'value', alias: 'sum', aggregate: 'SUM'},
      ];
      expect(summarize(makeModel({columns}))).toEqual({sum: 1100});

      // Active: Alice (100), Charlie (150), Eve (100), Trent (250n)
      const filters: Filter[] = [{field: 'active', op: '=', value: 1}];
      expect(summarize(makeModel({columns, filters}))).toEqual({sum: 600});
    });

    test('recomputes when the aggregate function changes', () => {
      expect(
        summarize(
          makeModel({
            columns: [{field: 'value', alias: 'value', aggregate: 'SUM'}],
          }),
        ),
      ).toEqual({value: 1100});

      expect(
        summarize(
          makeModel({
            columns: [{field: 'value', alias: 'value', aggregate: 'MIN'}],
          }),
        ),
      ).toEqual({value: 100});
    });
  });

  describe('caching behavior', () => {
    test('data is not reprocessed if columns and filters are identical', () => {
      const filters: Filter[] = [{field: 'tag', op: '=', value: 'A'}];
      const sort: FlatModel['sort'] = {alias: 'name', direction: 'ASC'};

      const result1 = dataSource.useRows(makeModel({filters, sort}));

      // Identical call
      const result2 = dataSource.useRows(makeModel({filters, sort}));

      // Should be the same array instance due to caching
      expect(result1.rows).toBe(result2.rows);
    });

    test('data is reprocessed if sorting changes', () => {
      const filters: Filter[] = [{field: 'tag', op: '=', value: 'A'}];

      const result1 = dataSource.useRows(
        makeModel({filters, sort: {alias: 'name', direction: 'ASC'}}),
      );

      const result2 = dataSource.useRows(
        makeModel({filters, sort: {alias: 'name', direction: 'DESC'}}),
      );

      expect(result1.rows).not.toBe(result2.rows);
      expect(result1.rows?.map((r) => r.id)).not.toEqual(
        result2.rows?.map((r) => r.id),
      );
    });

    test('data is reprocessed if filters change', () => {
      const filters1: Filter[] = [{field: 'tag', op: '=', value: 'A'}];
      const filters2: Filter[] = [{field: 'tag', op: '=', value: 'B'}];
      const sort: FlatModel['sort'] = {alias: 'name', direction: 'ASC'};

      const result1 = dataSource.useRows(makeModel({filters: filters1, sort}));
      const result2 = dataSource.useRows(makeModel({filters: filters2, sort}));

      expect(result1.rows).not.toBe(result2.rows);
      expect(result1.rows?.map((r) => r.id)).not.toEqual(
        result2.rows?.map((r) => r.id),
      );
    });

    test('data is reprocessed if filter value changes (Uint8Array)', () => {
      const filters1: Filter[] = [
        {field: 'blob', op: '=', value: new Uint8Array([1, 2])},
      ];
      const filters2: Filter[] = [
        {field: 'blob', op: '=', value: new Uint8Array([3, 4, 5])},
      ];

      const result1 = dataSource.useRows(makeModel({filters: filters1}));
      expect(result1.rows?.length).toBe(1);
      expect(result1.rows?.[0]!.id).toBe(1);

      const result2 = dataSource.useRows(makeModel({filters: filters2}));
      expect(result2.rows?.length).toBe(1);
      expect(result2.rows?.[0]!.id).toBe(2);

      expect(result1.rows).not.toBe(result2.rows);
    });
  });

  test('empty data source', () => {
    const emptyDataSource = new InMemoryDataSource([]);
    const result = emptyDataSource.useRows(makeModel());
    expect(result.rowOffset).toBe(0);
    expect(result.totalRows).toBe(0);
    expect(result.rows).toEqual([]);

    const resultAfterUpdate = emptyDataSource.useRows(
      makeModel({
        filters: [{field: 'name', op: '=', value: 'test'}],
        sort: {alias: 'id', direction: 'DESC'},
      }),
    );
    expect(resultAfterUpdate.totalRows).toBe(0);
    expect(resultAfterUpdate.rows).toEqual([]);
  });

  test('can index field with dot in the name as well as escaped dots in the name', () => {
    const result = dataSource.useRows(
      makeModel({
        columns: [
          {field: 'foo.bar', alias: 'straight'},
          {field: 'foo..bar', alias: 'escaped'},
        ],
      }),
    );
    expect(result.rows?.[0]).toEqual({
      straight: 'foo.bar value 1',
      escaped: 'foo.bar value 1',
    });
  });

  test('can filter on a field with a dot in the name', () => {
    const filters: Filter[] = [
      {field: 'foo.bar', op: '=', value: 'foo.bar value 2'},
    ];
    const result = dataSource.useRows(
      makeModel({
        columns: [
          {field: 'id', alias: 'id'},
          {field: 'foo.bar', alias: 'fooBar'},
        ],
        filters,
      }),
    );
    expect(result.rows?.map((r) => r.id).sort()).toEqual([2, 5]); // Bob, Eve
  });

  test('can filter on a field with a dot in the name using glob', () => {
    const filters: Filter[] = [
      {field: 'foo.bar', op: 'glob', value: '*value 3'},
    ];
    const result = dataSource.useRows(
      makeModel({
        columns: [
          {field: 'id', alias: 'id'},
          {field: 'foo.bar', alias: 'fooBar'},
        ],
        filters,
      }),
    );
    expect(result.rows?.map((r) => r.id).sort()).toEqual([3, 6]); // Charlie, Mallory
  });
});

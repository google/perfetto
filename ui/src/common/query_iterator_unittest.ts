// Copyright (C) 2020 The Android Open Source Project
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

import {RawQueryResult} from './protos';
import {
  findColumnIndex,
  iter,
  NUM,
  NUM_NULL,
  slowlyCountRows,
  STR,
  STR_NULL
} from './query_iterator';

const COLUMN_TYPE_STR = RawQueryResult.ColumnDesc.Type.STRING;
const COLUMN_TYPE_DOUBLE = RawQueryResult.ColumnDesc.Type.DOUBLE;
const COLUMN_TYPE_LONG = RawQueryResult.ColumnDesc.Type.LONG;

test('Columnar iteration slowlyCountRows', () => {
  const r = new RawQueryResult({
    columnDescriptors: [{
      name: 'string_column',
      type: COLUMN_TYPE_STR,
    }],
    numRecords: 1,
    columns: [{
      stringValues: ['foo'],
      isNulls: [false],
    }],
  });

  expect(slowlyCountRows(r)).toBe(1);
});

test('Columnar iteration findColumnIndex', () => {
  const r = new RawQueryResult({
    columnDescriptors: [
      {
        name: 'strings',
        type: COLUMN_TYPE_STR,
      },
      {
        name: 'doubles',
        type: COLUMN_TYPE_DOUBLE,
      },
      {
        name: 'longs',
        type: COLUMN_TYPE_LONG,
      },
      {
        name: 'nullable_strings',
        type: COLUMN_TYPE_STR,
      },
      {
        name: 'nullable_doubles',
        type: COLUMN_TYPE_DOUBLE,
      },
      {
        name: 'nullable_longs',
        type: COLUMN_TYPE_LONG,
      },
      {
        name: 'twin',
        type: COLUMN_TYPE_LONG,
      },
      {
        name: 'twin',
        type: COLUMN_TYPE_STR,
      }
    ],
    numRecords: 1,
    columns: [
      {
        stringValues: ['foo'],
        isNulls: [false],
      },
      {
        doubleValues: [1],
        isNulls: [false],
      },
      {
        longValues: [1],
        isNulls: [false],
      },
      {
        stringValues: [''],
        isNulls: [true],
      },
      {
        doubleValues: [0],
        isNulls: [true],
      },
      {
        longValues: [0],
        isNulls: [true],
      },
      {
        doubleValues: [0],
        isNulls: [false],
      },
      {
        stringValues: [''],
        isNulls: [false],
      }
    ],
  });

  expect(findColumnIndex(r, 'strings', STR)).toBe(0);
  expect(findColumnIndex(r, 'doubles', NUM)).toBe(1);
  expect(findColumnIndex(r, 'longs', NUM)).toBe(2);

  expect(findColumnIndex(r, 'nullable_strings', STR_NULL)).toBe(3);
  expect(findColumnIndex(r, 'nullable_doubles', NUM_NULL)).toBe(4);
  expect(findColumnIndex(r, 'nullable_longs', NUM_NULL)).toBe(5);

  expect(() => findColumnIndex(r, 'no such col', NUM)).toThrow(Error);

  // It's allowable to expect nulls but for the whole column to be non-null...
  expect(findColumnIndex(r, 'strings', STR_NULL)).toBe(0);
  expect(findColumnIndex(r, 'doubles', NUM_NULL)).toBe(1);
  expect(findColumnIndex(r, 'longs', NUM_NULL)).toBe(2);

  // ...but if we expect no-nulls there shouldn't be even one:
  expect(() => findColumnIndex(r, 'nullable_strings', STR)).toThrow(Error);
  expect(() => findColumnIndex(r, 'nullable_doubles', NUM)).toThrow(Error);
  expect(() => findColumnIndex(r, 'nullable_longs', NUM)).toThrow(Error);

  // If multiple columns have the desired name we error even if we could
  // distinguish based on the type:
  expect(() => findColumnIndex(r, 'twin', NUM)).toThrow(Error);

  expect(() => findColumnIndex(r, 'strings', NUM)).toThrow(Error);
  expect(() => findColumnIndex(r, 'longs', STR)).toThrow(Error);
  expect(() => findColumnIndex(r, 'doubles', STR)).toThrow(Error);
});

test('Columnar iteration over two rows', () => {
  const r = new RawQueryResult({
    columnDescriptors: [{
      name: 'name',
      type: COLUMN_TYPE_STR,
    }],
    numRecords: 2,
    columns: [{
      stringValues: ['Alice', 'Bob'],
      isNulls: [false, false],
    }],
  });

  const it = iter({'name': STR}, r);

  expect(it.valid()).toBe(true);
  const name: string = it.row.name;
  expect(name).toBe('Alice');
  it.next();

  expect(it.valid()).toBe(true);
  expect(it.row.name).toBe('Bob');
  it.next();

  expect(it.valid()).toBe(false);
});

test('Columnar iteration over empty query set', () => {
  const r = new RawQueryResult({
    columnDescriptors: [{
      name: 'emptyColumn',
      type: COLUMN_TYPE_STR,
    }],
    numRecords: 0,
    columns: [{
      stringValues: [],
      isNulls: [],
    }],
  });

  {
    const it = iter({'emptyColumn': STR}, r);
    expect(it.valid()).toBe(false);
  }

  {
    const it = iter({'emptyColumn': NUM}, r);
    expect(it.valid()).toBe(false);
  }

  {
    const it = iter({'emptyColumn': NUM_NULL}, r);
    expect(it.valid()).toBe(false);
  }

  {
    const it = iter({'emptyColumn': STR_NULL}, r);
    expect(it.valid()).toBe(false);
  }
});

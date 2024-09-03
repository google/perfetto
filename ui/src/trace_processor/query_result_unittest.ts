// Copyright (C) 2021 The Android Open Source Project
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

import {QueryResult as QueryResultProto} from '../protos';
import {
  createQueryResult,
  decodeInt64Varint,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from './query_result';

const T = QueryResultProto.CellsBatch.CellType;

test('QueryResult.SimpleOneRow', () => {
  const batch = QueryResultProto.CellsBatch.create({
    cells: [T.CELL_STRING, T.CELL_VARINT, T.CELL_STRING, T.CELL_FLOAT64],
    varintCells: [42],
    stringCells: ['the foo', 'the bar'].join('\0'),
    float64Cells: [42.42],
    isLastBatch: true,
  });
  const resProto = QueryResultProto.create({
    columnNames: ['a_str', 'b_int', 'c_str', 'd_float'],
    batch: [batch],
  });

  const qr = createQueryResult({query: 'Some query'});
  qr.appendResultBatch(QueryResultProto.encode(resProto).finish());
  expect(qr.isComplete()).toBe(true);
  expect(qr.numRows()).toBe(1);

  // First try iterating without selecting any column.
  {
    const iter = qr.iter({});
    expect(iter.valid()).toBe(true);
    iter.next();
    expect(iter.valid()).toBe(false);
  }

  // Then select only two of them.
  {
    const iter = qr.iter({c_str: STR, d_float: NUM});
    expect(iter.valid()).toBe(true);
    expect(iter.c_str).toBe('the bar');
    expect(iter.d_float).toBeCloseTo(42.42);
    iter.next();
    expect(iter.valid()).toBe(false);
  }

  // If a column is not present in the result set, iter() should throw.
  expect(() => qr.iter({nx: NUM})).toThrowError(/\bnx\b.*not found/);
});

test('QueryResult.BigNumbers', () => {
  const numAndExpectedStr = [
    [0, '0'],
    [-1, '-1'],
    [-1000, '-1000'],
    [1e12, '1000000000000'],
    [1e12 * -1, '-1000000000000'],
    [((1 << 31) - 1) | 0, '2147483647'],
    [1 << 31, '-2147483648'],
    [Number.MAX_SAFE_INTEGER, '9007199254740991'],
    [Number.MIN_SAFE_INTEGER, '-9007199254740991'],
  ];
  const batch = QueryResultProto.CellsBatch.create({
    cells: new Array<number>(numAndExpectedStr.length).fill(T.CELL_VARINT),
    varintCells: numAndExpectedStr.map((x) => x[0]) as number[],
    isLastBatch: true,
  });
  const resProto = QueryResultProto.create({
    columnNames: ['n'],
    batch: [batch],
  });

  const qr = createQueryResult({query: 'Some query'});
  qr.appendResultBatch(QueryResultProto.encode(resProto).finish());
  const actual: string[] = [];
  for (const iter = qr.iter({n: NUM}); iter.valid(); iter.next()) {
    actual.push(BigInt(iter.n).toString());
  }
  expect(actual).toEqual(numAndExpectedStr.map((x) => x[1]) as string[]);
});

test('QueryResult.Floats', () => {
  const floats = [
    0.0,
    1.0,
    -1.0,
    3.14159265358,
    Number.MIN_SAFE_INTEGER,
    Number.MAX_SAFE_INTEGER,
    Number.NEGATIVE_INFINITY,
    Number.POSITIVE_INFINITY,
    Number.NaN,
  ];
  const batch = QueryResultProto.CellsBatch.create({
    cells: new Array<number>(floats.length).fill(T.CELL_FLOAT64),
    float64Cells: floats,
    isLastBatch: true,
  });
  const resProto = QueryResultProto.create({
    columnNames: ['n'],
    batch: [batch],
  });

  const qr = createQueryResult({query: 'Some query'});
  qr.appendResultBatch(QueryResultProto.encode(resProto).finish());
  const actual: number[] = [];
  for (const iter = qr.iter({n: NUM}); iter.valid(); iter.next()) {
    actual.push(iter.n);
  }
  expect(actual).toEqual(floats);
});

test('QueryResult.Strings', () => {
  const strings = [
    'a',
    '',
    '',
    'hello world',
    'In einem Bächlein helle da schoß in froher Eil',
    '色は匂へど散りぬるを我が世誰ぞ常ならん有為の奥山今日越えて浅き夢見じ酔ひもせず',
  ];
  const batch = QueryResultProto.CellsBatch.create({
    cells: new Array<number>(strings.length).fill(T.CELL_STRING),
    stringCells: strings.join('\0'),
    isLastBatch: true,
  });
  const resProto = QueryResultProto.create({
    columnNames: ['s'],
    batch: [batch],
  });

  const qr = createQueryResult({query: 'Some query'});
  qr.appendResultBatch(QueryResultProto.encode(resProto).finish());
  const actual: string[] = [];
  for (const iter = qr.iter({s: STR}); iter.valid(); iter.next()) {
    actual.push(iter.s);
  }
  expect(actual).toEqual(strings);
});

test('QueryResult.NullChecks', () => {
  const cells: number[] = [];
  cells.push(T.CELL_VARINT, T.CELL_NULL);
  cells.push(T.CELL_NULL, T.CELL_STRING);
  cells.push(T.CELL_VARINT, T.CELL_STRING);
  const batch = QueryResultProto.CellsBatch.create({
    cells,
    varintCells: [1, 2],
    stringCells: ['a', 'b'].join('\0'),
    isLastBatch: true,
  });
  const resProto = QueryResultProto.create({
    columnNames: ['n', 's'],
    batch: [batch],
  });

  const qr = createQueryResult({query: 'Some query'});
  qr.appendResultBatch(QueryResultProto.encode(resProto).finish());
  const actualNums = new Array<number | null>();
  const actualStrings = new Array<string | null>();
  for (
    const iter = qr.iter({n: NUM_NULL, s: STR_NULL});
    iter.valid();
    iter.next()
  ) {
    actualNums.push(iter.n);
    actualStrings.push(iter.s);
  }
  expect(actualNums).toEqual([1, null, 2]);
  expect(actualStrings).toEqual([null, 'a', 'b']);

  // Check that using NUM / STR throws.
  expect(() => qr.iter({n: NUM_NULL, s: STR})).toThrowError(
    /col: 's'.*is NULL.*not expected/,
  );
  expect(() => qr.iter({n: NUM, s: STR_NULL})).toThrowError(
    /col: 'n'.*is NULL.*not expected/,
  );
  expect(qr.iter({n: NUM_NULL})).toBeTruthy();
  expect(qr.iter({s: STR_NULL})).toBeTruthy();
});

test('QueryResult.EarlyError', () => {
  const resProto = QueryResultProto.create({
    columnNames: [],
    batch: [{isLastBatch: true}],
    error: 'Oh dear, this SQL query is too complicated, I give up',
  });
  const qr = createQueryResult({query: 'Some query'});
  qr.appendResultBatch(QueryResultProto.encode(resProto).finish());
  expect(qr.error()).toContain('Oh dear');
  expect(qr.isComplete()).toBe(true);
  const iter = qr.iter({});
  expect(iter.valid()).toBe(false);
});

test('QueryResult.LateError', () => {
  const resProto = QueryResultProto.create({
    columnNames: ['n'],
    batch: [
      {
        cells: [T.CELL_VARINT],
        varintCells: [1],
      },
      {
        cells: [T.CELL_VARINT],
        varintCells: [2],
        isLastBatch: true,
      },
    ],
    error: 'I tried, I was getting there, but then I failed',
  });
  const qr = createQueryResult({query: 'Some query'});
  qr.appendResultBatch(QueryResultProto.encode(resProto).finish());
  expect(qr.error()).toContain('I failed');
  const rows: number[] = [];
  for (const iter = qr.iter({n: NUM}); iter.valid(); iter.next()) {
    rows.push(iter.n);
  }
  expect(rows).toEqual([1, 2]);
  expect(qr.isComplete()).toBe(true);
});

test('QueryResult.MultipleBatches', async () => {
  const batch1 = QueryResultProto.create({
    columnNames: ['n'],
    batch: [
      {
        cells: [T.CELL_VARINT],
        varintCells: [1],
        isLastBatch: false,
      },
    ],
  });
  const batch2 = QueryResultProto.create({
    batch: [
      {
        cells: [T.CELL_VARINT],
        varintCells: [2],
        isLastBatch: true,
      },
    ],
  });

  const qr = createQueryResult({query: 'Some query'});
  expect(qr.isComplete()).toBe(false);

  qr.appendResultBatch(QueryResultProto.encode(batch1).finish());
  qr.appendResultBatch(QueryResultProto.encode(batch2).finish());

  const awaitRes = await qr;

  expect(awaitRes.isComplete()).toBe(true);
  expect(qr.isComplete()).toBe(true);

  expect(awaitRes.numRows()).toBe(2);
  expect(qr.numRows()).toBe(2);
});

// Regression test for b/194891824 .
test('QueryResult.DuplicateColumnNames', () => {
  const batch = QueryResultProto.CellsBatch.create({
    cells: [
      T.CELL_VARINT,
      T.CELL_STRING,
      T.CELL_FLOAT64,
      T.CELL_STRING,
      T.CELL_STRING,
    ],
    varintCells: [42],
    stringCells: ['a', 'b', 'c'].join('\0'),
    float64Cells: [4.2],
    isLastBatch: true,
  });
  const resProto = QueryResultProto.create({
    columnNames: ['x', 'y', 'x', 'x', 'y'],
    batch: [batch],
  });

  const qr = createQueryResult({query: 'Some query'});
  qr.appendResultBatch(QueryResultProto.encode(resProto).finish());
  expect(qr.isComplete()).toBe(true);
  expect(qr.numRows()).toBe(1);
  expect(qr.columns()).toEqual(['x', 'y', 'x_1', 'x_2', 'y_1']);
  // First try iterating without selecting any column.
  {
    const iter = qr.iter({x: NUM, y: STR, x_1: NUM, x_2: STR, y_1: STR});
    expect(iter.valid()).toBe(true);
    expect(iter.x).toBe(42);
    expect(iter.y).toBe('a');
    expect(iter.x_1).toBe(4.2);
    expect(iter.x_2).toBe('b');
    expect(iter.y_1).toBe('c');
    iter.next();
    expect(iter.valid()).toBe(false);
  }
  expect(() => qr.iter({x_3: NUM})).toThrowError(/\bx_3\b.*not found/);
});

test('QueryResult.WaitMoreRows', async () => {
  const batchA = QueryResultProto.CellsBatch.create({
    cells: [T.CELL_VARINT],
    varintCells: [42],
    isLastBatch: false,
  });
  const resProtoA = QueryResultProto.create({
    columnNames: ['a_int'],
    batch: [batchA],
  });

  const qr = createQueryResult({query: 'Some query'});
  qr.appendResultBatch(QueryResultProto.encode(resProtoA).finish());

  const batchB = QueryResultProto.CellsBatch.create({
    cells: [T.CELL_VARINT],
    varintCells: [43],
    isLastBatch: true,
  });
  const resProtoB = QueryResultProto.create({
    columnNames: [],
    batch: [batchB],
  });

  const waitPromise = qr.waitMoreRows();
  const appendPromise = new Promise<void>((resolve, _) => {
    setTimeout(() => {
      qr.appendResultBatch(QueryResultProto.encode(resProtoB).finish());
      resolve();
    }, 0);
  });

  expect(qr.isComplete()).toBe(false);
  expect(qr.numRows()).toBe(1);

  await Promise.all([waitPromise, appendPromise]);

  expect(qr.isComplete()).toBe(true);
  expect(qr.numRows()).toBe(2);
});

describe('decodeInt64Varint', () => {
  test('Parsing empty input should throw an error', () => {
    expect(() => decodeInt64Varint(new Uint8Array(), 0)).toThrow(
      'Index out of range',
    );
  });

  test('Parsing single byte positive integers', () => {
    const testData: Array<[Uint8Array, BigInt]> = [
      [new Uint8Array([0x00]), 0n],
      [new Uint8Array([0x01]), 1n],
      [new Uint8Array([0x7f]), 127n],
    ];

    testData.forEach(([input, expected]) => {
      expect(decodeInt64Varint(input, 0)).toEqual(expected);
    });
  });

  test('Parsing multi-byte positive integers', () => {
    const testData: Array<[Uint8Array, BigInt]> = [
      [new Uint8Array([0x80, 0x01]), 128n],
      [new Uint8Array([0xff, 0x7f]), 16383n],
      [new Uint8Array([0x80, 0x80, 0x01]), 16384n],
      [new Uint8Array([0xff, 0xff, 0x7f]), 2097151n],
      [
        new Uint8Array([
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
        ]),
        9223372036854775807n,
      ],
    ];

    testData.forEach(([input, expected]) => {
      expect(decodeInt64Varint(input, 0)).toEqual(expected);
    });
  });

  test('Parsing negative integers', () => {
    const testData: Array<[Uint8Array, BigInt]> = [
      [
        new Uint8Array([
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01,
        ]),
        -1n,
      ],
      [
        new Uint8Array([
          0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01,
        ]),
        -2n,
      ],
      [
        new Uint8Array([
          0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01,
        ]),
        -9223372036854775808n,
      ],
    ];

    testData.forEach(([input, expected]) => {
      expect(decodeInt64Varint(input, 0)).toEqual(expected);
    });
  });

  test('Parsing with incomplete varint should throw an error', () => {
    const testData: Array<Uint8Array> = [
      new Uint8Array([0x80]),
      new Uint8Array([0x80, 0x80]),
    ];

    testData.forEach((input) => {
      expect(() => decodeInt64Varint(input, 0)).toThrow('Index out of range');
    });
  });
});

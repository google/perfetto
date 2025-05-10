// Copyright (C) 2024 The Android Open Source Project
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

import {PartitionedDataset, SourceDataset, UnionDataset} from './dataset'; // Updated imports
import {
  BLOB,
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
  UNKNOWN,
} from './query_result';

// Helper function to normalize whitespace in expected SQL queries for comparison
function normalize(query: string): string {
  return query.replace(/\s+/g, ' ').trim();
}

describe('SourceDataset', () => {
  test('get query for SourceDataset', () => {
    const dataset = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR},
    });

    expect(dataset.query()).toEqual('SELECT id, name FROM (slice)');
  });

  test('get query for SourceDataset with projection', () => {
    const dataset = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, ts: LONG},
    });

    expect(dataset.query({id: NUM, ts: LONG})).toEqual(
      'SELECT id, ts FROM (slice)',
    );
  });

  test('SourceDataset implements', () => {
    const dataset = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, ts: LONG},
    });

    expect(dataset.implements({id: NUM})).toBe(true);
    expect(dataset.implements({id: NUM, ts: LONG})).toBe(true);
    expect(dataset.implements({id: NUM, ts: LONG, name: STR})).toBe(false);
    expect(dataset.implements({id: LONG})).toBe(false); // Type mismatch

    // Check NUM implements NUM_NULL
    expect(dataset.implements({id: NUM_NULL})).toBe(true);

    // Check LONG implements LONG_NULL
    expect(dataset.implements({ts: LONG_NULL})).toBe(true);

    // Check anything implements UNKNOWN
    expect(dataset.implements({id: UNKNOWN})).toBe(true);
    expect(dataset.implements({ts: UNKNOWN})).toBe(true);

    // Check NUM_NULL does NOT implement NUM
    const nullableNumDataset = new SourceDataset({
      src: 'slice',
      schema: {id: NUM_NULL},
    });
    expect(nullableNumDataset.implements({id: NUM})).toBe(false);

    // Check LONG_NULL does NOT implement LONG
    const nullableLongDataset = new SourceDataset({
      src: 'slice',
      schema: {ts: LONG_NULL},
    });
    expect(nullableLongDataset.implements({ts: LONG})).toBe(false);
  });

  test('SourceDataset schema', () => {
    const schema = {id: NUM, ts: LONG};
    const dataset = new SourceDataset({
      src: 'slice',
      schema: schema,
    });
    expect(dataset.schema).toEqual(schema);
  });
});

describe('PartitionedDataset', () => {
  test("get query for PartitionedDataset with 'eq' filter", () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR},
    });
    const dataset = new PartitionedDataset({
      base: base,
      partition: {col: 'id', eq: 123},
      schema: {id: NUM, name: STR}, // Schema matches base here
    });

    // Expect whitespace normalized query
    const expectedQuery = normalize(`
        SELECT id, name
        FROM (SELECT id, name FROM (slice))
        WHERE id = 123
      `);
    expect(normalize(dataset.query())).toEqual(expectedQuery);
  });

  test("get query for PartitionedDataset with 'in' filter", () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR},
    });
    const dataset = new PartitionedDataset({
      base: base,
      partition: {col: 'id', in: [123, 456]},
      schema: {id: NUM, name: STR},
    });

    const expectedQuery = normalize(`
        SELECT id, name
        FROM (SELECT id, name FROM (slice))
        WHERE id IN (123, 456)
      `);
    expect(normalize(dataset.query())).toEqual(expectedQuery);
  });

  test("get query for PartitionedDataset with empty 'in' filter", () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR},
    });
    const dataset = new PartitionedDataset({
      base: base,
      partition: {col: 'id', in: []},
      schema: {id: NUM, name: STR},
    });

    const expectedQuery = normalize(`
        SELECT id, name
        FROM (SELECT id, name FROM (slice))
        WHERE 0
      `); // WHERE 0 effectively means WHERE false
    expect(normalize(dataset.query())).toEqual(expectedQuery);
  });

  test('get query for PartitionedDataset with projection', () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, ts: LONG},
    });
    const dataset = new PartitionedDataset({
      base: base,
      partition: {col: 'id', eq: 123},
      schema: {id: NUM, ts: LONG}, // Projecting only id and ts
    });

    const expectedQuery = normalize(`
        SELECT id, ts
        FROM (SELECT id, name, ts FROM (slice))
        WHERE id = 123
      `);
    expect(normalize(dataset.query())).toEqual(expectedQuery);
  });

  test('PartitionedDataset implements', () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, ts: LONG},
    });
    const dataset = new PartitionedDataset({
      base: base,
      partition: {col: 'id', eq: 123},
      schema: {id: NUM, name: STR}, // Effective schema
    });

    expect(dataset.implements({id: NUM})).toBe(true);
    expect(dataset.implements({id: NUM, name: STR})).toBe(true);
    expect(dataset.implements({id: NUM, ts: LONG})).toBe(false); // ts not in effective schema
    expect(dataset.implements({id: LONG})).toBe(false); // Type mismatch
  });

  test('PartitionedDataset schema', () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, ts: LONG},
    });
    const effectiveSchema = {id: NUM, name: STR};
    const dataset = new PartitionedDataset({
      base: base,
      partition: {col: 'id', eq: 123},
      schema: effectiveSchema,
    });
    expect(dataset.schema).toEqual(effectiveSchema);
  });
});

describe('UnionDataset', () => {
  test('get query for UnionDataset', () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR},
    });

    const dataset = new UnionDataset([
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 123},
        schema: {id: NUM, name: STR},
      }),
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 456},
        schema: {id: NUM, name: STR},
      }),
    ]);

    const expectedQuery = normalize(`
      SELECT id, name
      FROM (SELECT id, name FROM (slice))
      WHERE id IN (123, 456)
    `);

    expect(normalize(dataset.query())).toEqual(expectedQuery);
  });

  test('get query for UnionDataset with projection', () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, ts: LONG},
    });

    const dataset = new UnionDataset([
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 123},
        schema: {id: NUM, name: STR, ts: LONG},
      }),
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 456},
        schema: {id: NUM, name: STR, ts: LONG},
      }),
    ]);

    const expectedQuery = normalize(`
      SELECT id, name
      FROM (SELECT id, name, ts FROM (slice))
      WHERE id IN (123, 456)
    `);

    const overrideSchema = {id: NUM, name: STR};

    expect(normalize(dataset.query(overrideSchema))).toEqual(expectedQuery);
  });

  test('UnionDataset optimization combines Eq and In partitions', () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR},
    });

    const dataset = new UnionDataset([
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 123},
        schema: {id: NUM, name: STR},
      }),
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', in: [456, 789]},
        schema: {id: NUM, name: STR},
      }),
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 456},
        schema: {id: NUM, name: STR},
      }),
    ]);

    const expectedQuery = normalize(`
          SELECT id, name
          FROM (SELECT id, name FROM (slice))
          WHERE id IN (123, 456, 789)
        `);

    expect(normalize(dataset.query())).toEqual(expectedQuery);
  });

  test('UnionDataset optimization does not combine different bases', () => {
    const base1 = new SourceDataset({
      src: 'slice1',
      schema: {id: NUM, name: STR},
    });

    const base2 = new SourceDataset({
      src: 'slice2',
      schema: {id: NUM, name: STR},
    });

    const dataset = new UnionDataset([
      new PartitionedDataset({
        base: base1,
        partition: {col: 'id', eq: 123},
        schema: {id: NUM, name: STR},
      }),
      new PartitionedDataset({
        base: base2,
        partition: {col: 'id', eq: 456},
        schema: {id: NUM, name: STR},
      }),
    ]);

    const expectedQuery = normalize(`
      SELECT id, name
      FROM (SELECT id, name FROM (slice1))
      WHERE id = 123
      UNION ALL
      SELECT id, name
      FROM (SELECT id, name FROM (slice2))
      WHERE id = 456
    `);

    expect(normalize(dataset.query())).toEqual(normalize(expectedQuery));
  });

  test('UnionDataset optimization combines different schemas', () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, value: NUM},
    });

    const dataset = new UnionDataset([
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 123},
        schema: {id: NUM, name: STR},
      }),
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 456},
        schema: {id: NUM, value: NUM},
      }),
    ]);

    const expectedQuery = normalize(`
      SELECT id
      FROM (SELECT id, name, value FROM (slice))
      WHERE id IN (123, 456)
    `);

    expect(normalize(dataset.query())).toEqual(expectedQuery);
  });

  test('UnionDataset optimization combines different partition columns', () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR},
    });

    const dataset = new UnionDataset([
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 123},
        schema: {id: NUM, name: STR},
      }),
      new PartitionedDataset({
        base: base,
        partition: {col: 'name', eq: 'foo'},
        schema: {id: NUM, name: STR},
      }),
    ]);

    const expectedQuery = normalize(`
      SELECT id, name
      FROM (SELECT id, name FROM (slice))
      WHERE id = 123 OR name = 'foo'
    `);

    expect(normalize(dataset.query())).toEqual(normalize(expectedQuery));
  });

  test('UnionDataset optimization handles mixed optimizable and non-optimizable', () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR},
    });

    const dataset = new UnionDataset([
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 123},
        schema: {id: NUM, name: STR},
      }),
      new SourceDataset({
        src: 'other',
        schema: {id: NUM, name: STR},
      }),
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 456},
        schema: {id: NUM, name: STR},
      }),
    ]);

    const expectedQuery = normalize(`
      SELECT id, name
      FROM (SELECT id, name FROM (slice))
      WHERE id IN (123, 456)
      UNION ALL
      SELECT id, name FROM (other)
    `);

    expect(normalize(dataset.query())).toEqual(normalize(expectedQuery));
  });

  test('UnionDataset batches large numbers of unions', () => {
    const base = new SourceDataset({
      src: 'foo',
      schema: {bar: NUM, some_id: NUM},
    });
    const datasets = [];
    for (let i = 0; i < 800; i++) {
      datasets.push(base);
    }
    const query = new UnionDataset(datasets).query();

    expect(query).toContain('WITH');
    expect(query).toContain('union_batch_0 AS');
    expect(query).toContain('union_batch_1 AS');

    expect(query).toMatch(
      /SELECT bar, some_id FROM union_batch_0\s*UNION ALL\s*SELECT bar, some_id FROM union_batch_1/,
    );

    expect(query).toContain('SELECT bar, some_id');
    expect(query).toContain('FROM (foo)');

    const unionAllMatches = query.match(/UNION ALL/g);
    expect(unionAllMatches?.length).toBe(799);
  });

  test('UnionDataset implements', () => {
    const base = new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, ts: LONG},
    });
    const dataset = new UnionDataset([
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 1},
        schema: {id: NUM, name: STR},
      }),
      new PartitionedDataset({
        base: base,
        partition: {col: 'id', eq: 2},
        schema: {id: NUM, ts: LONG},
      }),
    ]);

    expect(dataset.implements({id: NUM})).toBe(true);
    expect(dataset.implements({id: NUM, name: STR})).toBe(false);
    expect(dataset.implements({id: NUM, ts: LONG})).toBe(false);
    expect(dataset.implements({id: LONG})).toBe(false);
  });

  test('UnionDataset schema calculation - differing names', () => {
    const dataset = new UnionDataset([
      new SourceDataset({src: 'table1', schema: {foo: NUM, common: STR}}),
      new SourceDataset({src: 'table2', schema: {bar: NUM, common: STR}}),
    ]);
    expect(dataset.schema).toEqual({common: STR});
  });

  test('UnionDataset schema calculation - differing types', () => {
    const dataset = new UnionDataset([
      new SourceDataset({src: 'table1', schema: {foo: NUM, common: STR}}),
      new SourceDataset({src: 'table2', schema: {foo: LONG, common: STR}}),
    ]);
    expect(dataset.schema).toEqual({foo: UNKNOWN, common: STR});
  });

  test('UnionDataset schema calculation - one common column', () => {
    const dataset = new UnionDataset([
      new SourceDataset({src: 'table1', schema: {foo: NUM, bar: NUM}}),
      new SourceDataset({src: 'table2', schema: {foo: NUM, baz: NUM}}),
    ]);
    expect(dataset.schema).toEqual({foo: NUM});
  });

  test('UnionDataset schema calculation - type widening', () => {
    const dataset = new UnionDataset([
      new SourceDataset({
        src: 'slice',
        schema: {foo: NUM, bar: STR_NULL, baz: BLOB, qux: NUM_NULL},
      }),
      new SourceDataset({
        src: 'slice',
        schema: {foo: NUM_NULL, bar: STR, baz: LONG, qux: NUM},
      }),
    ]);

    expect(dataset.schema).toEqual({
      foo: NUM_NULL,
      bar: STR_NULL,
      baz: UNKNOWN,
      qux: NUM_NULL,
    });
  });
});

// describe('Dataset Shared Functionality', () => {
//   test.each([
//     ['SourceDataset', new SourceDataset({src: 't', schema: {a: NUM_NULL}})],
//     [
//       'PartitionedDataset',
//       new PartitionedDataset({
//         base: new SourceDataset({src: 't', schema: {a: NUM_NULL, p: NUM}}),
//         partition: {col: 'p', eq: 1},
//         schema: {a: NUM_NULL},
//       }),
//     ],
//     [
//       'UnionDataset',
//       new UnionDataset([
//         new SourceDataset({src: 't1', schema: {a: NUM_NULL}}),
//         new SourceDataset({src: 't2', schema: {a: NUM}}),
//       ]),
//     ],
//   ])(
//     '%s implements with relaxed compat checks on optional types',
//     (_, dataset) => {
//       expect(dataset.implements({a: NUM_NULL})).toBe(true);
//       expect(dataset.implements({a: NUM})).toBe(false);

//       const datasetNonNullable = new SourceDataset({
//         src: 't',
//         schema: {a: NUM},
//       });
//       const partitionedNonNullable = new PartitionedDataset({
//         base: new SourceDataset({src: 't', schema: {a: NUM, p: NUM}}),
//         partition: {col: 'p', eq: 1},
//         schema: {a: NUM},
//       });
//       const unionNonNullable = new UnionDataset([
//         new SourceDataset({src: 't1', schema: {a: NUM}}),
//       ]);

//       expect(datasetNonNullable.implements({a: NUM_NULL})).toBe(true);
//       expect(partitionedNonNullable.implements({a: NUM_NULL})).toBe(true);
//       expect(unionNonNullable.implements({a: NUM_NULL})).toBe(true);

//       expect(datasetNonNullable.implements({a: NUM})).toBe(true);
//       expect(partitionedNonNullable.implements({a: NUM})).toBe(true);
//       expect(unionNonNullable.implements({a: NUM})).toBe(true);
//     },
//   );
// });

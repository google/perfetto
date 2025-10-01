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

import {SourceDataset, UnionDataset} from './dataset';
import {
  BLOB,
  BLOB_NULL,
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
  UNKNOWN,
} from './query_result';

test('get query for simple dataset', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM},
  });

  expect(dataset.query()).toEqual('SELECT id FROM (slice)');
});

test("get query for simple dataset with 'eq' filter", () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM},
    filter: {
      col: 'id',
      eq: 123,
    },
  });

  expect(dataset.query()).toEqual('SELECT id FROM (slice) WHERE id = 123');
});

test("get query for simple dataset with an 'in' filter", () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM},
    filter: {
      col: 'id',
      in: [123, 456],
    },
  });

  expect(dataset.query()).toEqual(
    'SELECT id FROM (slice) WHERE id IN (123,456)',
  );
});

test('get query for union dataset', () => {
  const dataset = new UnionDataset([
    new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {
        col: 'id',
        eq: 123,
      },
    }),
    new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {
        col: 'id',
        eq: 456,
      },
    }),
  ]);

  expect(dataset.query()).toEqual(
    'SELECT id FROM (slice) WHERE id = 123\nunion all\nSELECT id FROM (slice) WHERE id = 456',
  );
});

test('union dataset batches large numbers of unions', () => {
  const datasets = [];
  for (let i = 0; i < 800; i++) {
    datasets.push(
      new SourceDataset({
        src: 'foo',
        schema: {bar: NUM},
        filter: {
          col: 'some_id',
          eq: i,
        },
      }),
    );
  }

  const query = new UnionDataset(datasets).query();

  // Verify query structure with CTE batching.
  expect(query).toContain('with');

  // Should have at least 2 CTE batches.
  expect(query).toContain('union_batch_0 as');
  expect(query).toContain('union_batch_1 as');

  // 798 union alls within batches (for 800 datasets) + 1 union alls between the
  // 2 CTEs.
  const batchMatches = query.match(/union all/g);
  expect(batchMatches?.length).toBe(799);
});

test('implements', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, ts: LONG},
  });

  expect(dataset.implements({id: NUM})).toBe(true);
  expect(dataset.implements({id: NUM, ts: LONG})).toBe(true);
  expect(dataset.implements({id: NUM, ts: LONG, name: STR})).toBe(false);
  expect(dataset.implements({id: LONG})).toBe(false);
});

test('implements with relaxed compat checks on optional types', () => {
  expect(
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM_NULL, bar: LONG_NULL, baz: STR_NULL, qux: BLOB_NULL},
    }).implements({
      foo: NUM_NULL,
      bar: LONG_NULL,
      baz: STR_NULL,
      qux: BLOB_NULL,
    }),
  ).toBe(true);

  expect(
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM, bar: LONG, baz: STR, qux: BLOB},
    }).implements({
      foo: NUM_NULL,
      bar: LONG_NULL,
      baz: STR_NULL,
      qux: BLOB_NULL,
    }),
  ).toBe(true);

  expect(
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM_NULL, bar: LONG_NULL, baz: STR_NULL, qux: BLOB_NULL},
    }).implements({
      foo: NUM,
      bar: LONG,
      baz: STR,
      qux: BLOB,
    }),
  ).toBe(false);
});

test('find the schema of a simple dataset', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, ts: LONG},
  });

  expect(dataset.schema).toMatchObject({id: NUM, ts: LONG});
});

test('find the schema of a union where source sets differ in their names', () => {
  const dataset = new UnionDataset([
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM},
    }),
    new SourceDataset({
      src: 'slice',
      schema: {bar: NUM},
    }),
  ]);

  expect(dataset.schema).toMatchObject({});
});

test('find the schema of a union with differing source sets', () => {
  const dataset = new UnionDataset([
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM},
    }),
    new SourceDataset({
      src: 'slice',
      schema: {foo: LONG},
    }),
  ]);

  expect(dataset.schema).toMatchObject({});
});

test('find the schema of a union with one column in common', () => {
  const dataset = new UnionDataset([
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM, bar: NUM},
    }),
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM, baz: NUM},
    }),
  ]);

  expect(dataset.schema).toMatchObject({foo: NUM});
});

test('optimize a union dataset', () => {
  const dataset = new UnionDataset([
    new SourceDataset({
      src: 'slice',
      schema: {},
      filter: {
        col: 'track_id',
        eq: 123,
      },
    }),
    new SourceDataset({
      src: 'slice',
      schema: {},
      filter: {
        col: 'track_id',
        eq: 456,
      },
    }),
  ]);

  expect(dataset.optimize()).toEqual({
    src: 'slice',
    schema: {},
    filter: {
      col: 'track_id',
      in: [123, 456],
    },
  });
});

test('optimize a union dataset with different types of filters', () => {
  const dataset = new UnionDataset([
    new SourceDataset({
      src: 'slice',
      schema: {},
      filter: {
        col: 'track_id',
        eq: 123,
      },
    }),
    new SourceDataset({
      src: 'slice',
      schema: {},
      filter: {
        col: 'track_id',
        in: [456, 789],
      },
    }),
  ]);

  expect(dataset.optimize()).toEqual({
    src: 'slice',
    schema: {},
    filter: {
      col: 'track_id',
      in: [123, 456, 789],
    },
  });
});

test('optimize a union dataset with different schemas', () => {
  const dataset = new UnionDataset([
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM},
    }),
    new SourceDataset({
      src: 'slice',
      schema: {bar: NUM},
    }),
  ]);

  expect(dataset.optimize()).toEqual({
    src: 'slice',
    // The resultant schema is the combination of the union's member's schemas,
    // as we know the source is the same as we know we can get all of the 'seen'
    // columns from the source.
    schema: {
      foo: NUM,
      bar: NUM,
    },
  });
});

test('union type widening', () => {
  const dataset = new UnionDataset([
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM, bar: STR_NULL, baz: BLOB, missing: UNKNOWN},
    }),
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM_NULL, bar: STR, baz: LONG},
    }),
  ]);

  expect(dataset.schema).toEqual({
    foo: NUM_NULL,
    bar: STR_NULL,
    baz: UNKNOWN,
  });
});

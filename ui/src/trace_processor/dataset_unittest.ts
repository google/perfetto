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
import {LONG, NUM, STR} from './query_result';

test('get query for simple dataset', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM},
  });

  expect(dataset.query()).toEqual('select id from (slice)');
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

  expect(dataset.query()).toEqual('select id from (slice) where id = 123');
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
    'select id from (slice) where id in (123,456)',
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
    'select id from (slice) where id = 123 union all select id from (slice) where id = 456',
  );
});

test('doesImplement', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, ts: LONG},
  });

  expect(dataset.implements({id: NUM})).toBe(true);
  expect(dataset.implements({id: NUM, ts: LONG})).toBe(true);
  expect(dataset.implements({id: NUM, ts: LONG, name: STR})).toBe(false);
  expect(dataset.implements({id: LONG})).toBe(false);
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

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

import {UnionDataset, SourceDataset} from './dataset';
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

  expect(dataset.query()).toEqual(`SELECT
  id
FROM (slice)`);
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

  expect(dataset.query()).toEqual(`SELECT
  id
FROM (slice)
WHERE id = 123`);
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

  expect(dataset.query()).toEqual(`SELECT
  id
FROM (slice)
WHERE id IN (123, 456)`);
});

test('get query with column mapping', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, name: STR},
    select: {id: 'id', name: 'slice_name'},
  });

  expect(dataset.query()).toEqual(`SELECT
  id,
  slice_name AS name
FROM (slice)`);
});

test('get query with partial column mapping', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, name: STR, dur: LONG},
    select: {id: 'id', name: 'slice_name', dur: 'dur'},
  });

  // Only 'name' is mapped, 'id' and 'dur' use their original names
  expect(dataset.query()).toEqual(`SELECT
  id,
  slice_name AS name,
  dur
FROM (slice)`);
});

test('get query with column mapping and filter', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, name: STR},
    select: {id: 'slice_id', name: 'slice_name'},
    filter: {
      col: 'id',
      eq: 123,
    },
  });

  expect(dataset.query()).toEqual(`SELECT
  slice_id AS id,
  slice_name AS name
FROM (slice)
WHERE id = 123`);
});

test('get query with single join', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, name: STR, tid: NUM},
    joins: {
      thread: {from: 'thread USING (utid)'},
    },
  });

  expect(dataset.query()).toEqual(`SELECT
  id,
  name,
  tid
FROM (slice) JOIN thread AS thread USING (utid)`);
});

test('get query with multiple joins', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, name: STR, process_name: STR},
    joins: {
      thread: {from: 'thread USING (utid)'},
      process: {from: 'process USING (upid)'},
    },
  });

  expect(dataset.query()).toEqual(`SELECT
  id,
  name,
  process_name
FROM (slice) JOIN thread AS thread USING (utid) JOIN process AS process USING (upid)`);
});

test('get query with joins and filter', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, name: STR},
    joins: {
      thread: {from: 'thread USING (utid)'},
    },
    filter: {
      col: 'id',
      eq: 123,
    },
  });

  expect(dataset.query()).toEqual(`SELECT
  id,
  name
FROM (slice) JOIN thread AS thread USING (utid)
WHERE id = 123`);
});

test('get query with joins, column mapping, and filter', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, thread_name: STR},
    select: {id: 'slice_id', thread_name: 'thread.name'},
    joins: {
      thread: {from: 'thread USING (utid)'},
    },
    filter: {
      col: 'id',
      in: [123, 456],
    },
  });

  expect(dataset.query()).toEqual(`SELECT
  slice_id AS id,
  thread.name AS thread_name
FROM (slice) JOIN thread AS thread USING (utid)
WHERE id IN (123, 456)`);
});

test('get query with select using object format', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, thread_name: STR},
    select: {
      id: 'id',
      thread_name: {expr: 'thread.name', join: 'thread'},
    },
    joins: {
      thread: {from: 'thread USING (utid)'},
    },
  });

  expect(dataset.query()).toEqual(`SELECT
  id,
  thread.name AS thread_name
FROM (slice) JOIN thread AS thread USING (utid)`);
});

test('get query with mixed select formats', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, name: STR, thread_name: STR},
    select: {
      id: 'slice_id',
      name: 'slice.name',
      thread_name: {expr: 'thread.name', join: 'thread'},
    },
    joins: {
      thread: {from: 'thread USING (utid)'},
    },
  });

  expect(dataset.query()).toEqual(`SELECT
  slice_id AS id,
  slice.name AS name,
  thread.name AS thread_name
FROM (slice) JOIN thread AS thread USING (utid)`);
});

test('unique joins not referenced in select are omitted', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, name: STR},
    joins: {
      thread: {from: 'thread USING (utid)', unique: true},
      process: {from: 'process USING (upid)', unique: true},
    },
  });

  // Neither join is referenced, so both should be omitted
  expect(dataset.query()).toEqual(`SELECT
  id,
  name
FROM (slice)`);
});

test('non-unique joins are always included', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, name: STR},
    joins: {
      thread: {from: 'thread USING (utid)', unique: false},
      process: {from: 'process USING (upid)'},
    },
  });

  // Both joins should be included even though not referenced
  expect(dataset.query()).toEqual(`SELECT
  id,
  name
FROM (slice) JOIN thread AS thread USING (utid) JOIN process AS process USING (upid)`);
});

test('unique joins referenced in select are included', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, thread_name: STR},
    select: {
      id: 'id',
      thread_name: {expr: 'thread.name', join: 'thread'},
    },
    joins: {
      thread: {from: 'thread USING (utid)', unique: true},
      process: {from: 'process USING (upid)', unique: true},
    },
  });

  // Only 'thread' join is referenced, so 'process' should be omitted
  expect(dataset.query()).toEqual(`SELECT
  id,
  thread.name AS thread_name
FROM (slice) JOIN thread AS thread USING (utid)`);
});

test('mixed unique and non-unique joins', () => {
  const dataset = new SourceDataset({
    src: 'slice',
    schema: {id: NUM, name: STR},
    joins: {
      thread: {from: 'thread USING (utid)', unique: true},
      process: {from: 'process USING (upid)', unique: false},
    },
  });

  // 'thread' is unique and not referenced, so omitted
  // 'process' is not unique, so included
  expect(dataset.query()).toEqual(`SELECT
  id,
  name
FROM (slice) JOIN process AS process USING (upid)`);
});

test('union query with column elimination', () => {
  const dataset = UnionDataset.create([
    new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, dur: LONG},
      filter: {col: 'id', eq: 123},
    }),
    new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, dur: LONG},
      filter: {col: 'id', eq: 456},
    }),
  ]);

  // When querying with a subset of columns, only those columns are selected
  expect(dataset.query({id: NUM, name: STR})).toEqual(`SELECT
  id,
  name
FROM (slice)
WHERE id IN (123, 456)`);
});

test('union query with join elimination', () => {
  const dataset = UnionDataset.create([
    new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, thread_name: STR},
      select: {
        id: 'id',
        name: 'name',
        thread_name: {expr: 'thread.name', join: 'thread'},
      },
      joins: {
        thread: {from: 'thread USING (utid)', unique: true},
      },
      filter: {col: 'id', eq: 123},
    }),
    new SourceDataset({
      src: 'slice',
      schema: {id: NUM, name: STR, thread_name: STR},
      select: {
        id: 'id',
        name: 'name',
        thread_name: {expr: 'thread.name', join: 'thread'},
      },
      joins: {
        thread: {from: 'thread USING (utid)', unique: true},
      },
      filter: {col: 'id', eq: 456},
    }),
  ]);

  // When querying without thread_name, the unique thread join should be eliminated
  expect(dataset.query({id: NUM, name: STR})).toEqual(`SELECT
  id,
  name
FROM (slice)
WHERE id IN (123, 456)`);

  // When querying with thread_name, the thread join should be included
  expect(dataset.query({id: NUM, name: STR, thread_name: STR})).toEqual(`SELECT
  id,
  name,
  thread.name AS thread_name
FROM (slice) JOIN thread AS thread USING (utid)
WHERE id IN (123, 456)`);
});

test('get query for union dataset', () => {
  const dataset = UnionDataset.create([
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

  // Query automatically optimizes the union into a single source with IN filter
  expect(dataset.query()).toEqual(`SELECT
  id
FROM (slice)
WHERE id IN (123, 456)`);
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

  const query = UnionDataset.create(datasets).query();

  // After optimization, all 800 datasets are merged into a single source with
  // an IN filter, so the query should just be a simple SELECT with WHERE IN.
  expect(query).toContain('SELECT\n  bar\nFROM (foo)\nWHERE some_id IN (');

  // The IN clause should contain all 800 values
  const inMatch = query.match(/IN \(([\d, ]+)\)/);
  expect(inMatch).toBeTruthy();
  if (inMatch) {
    const values = inMatch[1].split(',');
    expect(values.length).toBe(800);
  }
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
  const dataset = UnionDataset.create([
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
  const dataset = UnionDataset.create([
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
  const dataset = UnionDataset.create([
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
  const dataset = UnionDataset.create([
    new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {
        col: 'track_id',
        eq: 123,
      },
    }),
    new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {
        col: 'track_id',
        eq: 456,
      },
    }),
  ]);

  // Optimization happens automatically in query()
  expect(dataset.query()).toEqual(`SELECT
  id
FROM (slice)
WHERE track_id IN (123, 456)`);
});

test('optimize a union dataset with different types of filters', () => {
  const dataset = UnionDataset.create([
    new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {
        col: 'track_id',
        eq: 123,
      },
    }),
    new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {
        col: 'track_id',
        in: [456, 789],
      },
    }),
  ]);

  // Optimization merges all values into a single IN filter
  expect(dataset.query()).toEqual(`SELECT
  id
FROM (slice)
WHERE track_id IN (123, 456, 789)`);
});

test('optimize a union dataset with different schemas', () => {
  const dataset = UnionDataset.create([
    new SourceDataset({
      src: 'slice',
      schema: {foo: NUM},
    }),
    new SourceDataset({
      src: 'slice',
      schema: {bar: NUM},
    }),
  ]);

  // When querying with the union schema (which is empty {}), we get an empty
  // SELECT. But we can query with a specific schema to get columns.
  expect(dataset.query({foo: NUM, bar: NUM})).toEqual(`SELECT
  foo,
  bar
FROM (slice)`);
});

test('union type widening', () => {
  const dataset = UnionDataset.create([
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

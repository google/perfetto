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

import {SourceDataset} from './dataset';
import {LONG, NUM, STR} from './query_result';
import {planQuery} from './dataset_query_utils';

interface TestInput {
  id: string;
  dataset: SourceDataset;
}

test('planQuery groups inputs by source table', () => {
  const inputs: TestInput[] = [
    {
      id: 'slice1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG},
        filter: {col: 'track_id', eq: 1},
      }),
    },
    {
      id: 'slice2',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG},
        filter: {col: 'track_id', eq: 2},
      }),
    },
    {
      id: 'counter1',
      dataset: new SourceDataset({
        src: 'counter',
        schema: {id: NUM, ts: LONG},
        filter: {col: 'track_id', eq: 3},
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM, ts: LONG},
  });

  // We expect the plan to contain two queries: one for 'slice' and one for
  // 'counter'
  expect(plan.queries).toHaveLength(2);

  // Find the slice query
  const sliceQuery = plan.queries.find((q) => q.src === 'slice');
  expect(sliceQuery).toBeDefined();
  expect(sliceQuery!.inputs).toHaveLength(2);
  expect(sliceQuery!.inputs).toContain(inputs[0]);
  expect(sliceQuery!.inputs).toContain(inputs[1]);

  expect(sliceQuery!.partitionColumns).toEqual(['track_id']);
  const partitionMapForTrackId = sliceQuery!.partitionMap.get('track_id')!;
  expect(partitionMapForTrackId).toBeDefined();
  expect(partitionMapForTrackId.size).toBe(2);
  expect(partitionMapForTrackId.get(1)!.has(inputs[0])).toBe(true);
  expect(partitionMapForTrackId.get(2)!.has(inputs[1])).toBe(true);

  expect(sliceQuery!.sql).toBe(
    'SELECT id, ts, track_id FROM (slice) WHERE track_id IN (1, 2)',
  );

  // Find the counter query
  const counterQuery = plan.queries.find((q) => q.src === 'counter');
  expect(counterQuery).toBeDefined();

  expect(counterQuery!.sql).toBe(
    'SELECT id, ts, track_id FROM (counter) WHERE track_id = 3',
  );

  expect(counterQuery!.inputs).toHaveLength(1);
  expect(counterQuery!.inputs).toContain(inputs[2]);

  expect(counterQuery!.partitionColumns).toEqual(['track_id']);
  const counterPartitionMapForTrackId =
    counterQuery!.partitionMap.get('track_id')!;
  expect(counterPartitionMapForTrackId).toBeDefined();
  expect(counterPartitionMapForTrackId.size).toBe(1);
  expect(counterPartitionMapForTrackId.get(3)!.has(inputs[2])).toBe(true);
});

test('planQuery generates optimized SQL with partition filters', () => {
  const inputs: TestInput[] = [
    {
      id: 'input1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG, name: STR},
        filter: {col: 'track_id', eq: 123},
      }),
    },
    {
      id: 'input2',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG, name: STR},
        filter: {col: 'track_id', eq: 456},
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM, ts: LONG},
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Should optimize into a single flattened query with IN clause
  expect(query.sql).toContain('WHERE track_id IN (123, 456)');
  expect(query.sql).toContain('SELECT id, ts, track_id FROM');
  expect(query.partitionColumns).toEqual(['track_id']);
});

test('planQuery handles IN filters', () => {
  const inputs: TestInput[] = [
    {
      id: 'input1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG},
        filter: {col: 'track_id', in: [1, 2, 3]},
      }),
    },
    {
      id: 'input2',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG},
        filter: {col: 'track_id', in: [4, 5]},
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM},
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Should merge all values into single IN clause
  expect(query.sql).toContain('WHERE track_id IN (1, 2, 3, 4, 5)');
  expect(query.partitionColumns).toEqual(['track_id']);
});

test('planQuery applies custom queryBuilder', () => {
  const inputs: TestInput[] = [
    {
      id: 'input1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, name: STR},
        filter: {col: 'track_id', eq: 1},
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM},
    queryBuilder: (baseQuery, resultCols) =>
      `SELECT ${resultCols.join(', ')} FROM (${baseQuery}) WHERE name LIKE '%foo%'`,
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Custom query builder wraps the base query
  expect(query.sql).toContain("WHERE name LIKE '%foo%'");
  expect(query.sql).toContain('SELECT id, track_id FROM');
});

test('planQuery includes filter columns in base query', () => {
  const inputs: TestInput[] = [
    {
      id: 'input1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG, name: STR},
        filter: {col: 'track_id', eq: 1},
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM},
    filterColumns: {name: STR},
    queryBuilder: (baseQuery, resultCols) =>
      `SELECT ${resultCols.join(', ')} FROM (${baseQuery}) WHERE name = 'test'`,
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // With custom query builder, it wraps the base query
  expect(query.sql).toContain('name');
  expect(query.sql).toMatch(/^SELECT id, track_id FROM/);
});

test('planQuery skips partition filters when requested', () => {
  const inputs: TestInput[] = [
    {
      id: 'input1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG},
        filter: {col: 'track_id', eq: 123},
      }),
    },
    {
      id: 'input2',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG},
        filter: {col: 'track_id', eq: 456},
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM, ts: LONG},
    skipPartitionFilters: true,
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Should not have WHERE clause when skipping partition filters
  expect(query.sql).not.toContain('WHERE');
  expect(query.sql).toBe('SELECT id, ts, track_id FROM (slice)');
  expect(query.partitionColumns).toEqual(['track_id']);
});

test('planQuery handles no inputs', () => {
  const plan = planQuery({
    inputs: [],
    datasetFetcher: (input: TestInput) => input.dataset,
    columns: {id: NUM},
  });

  expect(plan.queries).toHaveLength(0);
});

test('planQuery handles inputs without filters', () => {
  const inputs: TestInput[] = [
    {
      id: 'input1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG},
        // No filter
      }),
    },
    {
      id: 'input2',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, ts: LONG},
        // No filter
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM},
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Without filters, no partition columns
  expect(query.partitionColumns).toEqual([]);
  // Should still union the datasets
  expect(query.inputs).toHaveLength(2);
});

test('planQuery with column mapping', () => {
  const inputs: TestInput[] = [
    {
      id: 'input1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, name: STR},
        select: {id: 'slice_id', name: 'slice_name'},
        filter: {col: 'track_id', eq: 1},
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM, name: STR},
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Column mapping should be applied in the base query
  expect(query.sql).toContain('slice_id AS id');
  expect(query.sql).toContain('slice_name AS name');
});

test('planQuery with joins', () => {
  const inputs: TestInput[] = [
    {
      id: 'input1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, thread_name: STR},
        select: {
          id: 'id',
          thread_name: {expr: 'thread.name', join: 'thread'},
        },
        joins: {
          thread: {from: 'thread USING (utid)', unique: true},
        },
        filter: {col: 'track_id', eq: 1},
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM, thread_name: STR},
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Join should be included when thread_name is selected
  expect(query.sql).toContain('JOIN thread AS thread USING (utid)');
});

test('planQuery eliminates unused joins', () => {
  const inputs: TestInput[] = [
    {
      id: 'input1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, thread_name: STR},
        select: {
          id: 'id',
          thread_name: {expr: 'thread.name', join: 'thread'},
        },
        joins: {
          thread: {from: 'thread USING (utid)', unique: true},
        },
        filter: {col: 'track_id', eq: 1},
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM}, // Not selecting thread_name
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Join should be eliminated when thread_name is not selected
  expect(query.sql).not.toContain('JOIN thread');
});

test('planQuery with multiple partition columns', () => {
  const inputs: TestInput[] = [
    {
      id: 'input1',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, track_id: NUM, cpu: NUM},
        filter: {col: 'track_id', eq: 1},
      }),
    },
    {
      id: 'input2',
      dataset: new SourceDataset({
        src: 'slice',
        schema: {id: NUM, track_id: NUM, cpu: NUM},
        filter: {col: 'cpu', eq: 0},
      }),
    },
  ];

  const plan = planQuery({
    inputs,
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM},
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Both partition columns should be included
  expect(query.partitionColumns).toHaveLength(2);
  expect(query.partitionColumns).toContain('track_id');
  expect(query.partitionColumns).toContain('cpu');
});

test('planQuery groups correctly match input identity', () => {
  const input1 = {
    id: 'input1',
    dataset: new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {col: 'track_id', eq: 1},
    }),
  };

  const input2 = {
    id: 'input2',
    dataset: new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {col: 'track_id', eq: 2},
    }),
  };

  const plan = planQuery({
    inputs: [input1, input2],
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM},
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Verify exact input objects are preserved
  expect(query.inputs[0]).toBe(input1);
  expect(query.inputs[1]).toBe(input2);
});

test('planQuery exposes partition map for lineage verification', () => {
  const input1 = {
    id: 'input1',
    dataset: new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {col: 'track_id', eq: 100},
    }),
  };

  const input2 = {
    id: 'input2',
    dataset: new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {col: 'track_id', eq: 200},
    }),
  };

  const input3 = {
    id: 'input3',
    dataset: new SourceDataset({
      src: 'slice',
      schema: {id: NUM},
      filter: {col: 'track_id', in: [100, 300]},
    }),
  };

  const plan = planQuery({
    inputs: [input1, input2, input3],
    datasetFetcher: (input) => input.dataset,
    columns: {id: NUM},
  });

  expect(plan.queries).toHaveLength(1);
  const query = plan.queries[0];

  // Verify partition map structure
  expect(query.partitionMap.size).toBe(1);
  expect(query.partitionMap.has('track_id')).toBe(true);

  const trackIdMap = query.partitionMap.get('track_id')!;
  expect(trackIdMap.size).toBe(3);

  // Verify track_id=100 maps to input1 and input3
  const inputs100 = trackIdMap.get(100);
  expect(inputs100).toBeDefined();
  expect(inputs100!.size).toBe(2);
  expect(inputs100!.has(input1)).toBe(true);
  expect(inputs100!.has(input3)).toBe(true);

  // Verify track_id=200 maps to input2 only
  const inputs200 = trackIdMap.get(200);
  expect(inputs200).toBeDefined();
  expect(inputs200!.size).toBe(1);
  expect(inputs200!.has(input2)).toBe(true);

  // Verify track_id=300 maps to input3 only
  const inputs300 = trackIdMap.get(300);
  expect(inputs300).toBeDefined();
  expect(inputs300!.size).toBe(1);
  expect(inputs300!.has(input3)).toBe(true);
});

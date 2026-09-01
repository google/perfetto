// Copyright (C) 2026 The Android Open Source Project
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

import {AtomicTaskQueue} from '../../../../base/async_memo';
import type {Engine} from '../../../../trace_processor/engine';
import type {PivotModel} from '../data_source';
import type {SQLTableSchema} from '../sql_schema';
import {SQLDataSourceGroupBy} from './group_by';
import {SQLDataSourceRollupTree} from './rollup_tree';

describe('SQLDataSourceGroupBy parameterized aggregates', () => {
  const dummyEngine = {} as Engine;
  const queue = new AtomicTaskQueue();

  const trackSchema: SQLTableSchema = {
    tableOrSubquery: 'track',
    columns: {
      id: {column: 'id'},
      name: {column: 'name'},
      dimension_arg_set_id: {
        parameterized: true,
        expression: (alias, key) =>
          `extract_arg(${alias}.dimension_arg_set_id, ${JSON.stringify(key ?? null)})`,
      },
    },
  };

  const sliceSchema: SQLTableSchema = {
    tableOrSubquery: 'slice',
    columns: {
      id: {column: 'id'},
      name: {column: 'name'},
      dur: {column: 'dur'},
      args: {
        parameterized: true,
        expression: (alias, key) =>
          `extract_arg(${alias}.arg_set_id, ${JSON.stringify(key ?? null)})`,
      },
      track: {
        schema: trackSchema,
        foreignKey: 'track_id',
      },
    },
  };

  test('generates query with parameterized aggregate on base table', () => {
    const ds = new SQLDataSourceGroupBy(queue, dummyEngine, sliceSchema);
    const model: PivotModel = {
      mode: 'pivot',
      groupDisplay: 'flat',
      groupBy: [{field: 'name', alias: 'name_alias'}],
      aggregates: [
        {function: 'SUM', field: 'args.destination', alias: 'dest_sum'},
      ],
    };

    const query = ds.getQuery(model);
    expect(query).toContain(
      'SUM(extract_arg(base.arg_set_id, "destination")) AS "dest_sum"',
    );
    expect(query).toContain('GROUP BY base.name');
  });

  test('generates query with parameterized aggregate on joined relation', () => {
    const ds = new SQLDataSourceGroupBy(queue, dummyEngine, sliceSchema);
    const model: PivotModel = {
      mode: 'pivot',
      groupDisplay: 'flat',
      groupBy: [{field: 'name', alias: 'name_alias'}],
      aggregates: [
        {
          function: 'MAX',
          field: 'track.dimension_arg_set_id.key',
          alias: 'track_dim_max',
        },
      ],
    };

    const query = ds.getQuery(model);
    expect(query).toContain('LEFT JOIN (track) AS');
    expect(query).toContain('MAX(extract_arg(');
    expect(query).toContain('dimension_arg_set_id, "key")) AS "track_dim_max"');
  });
});

describe('SQLDataSourceRollupTree parameterized aggregates', () => {
  const dummyEngine = {} as Engine;
  const queue = new AtomicTaskQueue();

  const trackSchema: SQLTableSchema = {
    tableOrSubquery: 'track',
    columns: {
      id: {column: 'id'},
      name: {column: 'name'},
      dimension_arg_set_id: {
        parameterized: true,
        expression: (alias, key) =>
          `extract_arg(${alias}.dimension_arg_set_id, ${JSON.stringify(key ?? null)})`,
      },
    },
  };

  const sliceSchema: SQLTableSchema = {
    tableOrSubquery: 'slice',
    columns: {
      id: {column: 'id'},
      name: {column: 'name'},
      dur: {column: 'dur'},
      args: {
        parameterized: true,
        expression: (alias, key) =>
          `extract_arg(${alias}.arg_set_id, ${JSON.stringify(key ?? null)})`,
      },
      track: {
        schema: trackSchema,
        foreignKey: 'track_id',
      },
    },
  };

  test('generates rollup table with parameterized aggregate on base table', () => {
    const ds = new SQLDataSourceRollupTree(
      'test_uuid',
      queue,
      dummyEngine,
      sliceSchema,
    );
    const model: PivotModel = {
      mode: 'pivot',
      groupDisplay: 'tree',
      groupBy: [{field: 'name', alias: 'name_alias'}],
      aggregates: [
        {function: 'SUM', field: 'args.destination', alias: 'dest_sum'},
      ],
    };

    const query = ds.getQuery(model);
    expect(query).toContain(
      'SUM(extract_arg(base.arg_set_id, "destination")) AS __agg_0',
    );
    expect(query).toContain('FROM (slice) AS base');
  });

  test('generates rollup table with parameterized aggregate on joined relation', () => {
    const ds = new SQLDataSourceRollupTree(
      'test_uuid',
      queue,
      dummyEngine,
      sliceSchema,
    );
    const model: PivotModel = {
      mode: 'pivot',
      groupDisplay: 'tree',
      groupBy: [{field: 'name', alias: 'name_alias'}],
      aggregates: [
        {
          function: 'MAX',
          field: 'track.dimension_arg_set_id.key',
          alias: 'track_dim_max',
        },
      ],
    };

    const query = ds.getQuery(model);
    expect(query).toContain('LEFT JOIN (track) AS');
    expect(query).toContain('MAX(extract_arg(');
    expect(query).toContain('dimension_arg_set_id, "key")) AS __agg_0');
  });
});

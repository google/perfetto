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

import {describe, expect, test} from 'vitest';
import {type SQLTableSchema, SQLSchemaResolver} from './sql_schema';

describe('SQLSchemaResolver.resolveParameterizedColumn', () => {
  const trackSqlSchema: SQLTableSchema = {
    tableOrSubquery: 'track',
    columns: {
      dimension_arg_set_id: {
        expression: (alias, key) =>
          `extract_arg(${alias}.dimension_arg_set_id, '${key}')`,
        parameterized: true,
        parameterKeysQuery: (tableOrSubquery, alias) =>
          `SELECT DISTINCT args.key FROM (${tableOrSubquery}) AS ${alias}`,
      },
      parent_id: {
        foreignKey: 'parent_id',
        get schema() {
          return trackSqlSchema;
        },
      },
    },
  };

  const rootSchema: SQLTableSchema = {
    tableOrSubquery: 'slice',
    columns: {
      args: {
        expression: (alias, key) =>
          `extract_arg(${alias}.arg_set_id, '${key}')`,
        parameterized: true,
        parameterKeysQuery: (tableOrSubquery, alias) =>
          `SELECT DISTINCT args.key FROM (${tableOrSubquery}) AS ${alias}`,
      },
      track: {
        foreignKey: 'track_id',
        schema: trackSqlSchema,
      },
    },
  };

  test('finds top-level parameterized column', () => {
    const resolver = new SQLSchemaResolver(rootSchema);
    const result = resolver.resolveParameterizedColumn('args');
    expect(result).toBeDefined();
    expect(result?.targetAlias).toBe('base');
    expect(result?.colDef.parameterized).toBe(true);
    expect(resolver.getJoins().length).toBe(0);
  });

  test('finds nested parameterized column and accumulates joins', () => {
    const resolver = new SQLSchemaResolver(rootSchema);
    const result = resolver.resolveParameterizedColumn(
      'track.dimension_arg_set_id',
    );
    expect(result).toBeDefined();
    expect(result?.targetAlias).toBe('t0');
    expect(result?.colDef.parameterized).toBe(true);
    expect(resolver.buildJoinClauses()).toBe(
      'LEFT JOIN (track) AS t0 ON t0.id = base.track_id',
    );
  });

  test('finds recursively joined parameterized column and accumulates joins', () => {
    const resolver = new SQLSchemaResolver(rootSchema);
    const result = resolver.resolveParameterizedColumn(
      'track.parent_id.dimension_arg_set_id',
    );
    expect(result).toBeDefined();
    expect(result?.targetAlias).toBe('t1');
    expect(result?.colDef.parameterized).toBe(true);
    expect(resolver.buildJoinClauses()).toBe(
      'LEFT JOIN (track) AS t0 ON t0.id = base.track_id\nLEFT JOIN (track) AS t1 ON t1.id = t0.parent_id',
    );
  });

  test('returns undefined for non-existent path', () => {
    const resolver = new SQLSchemaResolver(rootSchema);
    expect(resolver.resolveParameterizedColumn('nonexistent')).toBeUndefined();
    expect(
      resolver.resolveParameterizedColumn('track.nonexistent'),
    ).toBeUndefined();
  });

  test('returns undefined for non-parameterized column', () => {
    const schemaWithPlain: SQLTableSchema = {
      tableOrSubquery: 'track',
      columns: {
        id: {},
      },
    };
    const resolver = new SQLSchemaResolver(schemaWithPlain);
    expect(resolver.resolveParameterizedColumn('id')).toBeUndefined();
  });
});

describe('SQLSchemaResolver.resolveColumnPath', () => {
  const trackSqlSchema: SQLTableSchema = {
    tableOrSubquery: 'track',
    columns: {
      dimension_arg_set_id: {
        expression: (alias, key) =>
          `extract_arg(${alias}.dimension_arg_set_id, '${key}')`,
        parameterized: true,
      },
      parent_id: {
        foreignKey: 'parent_id',
        get schema() {
          return trackSqlSchema;
        },
      },
    },
  };

  const rootSchema: SQLTableSchema = {
    tableOrSubquery: 'slice',
    columns: {
      track: {
        foreignKey: 'track_id',
        schema: trackSqlSchema,
      },
    },
  };

  test('resolves nested parameterized column with join', () => {
    const resolver = new SQLSchemaResolver(rootSchema);
    const expr = resolver.resolveColumnPath('track.dimension_arg_set_id.foo');
    expect(expr).toBe("extract_arg(t0.dimension_arg_set_id, 'foo')");
    expect(resolver.buildJoinClauses()).toBe(
      'LEFT JOIN (track) AS t0 ON t0.id = base.track_id',
    );
  });

  test('resolves recursive join parameterized column', () => {
    const resolver = new SQLSchemaResolver(rootSchema);
    const expr = resolver.resolveColumnPath(
      'track.parent_id.dimension_arg_set_id.bar',
    );
    expect(expr).toBe("extract_arg(t1.dimension_arg_set_id, 'bar')");
    expect(resolver.buildJoinClauses()).toBe(
      'LEFT JOIN (track) AS t0 ON t0.id = base.track_id\nLEFT JOIN (track) AS t1 ON t1.id = t0.parent_id',
    );
  });
});

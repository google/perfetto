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

import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {RootNodeData} from './graph_model';
import {flattenNodes} from './graph_utils';
import {serializePsn} from './serialize_psn';
import {serializeSql} from './serialize_sql';

// From(slice) -> Filter -> Extend -> GroupBy -> Filter -> Sort -> Limit
const chain: RootNodeData = {
  type: 'from',
  id: 'src',
  x: 0,
  y: 0,
  config: {table: 'slice'},
  next: {
    type: 'filter',
    id: 'flt1',
    config: {conditions: [{column: 'dur', op: '>', value: '1000'}]},
    next: {
      type: 'extend',
      id: 'ext',
      config: {entries: [{expression: 'dur / 1e6', alias: 'dur_ms'}]},
      next: {
        type: 'groupby',
        id: 'gb',
        config: {
          groupColumns: ['name'],
          aggregations: [
            {func: 'COUNT', column: '*', alias: 'cnt'},
            {func: 'SUM', column: 'dur_ms', alias: 'total_ms'},
          ],
        },
        next: {
          type: 'filter',
          id: 'flt2',
          config: {conditions: [{column: 'cnt', op: '>', value: '5'}]},
          next: {
            type: 'sort',
            id: 'srt',
            config: {conditions: [{column: 'total_ms', order: 'DESC'}]},
            next: {type: 'limit', id: 'lim', config: {limitCount: '10'}},
          },
        },
      },
    },
  },
};

function sqlOf(
  nodes: RootNodeData[],
  outputId: string,
  tables?: ReadonlyMap<string, string>,
): string {
  const res = serializeSql(flattenNodes(nodes), outputId, {tables});
  if (!res.ok) throw new Error(res.error);
  return res.sql;
}

describe('serializeSql', () => {
  it('folds a chain by clause order, starting a CTE when it must', () => {
    expect(sqlOf([chain], 'lim')).toBe(
      [
        'WITH',
        'extend_1 AS (',
        '  SELECT',
        '    *,',
        '    dur / 1e6 AS dur_ms',
        '  FROM slice',
        '  WHERE dur > 1000',
        '),',
        'groupby_1 AS (',
        '  SELECT',
        '    name,',
        '    COUNT(*) AS cnt,',
        '    SUM(dur_ms) AS total_ms',
        '  FROM extend_1',
        '  GROUP BY name',
        ')',
        'SELECT *',
        'FROM groupby_1',
        'WHERE cnt > 5',
        'ORDER BY total_ms DESC',
        'LIMIT 10',
      ].join('\n'),
    );
  });

  it('reads nodes named as tables and renders nothing upstream of them', () => {
    expect(sqlOf([chain], 'lim', new Map([['gb', '_qb_gb']]))).toBe(
      [
        'SELECT *',
        'FROM _qb_gb',
        'WHERE cnt > 5',
        'ORDER BY total_ms DESC',
        'LIMIT 10',
      ].join('\n'),
    );
  });

  it('returns the relation when the output is a bare table', () => {
    const res = serializeSql(flattenNodes([chain]), 'src');
    expect(res).toEqual({
      ok: true,
      sql: 'SELECT *\nFROM slice',
      includes: [],
      relation: 'slice',
    });
  });

  it('writes a node read twice as one CTE', () => {
    const nodes: RootNodeData[] = [
      {
        type: 'from',
        id: 'src',
        x: 0,
        y: 0,
        config: {table: 'slice'},
        next: {
          type: 'filter',
          id: 'flt',
          config: {conditions: [{column: 'dur', op: '>', value: '0'}]},
        },
      },
      {
        type: 'union',
        id: 'uni',
        x: 0,
        y: 0,
        inputs: ['flt', 'flt'],
        config: {numInputs: 2, distinct: false},
      },
    ];
    expect(sqlOf(nodes, 'uni')).toBe(
      [
        'WITH',
        'filter_1 AS (',
        '  SELECT *',
        '  FROM slice',
        '  WHERE dur > 0',
        ')',
        'SELECT *',
        'FROM (',
        '  SELECT * FROM filter_1',
        '  UNION ALL',
        '  SELECT * FROM filter_1',
        ')',
      ].join('\n'),
    );
  });

  it('passes an empty fragment through', () => {
    const nodes: RootNodeData[] = [
      {
        type: 'from',
        id: 'src',
        x: 0,
        y: 0,
        config: {table: 'slice'},
        next: {
          type: 'sort',
          id: 'srt',
          config: {conditions: []},
          next: {type: 'limit', id: 'lim', config: {limitCount: '5'}},
        },
      },
    ];
    expect(sqlOf(nodes, 'lim')).toBe('SELECT *\nFROM slice\nLIMIT 5');
  });

  it('collects includes from the generators', () => {
    const sqlModules = {
      getModuleForTable: (table: string) =>
        table === 'thread_slice'
          ? {includeKey: 'slices.with_context'}
          : undefined,
    } as unknown as SqlModules;
    const res = serializeSql(
      flattenNodes([
        {type: 'from', id: 'f', x: 0, y: 0, config: {table: 'thread_slice'}},
      ]),
      'f',
      {sqlModules},
    );
    expect(res.ok && res.includes).toEqual(['slices.with_context']);
  });

  it('reports the incomplete node', () => {
    const res = serializeSql(
      flattenNodes([
        {
          type: 'from',
          id: 'src',
          x: 0,
          y: 0,
          config: {table: 'slice'},
          next: {type: 'limit', id: 'lim', config: {limitCount: 'abc'}},
        },
      ]),
      'lim',
    );
    expect(res).toEqual({
      ok: false,
      error: 'Limit: incomplete node',
      nodeId: 'lim',
    });
  });

  it('reports unknown node types', () => {
    const res = serializeSql(
      flattenNodes([
        {type: 'nope', id: 'x', x: 0, y: 0, config: {}} as RootNodeData,
      ]),
      'x',
    );
    expect(res.ok).toBe(false);
  });
});

function psnOf(
  nodes: RootNodeData[],
  outputId: string,
  tables?: ReadonlyMap<string, string>,
): string {
  const res = serializePsn(flattenNodes(nodes), outputId, {tables});
  if (!res.ok) throw new Error(res.error);
  return res.sql;
}

describe('serializePsn', () => {
  it('appends each node as a stage', () => {
    expect(psnOf([chain], 'lim')).toBe(
      [
        'FROM slice',
        '|> WHERE dur > 1000',
        '|> EXTEND dur / 1e6 AS dur_ms',
        '|> AGGREGATE COUNT(*) AS cnt, SUM(dur_ms) AS total_ms GROUP BY name',
        '|> WHERE cnt > 5',
        '|> ORDER BY total_ms DESC',
        '|> LIMIT 10',
      ].join('\n'),
    );
  });

  it('reads nodes named as tables', () => {
    expect(psnOf([chain], 'lim', new Map([['gb', '_qb_gb']]))).toBe(
      [
        'FROM _qb_gb',
        '|> WHERE cnt > 5',
        '|> ORDER BY total_ms DESC',
        '|> LIMIT 10',
      ].join('\n'),
    );
  });

  it('returns the relation for a bare table', () => {
    const res = serializePsn(flattenNodes([chain]), 'src');
    expect(res).toEqual({
      ok: true,
      sql: 'FROM slice',
      includes: [],
      relation: 'slice',
    });
  });

  it('repeats a pipeline read twice', () => {
    const nodes: RootNodeData[] = [
      {
        type: 'from',
        id: 'src',
        x: 0,
        y: 0,
        config: {table: 'slice'},
        next: {
          type: 'filter',
          id: 'flt',
          config: {conditions: [{column: 'dur', op: '>', value: '0'}]},
        },
      },
      {
        type: 'union',
        id: 'uni',
        x: 0,
        y: 0,
        inputs: ['flt', 'flt'],
        config: {numInputs: 2, distinct: false},
      },
    ];
    expect(psnOf(nodes, 'uni')).toBe(
      [
        'FROM slice',
        '|> WHERE dur > 0',
        '|> UNION ALL (',
        '  FROM slice',
        '  |> WHERE dur > 0',
        ')',
      ].join('\n'),
    );
  });

  it('parenthesizes a table read by a union, and uses a subquery as is', () => {
    const nodes: RootNodeData[] = [
      {type: 'from', id: 'a', x: 0, y: 0, config: {table: 'slice'}},
      {
        type: 'time_range',
        id: 'tr',
        x: 0,
        y: 0,
        config: {ts: '10', dur: '20'},
      },
      {
        type: 'union',
        id: 'uni',
        x: 0,
        y: 0,
        inputs: ['a', 'b'],
        config: {numInputs: 2, distinct: false},
      },
      {type: 'from', id: 'b', x: 0, y: 0, config: {table: 'sched'}},
      {
        type: 'union',
        id: 'uni2',
        x: 0,
        y: 0,
        inputs: ['a', 'tr'],
        config: {numInputs: 2, distinct: false},
      },
    ];
    expect(psnOf(nodes, 'uni')).toBe('FROM slice\n|> UNION ALL (FROM sched)');
    expect(psnOf(nodes, 'uni2')).toBe(
      'FROM slice\n|> UNION ALL (SELECT 0 AS id, 10 AS ts, 20 AS dur)',
    );
  });

  it('rejects a pipeline as an input to the SQL node', () => {
    const nodes: RootNodeData[] = [
      {
        type: 'from',
        id: 'src',
        x: 0,
        y: 0,
        config: {table: 'slice'},
        next: {
          type: 'filter',
          id: 'flt',
          config: {conditions: [{column: 'dur', op: '>', value: '0'}]},
        },
      },
      {
        type: 'sql',
        id: 'q',
        x: 0,
        y: 0,
        inputs: ['flt'],
        config: {sql: 'SELECT * FROM input_0', columns: [], inputPorts: ['']},
      },
    ];
    const index = flattenNodes(nodes);
    expect(serializePsn(index, 'q').ok).toBe(false);
    // Reading the filter as a table makes it valid.
    expect(
      serializePsn(index, 'q', {tables: new Map([['flt', '_qb_flt']])}).ok,
    ).toBe(true);
  });
});

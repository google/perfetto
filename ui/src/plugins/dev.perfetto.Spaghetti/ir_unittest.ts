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

import {ensureDefined} from '../../base/assert';
import type {NodeData, RootNodeData} from './graph_model';
import {flattenNodes} from './graph_utils';
import {
  buildDisplayPipeSql,
  buildDisplaySql,
  buildIR,
  buildLogicalPlan,
  type IrEntry,
} from './ir';

// Helper to build a graph index from root nodes.
function makeNodes(nodeList: RootNodeData[]) {
  return flattenNodes(nodeList);
}

describe('buildLogicalPlan', () => {
  it('emits language-agnostic ops and computes refCount on diamond DAGs', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        conditions: [{column: 'dur', op: '>', value: '0'}],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: filter,
      config: {table: 'slice'},
    };
    const union: RootNodeData = {
      type: 'union',
      id: 'u1',
      x: 100,
      y: 0,
      inputs: ['flt1', 'flt1'],
      config: {distinct: false, numInputs: 2},
    };

    const plan = ensureDefined(
      buildLogicalPlan(makeNodes([from, union]), 'u1', undefined),
    );
    expect(plan.targetNodeId).toBe('u1');
    expect(plan.nodes.map((n) => [n.nodeId, n.op.kind, n.refCount])).toEqual([
      ['f1', 'scan', 1],
      ['flt1', 'filter', 2],
      ['u1', 'union', 0],
    ]);
  });

  it('returns undefined when required upstream input is unconnected', () => {
    const filter: RootNodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      config: {
        conditions: [{column: 'dur', op: '>', value: '100'}],
      },
    };
    expect(
      buildLogicalPlan(makeNodes([filter]), 'flt1', undefined),
    ).toBeUndefined();
  });
});

describe('buildIR (Standard SQL backend)', () => {
  it('returns undefined for nonexistent node', () => {
    const nodes = makeNodes([]);
    expect(buildIR(nodes, 'missing', undefined)).toBeUndefined();
  });

  it('produces one entry for a single from node', () => {
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const nodes = makeNodes([from]);
    const entries = ensureDefined(buildIR(nodes, 'f1', undefined));
    expect(entries.length).toBe(1);
    const e0 = ensureDefined(entries[0]);
    expect(e0.sql).toBe('SELECT *\nFROM slice');
    expect(e0.deps.length).toBe(0);
  });

  it('creates one IR entry for from -> filter chain', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: filter,
      config: {table: 'slice'},
    };
    const nodes = makeNodes([from]);
    const entries = ensureDefined(buildIR(nodes, 'flt1', undefined));
    expect(entries.length).toBe(1);
    const e0 = ensureDefined(entries[0]);
    expect(e0.sql).toContain('WHERE dur > 1000');
    expect(e0.sql).toContain('FROM slice');
    expect(e0.deps.length).toBe(0); // from node = no dep
    expect(e0.hash).toMatch(/^_qb_[0-9a-f]{8}$/);
  });

  it('folds select into the same statement as filter when possible', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        conditions: [{column: 'dur', op: '>', value: '100'}],
      },
    };
    const select: NodeData = {
      type: 'select',
      id: 's1',
      next: filter,
      config: {
        entries: [
          {column: 'name', alias: ''},
          {column: 'dur', alias: ''},
        ],
        expressions: [],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: select,
      config: {table: 'slice'},
    };
    const nodes = makeNodes([from]);
    const entries = ensureDefined(buildIR(nodes, 'flt1', undefined));
    // select + filter should fold into one entry
    expect(entries.length).toBe(1);
    const e0 = ensureDefined(entries[0]);
    expect(e0.sql).toContain('name,\n  dur');
    expect(e0.sql).toContain('WHERE dur > 100');
  });

  it('creates separate entries when folding is not possible', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        conditions: [{column: 'cnt', op: '>', value: '5'}],
      },
    };
    const groupby: NodeData = {
      type: 'groupby',
      id: 'gb1',
      next: filter,
      config: {
        groupColumns: ['name'],
        aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: groupby,
      config: {table: 'slice'},
    };
    const nodes = makeNodes([from]);
    const entries = ensureDefined(buildIR(nodes, 'flt1', undefined));
    expect(entries.length).toBe(2);
    const e0 = ensureDefined(entries[0]);
    const e1 = ensureDefined(entries[1]);
    // First entry is the groupby
    expect(e0.sql).toContain('GROUP BY name');
    // Second entry is filter, depends on groupby hash
    expect(e1.deps).toContain(e0.hash);
    expect(e1.sql).toContain('WHERE cnt > 5');
  });

  it('lowers interval_intersect with stdlib module include', () => {
    const left: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const right: RootNodeData = {
      type: 'from',
      id: 'f2',
      x: 100,
      y: 0,
      config: {table: 'sched'},
    };
    const ii: RootNodeData = {
      type: 'interval_intersect',
      id: 'ii1',
      x: 200,
      y: 0,
      inputs: ['f1', 'f2'],
      config: {
        numInputs: 2,
        partitionColumns: ['utid'],
        filterNegativeDur: true,
      },
    };

    const entries = ensureDefined(
      buildIR(makeNodes([left, right, ii]), 'ii1', undefined),
    );
    expect(entries.length).toBe(1);
    const e0 = ensureDefined(entries[0]);
    expect(e0.includes).toContain('intervals.intersect');
    expect(e0.sql).toContain('_interval_intersect!');
    expect(e0.sql).toContain('(utid)');
  });

  it('flushes foldable right-input branch before standalone join and downstream ops', () => {
    const sliceRoot: RootNodeData = {
      id: 'c7aEUHyXD',
      type: 'from',
      x: 538,
      y: 144,
      config: {table: 'slice'},
      next: {
        id: 'cz2hanKDC',
        type: 'join',
        inputs: [null, 'cZayefyv4'],
        config: {
          leftColumn: 'arg_set_id',
          rightColumn: 'arg_set_id',
          columns: [
            {column: 'key', alias: ''},
            {column: 'display_value', alias: 'arg'},
          ],
          joinType: 'LEFT',
        },
        next: {
          id: 'c1J5Lwm1W',
          type: 'filter',
          config: {
            conditions: [
              {column: 'name', op: '=', value: 'hipStreamSynchronize'},
            ],
            conjunction: 'AND',
          },
          next: {
            id: 'cEL1iGXLx',
            type: 'sort',
            config: {
              conditions: [{column: 'dur', order: 'DESC'}],
            },
          },
        },
      },
    };
    const argsRoot: RootNodeData = {
      id: 'c_FkMas6e',
      type: 'from',
      x: 130,
      y: 141,
      config: {table: 'args'},
      next: {
        id: 'cZayefyv4',
        type: 'filter',
        config: {
          conditions: [{column: 'key', op: '=', value: 'hipStreamSynchronize'}],
          conjunction: 'AND',
        },
      },
    };

    const index = makeNodes([sliceRoot, argsRoot]);

    const joinEntries = ensureDefined(buildIR(index, 'cz2hanKDC', undefined));
    expect(joinEntries.length).toBe(2);
    const rightEntry = ensureDefined(joinEntries[0]);
    const joinEntry = ensureDefined(joinEntries[1]);
    expect(rightEntry.sql).toContain('FROM args');
    expect(rightEntry.sql).toContain("WHERE key = 'hipStreamSynchronize'");
    expect(joinEntry.deps).toEqual([rightEntry.hash]);
    expect(joinEntry.sql).toContain(`LEFT JOIN ${rightEntry.hash} AS r`);
    expect(joinEntry.sql).toContain('r.display_value AS arg');

    const sortEntries = ensureDefined(buildIR(index, 'cEL1iGXLx', undefined));
    expect(sortEntries.length).toBe(3);
    const tailEntry = ensureDefined(sortEntries[2]);
    expect(tailEntry.deps).toEqual([joinEntry.hash]);
    expect(tailEntry.sql).toContain("WHERE name = 'hipStreamSynchronize'");
    expect(tailEntry.sql).toContain('ORDER BY dur DESC');
  });

  it('computes stable hashes based on content and deps', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: filter,
      config: {table: 'slice'},
    };
    const nodes = makeNodes([from]);

    const e1 = ensureDefined(buildIR(nodes, 'flt1', undefined));
    const e2 = ensureDefined(buildIR(nodes, 'flt1', undefined));
    expect(ensureDefined(e1[0]).hash).toBe(ensureDefined(e2[0]).hash);
  });

  it('hash changes when filter value changes', () => {
    const filter1: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const filter2: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        conditions: [{column: 'dur', op: '>', value: '2000'}],
      },
    };

    const nodes1 = makeNodes([
      {
        type: 'from',
        id: 'f1',
        x: 0,
        y: 0,
        next: filter1,
        config: {table: 'slice'},
      },
    ]);
    const nodes2 = makeNodes([
      {
        type: 'from',
        id: 'f1',
        x: 0,
        y: 0,
        next: filter2,
        config: {table: 'slice'},
      },
    ]);

    const e1 = ensureDefined(buildIR(nodes1, 'flt1', undefined));
    const e2 = ensureDefined(buildIR(nodes2, 'flt1', undefined));
    expect(ensureDefined(e1[0]).hash).not.toBe(ensureDefined(e2[0]).hash);
  });

  it('hash changes when upstream from table changes', () => {
    const makeGraph = (table: string) => {
      const filter: NodeData = {
        type: 'filter',
        id: 'flt1',
        config: {
          conditions: [{column: 'dur', op: '>', value: '1000'}],
        },
      };
      return makeNodes([
        {type: 'from', id: 'f1', x: 0, y: 0, next: filter, config: {table}},
      ]);
    };

    const g1 = makeGraph('slice');
    const g2 = makeGraph('sched');
    const e1 = ensureDefined(buildIR(g1, 'flt1', undefined));
    const e2 = ensureDefined(buildIR(g2, 'flt1', undefined));

    // The SQL differs (FROM slice vs FROM sched), so hashes differ.
    expect(ensureDefined(e1[0]).hash).not.toBe(ensureDefined(e2[0]).hash);
  });

  it('returns undefined when a node is invalid', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {conditions: []},
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: filter,
      config: {table: ''},
    };
    const nodes = makeNodes([from]);
    expect(buildIR(nodes, 'flt1', undefined)).toBeUndefined();
  });
});

describe('buildDisplaySql', () => {
  function getEntries(nodeList: RootNodeData[], nodeId: string): IrEntry[] {
    const nodes = makeNodes(nodeList);
    const entries = buildIR(nodes, nodeId, undefined);
    if (!entries) throw new Error('buildIR returned undefined');
    return entries;
  }

  it('returns undefined for empty entries', () => {
    expect(buildDisplaySql([])).toBeUndefined();
  });

  it('returns simple query for from -> filter', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: filter,
      config: {table: 'slice'},
    };
    const entries = getEntries([from], 'flt1');
    const sql = ensureDefined(buildDisplaySql(entries));
    expect(sql).toContain('SELECT *');
    expect(sql).toContain('FROM slice');
    expect(sql).toContain('WHERE dur > 1000');
    // No WITH clause for a single entry
    expect(sql).not.toContain('WITH');
  });

  it('generates WITH clause with readable step names for multi-entry chains', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        conditions: [{column: 'cnt', op: '>', value: '5'}],
      },
    };
    const groupby: NodeData = {
      type: 'groupby',
      id: 'gb1',
      next: filter,
      config: {
        groupColumns: ['name'],
        aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: groupby,
      config: {table: 'slice'},
    };
    const entries = getEntries([from], 'flt1');
    const sql = ensureDefined(buildDisplaySql(entries));
    expect(sql).toContain('WITH');
    expect(sql).toContain('step_1 AS (');
    expect(sql).toContain('FROM step_1');
    expect(sql).toContain('GROUP BY name');
    expect(sql).toContain('WHERE cnt > 5');
  });

  it('shows correct SQL when clicking intermediate node', () => {
    const sort: NodeData = {
      type: 'sort',
      id: 'srt1',
      config: {sortColumn: 'dur', sortOrder: 'DESC'},
    };
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      next: sort,
      config: {
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: filter,
      config: {table: 'slice'},
    };

    // Clicking on filter: build IR from filter upstream only
    const filterEntries = getEntries([from], 'flt1');
    const filterSql = ensureDefined(buildDisplaySql(filterEntries));
    expect(filterSql).toContain('WHERE dur > 1000');
    expect(filterSql).not.toContain('ORDER BY');

    // Clicking on sort: build IR from sort upstream (includes filter)
    const sortEntries = getEntries([from], 'srt1');
    const sortSql = ensureDefined(buildDisplaySql(sortEntries));
    expect(sortSql).toContain('WHERE dur > 1000');
    expect(sortSql).toContain('ORDER BY dur DESC');
  });
});

describe('PerfettoSQL Next (pipe) backend', () => {
  it('composes groupby -> filter -> drop -> sort -> limit into a single pipeline', () => {
    const limit: NodeData = {
      type: 'limit',
      id: 'lim1',
      config: {limitCount: '25'},
    };
    const sort: NodeData = {
      type: 'sort',
      id: 'srt1',
      next: limit,
      config: {
        sortColumn: 'name',
        sortOrder: 'ASC',
        conditions: [{column: 'name', order: 'ASC'}],
      },
    };
    const drop: NodeData = {
      type: 'drop',
      id: 'drp1',
      next: sort,
      config: {columns: ['cnt']},
    };
    const havingFilter: NodeData = {
      type: 'filter',
      id: 'flt2',
      next: drop,
      config: {
        conditions: [{column: 'cnt', op: '>', value: '5'}],
      },
    };
    const groupby: NodeData = {
      type: 'groupby',
      id: 'gb1',
      next: havingFilter,
      config: {
        groupColumns: ['name'],
        aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
      },
    };
    const preFilter: NodeData = {
      type: 'filter',
      id: 'flt1',
      next: groupby,
      config: {
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: preFilter,
      config: {table: 'slice'},
    };

    const index = makeNodes([from]);
    const pipeEntries = ensureDefined(
      buildIR(index, 'lim1', undefined, 'pipe'),
    );
    // Unlike Standard SQL (which needs multiple CTEs), PerfettoSQL Next folds
    // the entire linear pipeline into a single entry!
    expect(pipeEntries.length).toBe(1);

    const display = ensureDefined(buildDisplayPipeSql(pipeEntries));
    expect(display).toBe(
      [
        'FROM slice',
        '|> WHERE dur > 1000',
        '|> AGGREGATE COUNT(*) AS cnt GROUP BY name',
        '|> WHERE cnt > 5',
        '|> DROP cnt',
        '|> ORDER BY name ASC',
        '|> LIMIT 25',
      ].join('\n'),
    );
  });

  it('lowers interval_intersect to native INTERVAL INTERSECTION OF without stdlib include', () => {
    const left: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const right: RootNodeData = {
      type: 'from',
      id: 'f2',
      x: 100,
      y: 0,
      config: {table: 'sched'},
    };
    const ii: RootNodeData = {
      type: 'interval_intersect',
      id: 'ii1',
      x: 200,
      y: 0,
      inputs: ['f1', 'f2'],
      config: {
        numInputs: 2,
        partitionColumns: ['utid'],
        filterNegativeDur: true,
      },
    };

    const entries = ensureDefined(
      buildIR(makeNodes([left, right, ii]), 'ii1', undefined, 'pipe'),
    );
    expect(entries.length).toBe(1);
    const e0 = ensureDefined(entries[0]);
    expect(e0.includes).toEqual([]);
    expect(e0.sql).toContain('INTERVAL INTERSECTION OF (');
    expect(e0.sql).toContain('(FROM slice |> WHERE dur >= 0) AS t1');
    expect(e0.sql).toContain('(FROM sched |> WHERE dur >= 0) AS t2');
    expect(e0.sql).toContain(') PER utid');
    expect(e0.sql).toContain('|> SELECT ts, dur, t1.id AS id_0, t2.id AS id_1');
  });

  it('formats multi-entry diamond DAGs using CREATE OR REPLACE PERFETTO TABLE', () => {
    const sharedFilter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        conditions: [{column: 'dur', op: '>', value: '0'}],
      },
    };
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      next: sharedFilter,
      config: {table: 'slice'},
    };
    const ii: RootNodeData = {
      type: 'interval_intersect',
      id: 'ii1',
      x: 200,
      y: 0,
      inputs: ['flt1', 'flt1'],
      config: {
        numInputs: 2,
        partitionColumns: [],
        filterNegativeDur: false,
      },
    };

    const entries = ensureDefined(
      buildIR(makeNodes([from, ii]), 'ii1', undefined, 'pipe'),
    );
    expect(entries.length).toBe(2);
    const display = ensureDefined(buildDisplayPipeSql(entries));
    expect(display).toContain(
      'CREATE OR REPLACE PERFETTO TABLE step_1 AS\nFROM slice\n|> WHERE dur > 0;',
    );
    expect(display).toContain('step_1 AS t1');
    expect(display).toContain('step_1 AS t2');
  });

  it('lowers distinct, rename, set, and limit offset in both dialects', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'select',
        id: 'sel1',
        config: {
          entries: [
            {column: 'id', alias: ''},
            {column: 'dur', alias: ''},
          ],
          expressions: [],
        },
        next: {
          type: 'set',
          id: 'set1',
          config: {
            entries: [{column: 'dur', expression: 'dur / 1000'}],
          },
          next: {
            type: 'rename',
            id: 'ren1',
            config: {
              entries: [{column: 'dur', alias: 'dur_us'}],
            },
            next: {
              type: 'distinct',
              id: 'dist1',
              config: {},
              next: {
                type: 'limit',
                id: 'lim1',
                config: {limitCount: '10', offset: '5'},
              },
            },
          },
        },
      },
    };

    const index = makeNodes([root]);

    const pipeEntries = ensureDefined(
      buildIR(index, 'lim1', undefined, 'pipe'),
    );
    expect(pipeEntries.length).toBe(1);
    expect(buildDisplayPipeSql(pipeEntries)).toBe(
      [
        'FROM slice',
        '|> SELECT id, dur',
        '|> SET dur = dur / 1000',
        '|> RENAME dur AS dur_us',
        '|> DISTINCT',
        '|> LIMIT 10 OFFSET 5',
      ].join('\n'),
    );

    const sqlEntries = ensureDefined(buildIR(index, 'lim1', undefined, 'sql'));
    const displaySql = ensureDefined(buildDisplaySql(sqlEntries));
    expect(displaySql).toContain('dur / 1000 AS dur');
    expect(displaySql).toContain('dur AS dur_us');
    expect(displaySql).toContain('SELECT DISTINCT *');
    expect(displaySql).toContain('LIMIT 10 OFFSET 5');
  });

  it('lowers interval_flatten in both dialects', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'interval_flatten',
        id: 'flat1',
        config: {
          partitionColumns: ['track_id'],
          aggregations: [
            {func: 'COUNT', column: '*', alias: 'depth_cnt'},
            {func: 'SUM', column: 'dur', alias: 'total_dur'},
          ],
          filterNegativeDur: true,
        },
      },
    };

    const index = makeNodes([root]);

    const pipeEntries = ensureDefined(
      buildIR(index, 'flat1', undefined, 'pipe'),
    );
    expect(pipeEntries.length).toBe(1);
    expect(buildDisplayPipeSql(pipeEntries)).toBe(
      [
        'FROM slice',
        '|> WHERE dur >= 0',
        '|> INTERVAL FLATTEN PER track_id AGGREGATE COUNT(*) AS depth_cnt, SUM(dur) AS total_dur',
      ].join('\n'),
    );

    const sqlEntries = ensureDefined(buildIR(index, 'flat1', undefined, 'sql'));
    expect(sqlEntries.length).toBe(1);
    const sqlText = ensureDefined(sqlEntries[0]).sql;
    expect(sqlText).toContain('PARTITION BY track_id');
    expect(sqlText).toContain('_active AS depth_cnt');
    expect(sqlText).toContain('AS total_dur');
  });

  it('lowers interval_fill_gaps and narrows extra background columns in pipe mode', () => {
    const bg: RootNodeData = {
      type: 'time_range',
      id: 'tr1',
      x: 0,
      y: 0,
      config: {ts: '0', dur: '1000'},
    };
    const fg: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 200,
      y: 0,
      config: {table: 'sched'},
      next: {
        type: 'select',
        id: 'sel1',
        config: {
          entries: [
            {column: 'ts', alias: ''},
            {column: 'dur', alias: ''},
            {column: 'cpu', alias: ''},
          ],
          expressions: [],
        },
        next: {
          type: 'interval_fill_gaps',
          id: 'fg1',
          inputs: [null, 'tr1'],
          config: {partitionColumns: ['cpu']},
        },
      },
    };

    const index = makeNodes([bg, fg]);

    const pipeEntries = ensureDefined(buildIR(index, 'fg1', undefined, 'pipe'));
    const pipeDisplay = ensureDefined(buildDisplayPipeSql(pipeEntries));
    // `time_range` exposes `(id, ts, dur)` while `sel1` only has `(ts, dur, cpu)`,
    // so the pipe backend narrows the background to `(FROM step_2 |> SELECT ts, dur)`.
    expect(pipeDisplay).toContain(
      '|> INTERVAL FILL GAPS WITH (FROM step_2 |> SELECT ts, dur) PER cpu',
    );

    const sqlEntries = ensureDefined(buildIR(index, 'fg1', undefined, 'sql'));
    const sqlDisplay = ensureDefined(buildDisplaySql(sqlEntries));
    expect(sqlDisplay).toContain('_islands AS (');
    expect(sqlDisplay).toContain('_gaps AS (');
    expect(sqlDisplay).toContain('b.cpu AS cpu');
  });

  it('lowers graph_tree + tree_accumulate pipeline in both dialects', () => {
    const nodesRoot: RootNodeData = {
      type: 'from',
      id: 'n1',
      x: 0,
      y: 0,
      config: {table: 'heap_graph_object'},
      next: {
        type: 'graph_tree',
        id: 'gt1',
        inputs: [null, 'e1', 'r1'],
        config: {
          kind: 'DOMINATOR',
          nodeIdColumn: 'id',
          sourceColumn: 'owner_id',
          destColumn: 'owned_id',
          rootIdColumn: 'id',
        },
        next: {
          type: 'tree_accumulate',
          id: 'ta1',
          config: {
            direction: 'UP',
            idColumn: 'id',
            parentIdColumn: 'parent_id',
            aggregations: [
              {func: 'SUM', column: 'self_size', alias: 'retained_size'},
            ],
          },
        },
      },
    };
    const edgesRoot: RootNodeData = {
      type: 'from',
      id: 'e1',
      x: 200,
      y: 0,
      config: {table: 'heap_graph_reference'},
    };
    const rootsRoot: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 400,
      y: 0,
      config: {table: 'heap_roots'},
    };

    const index = makeNodes([nodesRoot, edgesRoot, rootsRoot]);

    const pipeEntries = ensureDefined(buildIR(index, 'ta1', undefined, 'pipe'));
    expect(pipeEntries.length).toBe(1);
    expect(buildDisplayPipeSql(pipeEntries)).toBe(
      [
        'GRAPH DOMINATOR TREE',
        '  NODES heap_graph_object',
        '  EDGES (FROM heap_graph_reference |> SELECT owner_id AS src, owned_id AS dst)',
        '  FROM heap_roots',
        '|> TREE ACCUMULATE UP SUM(self_size) AS retained_size',
      ].join('\n'),
    );

    const sqlEntries = ensureDefined(buildIR(index, 'ta1', undefined, 'sql'));
    expect(sqlEntries.length).toBe(2);
    const sqlDisplay = ensureDefined(buildDisplaySql(sqlEntries));
    expect(sqlDisplay).toContain('INCLUDE PERFETTO MODULE graphs.search;');
    expect(sqlDisplay).toContain(
      'INCLUDE PERFETTO MODULE graphs.dominator_tree;',
    );
    expect(sqlDisplay).toContain('graph_dominator_tree!');
    expect(sqlDisplay).toContain('WITH RECURSIVE');
    expect(sqlDisplay).toContain('SUM(v.self_size) AS retained_size');
  });
});

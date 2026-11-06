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
  buildDisplayQuery,
  buildLogicalPlan,
  formatLogicalPlan,
  formatPipe,
  formatSql,
  type LogicalPlan,
  type SqlDialect,
} from './ir';
import {planMaterialization} from './materialization';

function nodeHash(plan: LogicalPlan, id: string): string {
  return ensureDefined(plan.nodes.find((n) => n.nodeId === id)).hash;
}

// Helper to build a graph index from root nodes.
function makeNodes(nodeList: RootNodeData[]) {
  return flattenNodes(nodeList);
}

function getDisplay(
  nodeList: RootNodeData[],
  nodeId: string,
  dialect: SqlDialect = 'sql',
): string {
  const plan = ensureDefined(
    buildLogicalPlan(makeNodes(nodeList), nodeId, undefined),
  );
  return ensureDefined(buildDisplayQuery(plan, dialect));
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

  it('returns undefined for nonexistent or invalid upstream nodes', () => {
    const empty = makeNodes([]);
    expect(buildLogicalPlan(empty, 'missing', undefined)).toBeUndefined();

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
    expect(
      buildLogicalPlan(makeNodes([from]), 'flt1', undefined),
    ).toBeUndefined();
  });
});

describe('formatSql (Standard SQL backend)', () => {
  it('returns undefined display query for undefined plan', () => {
    expect(buildDisplayQuery(undefined, 'sql')).toBeUndefined();
  });

  it('formats a single from node directly', () => {
    const from: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    expect(getDisplay([from], 'f1', 'sql')).toBe('SELECT *\nFROM slice');
  });

  it('folds from -> select -> filter into one SELECT without WITH clause', () => {
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
    const sql = getDisplay([from], 'flt1', 'sql');
    expect(sql).toContain('name,\n  dur');
    expect(sql).toContain('FROM slice');
    expect(sql).toContain('WHERE dur > 100');
    expect(sql).not.toContain('WITH');
  });

  it('generates WITH clause with readable step names when folding is not possible', () => {
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
    const sql = getDisplay([from], 'flt1', 'sql');
    expect(sql).toContain('WITH');
    expect(sql).toContain('step_1 AS (');
    expect(sql).toContain('FROM step_1');
    expect(sql).toContain('GROUP BY name');
    expect(sql).toContain('WHERE cnt > 5');
  });

  it('merges upstream CTEs cleanly when final stage itself starts with WITH', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'filter',
        id: 'flt1',
        config: {
          conditions: [{column: 'dur', op: '>', value: '100'}],
        },
        next: {
          type: 'interval_flatten',
          id: 'flat1',
          config: {
            partitionColumns: ['track_id'],
            aggregations: [{func: 'COUNT', column: '*', alias: 'depth_cnt'}],
            filterNegativeDur: true,
          },
        },
      },
    };
    const sql = getDisplay([root], 'flat1', 'sql');
    expect(sql.match(/\bWITH\b/g)?.length).toBe(1);
    expect(sql).toContain('step_1 AS (');
    expect(sql).toContain('_src AS (');
    expect(sql).toContain('FROM step_1');
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

    const plan = ensureDefined(
      buildLogicalPlan(makeNodes([left, right, ii]), 'ii1', undefined),
    );
    const res = formatSql(plan, {includeModuleHeaders: true});
    expect(res.ok).toBe(true);
    if (!res.ok) return;
    expect(res.includes).toContain('intervals.intersect');
    expect(res.sql).toContain('INCLUDE PERFETTO MODULE intervals.intersect;');
    expect(res.sql).toContain('_interval_intersect!');
    expect(res.sql).toContain('(utid)');
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

    const joinSql = getDisplay([sliceRoot, argsRoot], 'cz2hanKDC', 'sql');
    expect(joinSql).toContain('step_1 AS (');
    expect(joinSql).toContain('FROM args');
    expect(joinSql).toContain("WHERE key = 'hipStreamSynchronize'");
    expect(joinSql).toContain('LEFT JOIN step_1 AS r');
    expect(joinSql).toContain('r.display_value AS arg');

    const sortSql = getDisplay([sliceRoot, argsRoot], 'cEL1iGXLx', 'sql');
    expect(sortSql).toContain('step_2 AS (');
    expect(sortSql).toContain('FROM step_2');
    expect(sortSql).toContain("WHERE name = 'hipStreamSynchronize'");
    expect(sortSql).toContain('ORDER BY dur DESC');
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

    const filterSql = getDisplay([from], 'flt1', 'sql');
    expect(filterSql).toContain('WHERE dur > 1000');
    expect(filterSql).not.toContain('ORDER BY');

    const sortSql = getDisplay([from], 'srt1', 'sql');
    expect(sortSql).toContain('WHERE dur > 1000');
    expect(sortSql).toContain('ORDER BY dur DESC');
  });
});

describe('formatPipe (PerfettoSQL Next backend)', () => {
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

    const display = getDisplay([from], 'lim1', 'pipe');
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

    const plan = ensureDefined(
      buildLogicalPlan(makeNodes([left, right, ii]), 'ii1', undefined),
    );
    const res = formatPipe(plan);
    expect(res.ok).toBe(true);
    if (!res.ok) return;
    expect(res.includes).toEqual([]);
    expect(res.sql).toContain('INTERVAL INTERSECTION OF (');
    expect(res.sql).toContain('(FROM slice |> WHERE dur >= 0) AS t1');
    expect(res.sql).toContain('(FROM sched |> WHERE dur >= 0) AS t2');
    expect(res.sql).toContain(') PER utid');
    expect(res.sql).toContain(
      '|> SELECT ts, dur, t1.id AS id_0, t2.id AS id_1',
    );
  });

  it('formats multi-use diamond DAGs in pipe mode as a single query by repeating the shared step', () => {
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

    const display = getDisplay([from, ii], 'ii1', 'pipe');
    expect(display).not.toContain('CREATE OR REPLACE PERFETTO TABLE');
    expect(display).toBe(
      [
        'INTERVAL INTERSECTION OF (',
        '  (',
        '    FROM slice',
        '    |> WHERE dur > 0',
        '  ) AS t1,',
        '  (',
        '    FROM slice',
        '    |> WHERE dur > 0',
        '  ) AS t2',
        ')',
        '|> SELECT ts, dur, t1.id AS id_0, t2.id AS id_1',
      ].join('\n'),
    );
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

    expect(getDisplay([root], 'lim1', 'pipe')).toBe(
      [
        'FROM slice',
        '|> SELECT id, dur',
        '|> SET dur = dur / 1000',
        '|> RENAME dur AS dur_us',
        '|> DISTINCT',
        '|> LIMIT 10 OFFSET 5',
      ].join('\n'),
    );

    const displaySql = getDisplay([root], 'lim1', 'sql');
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

    expect(getDisplay([root], 'flat1', 'pipe')).toBe(
      [
        'FROM slice',
        '|> WHERE dur >= 0',
        '|> INTERVAL FLATTEN PER track_id AGGREGATE COUNT(*) AS depth_cnt, SUM(dur) AS total_dur',
      ].join('\n'),
    );

    const sqlText = getDisplay([root], 'flat1', 'sql');
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

    expect(getDisplay([bg, fg], 'fg1', 'pipe')).toBe(
      [
        'FROM sched',
        '|> SELECT ts, dur, cpu',
        '|> INTERVAL FILL GAPS WITH (FROM (SELECT 0 AS id, 0 AS ts, 1000 AS dur) |> SELECT ts, dur) PER cpu',
      ].join('\n'),
    );

    const sqlDisplay = getDisplay([bg, fg], 'fg1', 'sql');
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

    expect(getDisplay([nodesRoot, edgesRoot, rootsRoot], 'ta1', 'pipe')).toBe(
      [
        'GRAPH DOMINATOR TREE',
        '  NODES heap_graph_object',
        '  EDGES (FROM heap_graph_reference |> SELECT owner_id AS src, owned_id AS dst)',
        '  FROM heap_roots',
        '|> TREE ACCUMULATE UP SUM(self_size) AS retained_size',
      ].join('\n'),
    );

    const sqlDisplay = getDisplay(
      [nodesRoot, edgesRoot, rootsRoot],
      'ta1',
      'sql',
    );
    expect(sqlDisplay).toContain('INCLUDE PERFETTO MODULE graphs.search;');
    expect(sqlDisplay).toContain(
      'INCLUDE PERFETTO MODULE graphs.dominator_tree;',
    );
    expect(sqlDisplay).toContain('graph_dominator_tree!');
    expect(sqlDisplay).toContain('WITH RECURSIVE');
    expect(sqlDisplay.match(/^WITH\b/gm)?.length).toBe(1);
    expect(sqlDisplay).toContain('SUM(v.self_size) AS retained_size');
  });

  it('lowers as stage in both dialects', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'as',
        id: 'as1',
        config: {alias: 's'},
        next: {
          type: 'extend',
          id: 'ext1',
          config: {
            entries: [{expression: 's.ts + s.dur', alias: 'ts_end'}],
          },
        },
      },
    };

    expect(getDisplay([root], 'ext1', 'pipe')).toBe(
      ['FROM slice', '|> AS s', '|> EXTEND s.ts + s.dur AS ts_end'].join('\n'),
    );

    const sqlDisplay = getDisplay([root], 'ext1', 'sql');
    expect(sqlDisplay).toContain('s.ts + s.dur AS ts_end');
    expect(sqlDisplay).toContain('FROM slice AS s');
  });

  it('lowers interval_merge (OVERLAPPING and CONSECUTIVE) in both dialects', () => {
    const overlappingRoot: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'interval_merge',
        id: 'im1',
        config: {
          mergeKind: 'OVERLAPPING',
          keyColumns: [],
          partitionColumns: ['track_id'],
          aggregations: [{func: 'COUNT', column: '*', alias: 'slice_cnt'}],
          filterNegativeDur: true,
        },
      },
    };

    expect(getDisplay([overlappingRoot], 'im1', 'pipe')).toBe(
      [
        'FROM slice',
        '|> WHERE dur >= 0',
        '|> INTERVAL MERGE OVERLAPPING PER track_id AGGREGATE COUNT(*) AS slice_cnt',
      ].join('\n'),
    );

    const ovSqlText = getDisplay([overlappingRoot], 'im1', 'sql');
    expect(ovSqlText).toContain('MAX(ts + dur) OVER (PARTITION BY track_id');
    expect(ovSqlText).toContain('COUNT(*) AS slice_cnt');

    const consecutiveRoot: RootNodeData = {
      type: 'from',
      id: 'f2',
      x: 0,
      y: 0,
      config: {table: 'sched'},
      next: {
        type: 'interval_merge',
        id: 'im2',
        config: {
          mergeKind: 'CONSECUTIVE',
          keyColumns: ['utid', 'end_state'],
          partitionColumns: ['cpu'],
          aggregations: [],
          filterNegativeDur: false,
        },
      },
    };

    expect(getDisplay([consecutiveRoot], 'im2', 'pipe')).toBe(
      [
        'FROM sched',
        '|> INTERVAL MERGE CONSECUTIVE BY utid, end_state PER cpu AGGREGATE COUNT(*) AS _pf_cnt',
        '|> DROP _pf_cnt',
      ].join('\n'),
    );

    const consSqlText = getDisplay([consecutiveRoot], 'im2', 'sql');
    expect(consSqlText).toContain('LAG(ts + dur) OVER (PARTITION BY cpu');
    expect(consSqlText).toContain('GROUP BY cpu, _grp, utid, end_state');
  });

  it('lowers intervals_from_events in pipe mode and trivial/unsupported sql modes', () => {
    const simpleRoot: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'counter'},
      next: {
        type: 'intervals_from_events',
        id: 'ife1',
        config: {
          partitionColumns: ['track_id'],
          lastMode: 'CLOSE_AT',
          closeAtExpr: 'trace_end()',
        },
      },
    };

    expect(getDisplay([simpleRoot], 'ife1', 'pipe')).toBe(
      [
        'FROM counter',
        '|> INTERVALS FROM EVENTS PER track_id CLOSING LAST AT (trace_end())',
      ].join('\n'),
    );

    expect(getDisplay([simpleRoot], 'ife1', 'sql')).toContain(
      'COALESCE(LEAD(ts) OVER (PARTITION BY track_id ORDER BY ts), (trace_end())) - ts AS dur',
    );

    // With STOP connected: pipe emits `STOP <ref>`, while SQL marks it unsupported.
    const stopRoot: RootNodeData = {
      type: 'from',
      id: 'stop1',
      x: 200,
      y: 0,
      config: {table: 'process'},
    };
    const withStopRoot: RootNodeData = {
      type: 'from',
      id: 'ev1',
      x: 0,
      y: 0,
      config: {table: 'counter'},
      next: {
        type: 'intervals_from_events',
        id: 'ife2',
        inputs: [null, 'stop1'],
        config: {
          partitionColumns: ['track_id'],
          lastMode: 'KEEP_OPEN',
          closeAtExpr: 'trace_end()',
        },
      },
    };

    expect(getDisplay([stopRoot, withStopRoot], 'ife2', 'pipe')).toBe(
      [
        'FROM counter',
        '|> INTERVALS FROM EVENTS PER track_id STOP process KEEPING LAST OPEN',
      ].join('\n'),
    );

    const stopPlan = ensureDefined(
      buildLogicalPlan(makeNodes([stopRoot, withStopRoot]), 'ife2', undefined),
    );
    const stopSqlRes = formatLogicalPlan(stopPlan, 'sql');
    expect(stopSqlRes.ok).toBe(false);
    expect(getDisplay([stopRoot, withStopRoot], 'ife2', 'sql')).toContain(
      '-- Unsupported in Standard SQL: Intervals From Events with a Stop input is only supported in PerfettoSQL Next mode.',
    );
  });

  it('lowers data_table with typed values, NULLs, escaping, and 0-row tables in both dialects', () => {
    const populated: RootNodeData = {
      type: 'data_table',
      id: 'dt1',
      x: 0,
      y: 0,
      config: {
        columns: [
          {name: 'id', type: 'int'},
          {name: 'label', type: 'string'},
          {name: 'ratio', type: 'double'},
          {name: 'active', type: 'boolean'},
        ],
        rows: [
          ['1', "App's root", '2', 'true'],
          ['2', '', '', 'false'],
        ],
      },
      next: {
        type: 'drop',
        id: 'dr1',
        config: {columns: ['active']},
      },
    };

    const pipeSql = getDisplay([populated], 'dr1', 'pipe');
    expect(pipeSql).toContain('WITH _data(id, label, ratio, active) AS (');
    expect(pipeSql).toContain("(1, 'App''s root', 2.0, 1)");
    expect(pipeSql).toContain('(2, CAST(NULL AS TEXT), CAST(NULL AS REAL), 0)');
    expect(pipeSql).toContain('|> DROP active');

    const sqlText = getDisplay([populated], 'dr1', 'sql');
    expect(sqlText).toContain('WITH _data(id, label, ratio, active) AS (');
    expect(sqlText).toContain('id,\n  label,\n  ratio');

    const emptyRoot: RootNodeData = {
      type: 'data_table',
      id: 'dt_empty',
      x: 0,
      y: 0,
      config: {
        columns: [
          {name: 'src', type: 'int'},
          {name: 'dst', type: 'int'},
        ],
        rows: [],
      },
    };
    expect(getDisplay([emptyRoot], 'dt_empty', 'sql')).toBe(
      'SELECT\n  CAST(NULL AS INTEGER) AS src,\n  CAST(NULL AS INTEGER) AS dst\nWHERE 0',
    );
  });

  it('inlines multiple data_table branches directly into graph_tree without CREATE PERFETTO TABLE', () => {
    const nodesRoot: RootNodeData = {
      type: 'data_table',
      id: 'dt_nodes',
      x: 0,
      y: 0,
      config: {
        columns: [
          {name: 'id', type: 'int'},
          {name: 'self_size', type: 'int'},
        ],
        rows: [
          ['1', '10'],
          ['2', '20'],
        ],
      },
      next: {
        type: 'graph_tree',
        id: 'gt1',
        inputs: [null, 'dt_edges', 'dt_roots'],
        config: {
          kind: 'DOMINATOR',
          nodeIdColumn: 'id',
          sourceColumn: 'src',
          destColumn: 'dst',
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
      type: 'data_table',
      id: 'dt_edges',
      x: 200,
      y: 0,
      config: {
        columns: [
          {name: 'src', type: 'int'},
          {name: 'dst', type: 'int'},
        ],
        rows: [['1', '2']],
      },
    };
    const rootsRoot: RootNodeData = {
      type: 'data_table',
      id: 'dt_roots',
      x: 400,
      y: 0,
      config: {
        columns: [{name: 'id', type: 'int'}],
        rows: [['1']],
      },
    };

    const display = getDisplay(
      [nodesRoot, edgesRoot, rootsRoot],
      'ta1',
      'pipe',
    );
    expect(display).not.toContain('CREATE');
    expect(display).toBe(
      [
        'GRAPH DOMINATOR TREE',
        '  NODES (',
        '    WITH _data(id, self_size) AS (',
        '      VALUES',
        '        (1, 10),',
        '        (2, 20)',
        '    )',
        '    SELECT id, self_size FROM _data',
        '  )',
        '  EDGES (',
        '    WITH _data(src, dst) AS (',
        '      VALUES',
        '        (1, 2)',
        '    )',
        '    SELECT src, dst FROM _data',
        '  )',
        '  FROM (',
        '    WITH _data(id) AS (',
        '      VALUES',
        '        (1)',
        '    )',
        '    SELECT id FROM _data',
        '  )',
        '|> TREE ACCUMULATE UP SUM(self_size) AS retained_size',
      ].join('\n'),
    );
  });
});

describe('computeLogicalPlanHashes & planMaterialization', () => {
  it('computes identical Merkle hashes for semantically identical branches with different nodeIds', () => {
    const branchA: RootNodeData = {
      type: 'from',
      id: 'from_a',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'filter',
        id: 'flt_a',
        config: {conditions: [{column: 'dur', op: '>', value: '1000'}]},
      },
    };
    const branchB: RootNodeData = {
      type: 'from',
      id: 'from_b',
      x: 200,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'filter',
        id: 'flt_b',
        config: {conditions: [{column: 'dur', op: '>', value: '1000'}]},
      },
    };
    const union: RootNodeData = {
      type: 'union',
      id: 'u1',
      x: 100,
      y: 200,
      inputs: ['flt_a', 'flt_b'],
      config: {distinct: false, numInputs: 2},
    };

    const plan = ensureDefined(
      buildLogicalPlan(makeNodes([branchA, branchB, union]), 'u1', undefined),
    );
    const filterHash = nodeHash(plan, 'flt_a');
    expect(filterHash).toMatch(/^_qb_[0-9a-f]{8}$/);
    expect(nodeHash(plan, 'flt_b')).toBe(filterHash);

    // Union inputs are not forced into cache tables: cheap filter branches fold
    // into the union's own step as CTEs.
    const matPlan = planMaterialization(plan);
    expect(matPlan.steps.length).toBe(1);
    expect(matPlan.steps[0]?.nodeIds).toEqual(['flt_a', 'flt_b', 'u1']);
    expect(formatSql(matPlan.steps[0]!.plan)).toEqual({
      ok: true,
      sql: [
        'WITH',
        'step_1 AS (',
        '  SELECT *',
        '  FROM slice',
        '  WHERE dur > 1000',
        '),',
        'step_2 AS (',
        '  SELECT *',
        '  FROM slice',
        '  WHERE dur > 1000',
        ')',
        'SELECT *',
        'FROM step_1',
        'UNION ALL',
        'SELECT *',
        'FROM step_2',
      ].join('\n'),
      includes: [],
    });
  });

  it('changes Merkle hash when filter value or upstream table changes', () => {
    const makeFilterPlan = (table: string, val: string) =>
      ensureDefined(
        buildLogicalPlan(
          makeNodes([
            {
              type: 'from',
              id: 'f1',
              x: 0,
              y: 0,
              config: {table},
              next: {
                type: 'filter',
                id: 'flt1',
                config: {conditions: [{column: 'dur', op: '>', value: val}]},
              },
            },
          ]),
          'flt1',
          undefined,
        ),
      );

    const h1 = nodeHash(makeFilterPlan('slice', '1000'), 'flt1');
    const h2 = nodeHash(makeFilterPlan('slice', '2000'), 'flt1');
    const h3 = nodeHash(makeFilterPlan('sched', '1000'), 'flt1');
    expect(h1).not.toBe(h2);
    expect(h1).not.toBe(h3);
  });

  it('emits zero materialization steps for zero-cost queries (From, Chart, TimeRange, DataTable)', () => {
    const fromChart: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'chart',
        id: 'ch1',
        config: {charts: [], numInputs: 1},
      },
    };
    const index = makeNodes([fromChart]);
    const fromPlan = ensureDefined(buildLogicalPlan(index, 'f1', undefined));
    const fromMat = planMaterialization(fromPlan);
    expect(fromMat.steps).toEqual([]);
    expect(fromMat.finalRead.nodes[0]?.op).toEqual({
      kind: 'scan',
      table: 'slice',
    });

    const chartPlan = ensureDefined(buildLogicalPlan(index, 'ch1', undefined));
    const chartMat = planMaterialization(chartPlan);
    expect(chartMat.steps).toEqual([]);
    expect(chartMat.finalRead.nodes[0]?.op).toEqual({
      kind: 'scan',
      table: 'slice',
    });

    const tr: RootNodeData = {
      type: 'time_range',
      id: 'tr1',
      x: 0,
      y: 0,
      config: {ts: '10', dur: '20'},
    };
    const trPlan = ensureDefined(
      buildLogicalPlan(makeNodes([tr]), 'tr1', undefined),
    );
    const trMat = planMaterialization(trPlan);
    expect(trMat.steps).toEqual([]);
    expect(formatSql(trMat.finalRead)).toEqual({
      ok: true,
      sql: 'SELECT 0 AS id, 10 AS ts, 20 AS dur',
      includes: [],
    });
  });

  it('fuses pipelined ops into a single step', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'filter',
        id: 'flt1',
        config: {conditions: [{column: 'dur', op: '>', value: '100'}]},
        next: {
          type: 'sort',
          id: 'srt1',
          config: {conditions: [{column: 'dur', order: 'DESC'}]},
        },
      },
    };
    const plan = ensureDefined(
      buildLogicalPlan(makeNodes([root]), 'srt1', undefined),
    );
    const sortHash = nodeHash(plan, 'srt1');

    // Filter + Sort fuse into a single materialized step.
    const coldMat = planMaterialization(plan);
    expect(coldMat.steps.length).toBe(1);
    expect(coldMat.steps[0]?.hash).toBe(sortHash);
    expect(formatSql(coldMat.steps[0]!.plan)).toEqual({
      ok: true,
      sql: 'SELECT *\nFROM slice\nWHERE dur > 100\nORDER BY dur DESC',
      includes: [],
    });
  });

  it('materializes only expensive ops and folds cheap ops (including shared forks) into consumers', () => {
    // 1. Expensive op: GroupBy gets its own table; the cheap Filter target
    // reads from it in the final read without its own table.
    const gbChain: RootNodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'groupby',
        id: 'gb1',
        config: {
          groupColumns: ['name'],
          aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
        },
        next: {
          type: 'filter',
          id: 'flt1',
          config: {conditions: [{column: 'cnt', op: '>', value: '5'}]},
        },
      },
    };
    const gbPlan = ensureDefined(
      buildLogicalPlan(makeNodes([gbChain]), 'flt1', undefined),
    );
    const gbMat = planMaterialization(gbPlan);
    expect(gbMat.steps.length).toBe(1);
    expect(gbMat.steps[0]?.nodeIds).toEqual(['gb1']);
    expect(formatSql(gbMat.finalRead)).toEqual({
      ok: true,
      sql: `SELECT *\nFROM ${gbMat.steps[0]!.hash}\nWHERE cnt > 5`,
      includes: [],
    });

    // 2. Canvas fork after a cheap op: From(slice) -> Filter(flt_shared)
    // feeds Sort(branch_a) and Limit(branch_b). The shared Filter is not
    // checkpointed; it folds into the expensive Sort's table.
    const sharedRoot: RootNodeData = {
      type: 'from',
      id: 'f_shared',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: {
        type: 'filter',
        id: 'flt_shared',
        config: {conditions: [{column: 'dur', op: '>', value: '500'}]},
        next: {
          type: 'sort',
          id: 'branch_a',
          config: {conditions: [{column: 'dur', order: 'DESC'}]},
        },
      },
    };
    const branchB: RootNodeData = {
      type: 'limit',
      id: 'branch_b',
      x: 200,
      y: 100,
      inputs: ['flt_shared'],
      config: {limitCount: '10'},
    };
    const forkIndex = makeNodes([sharedRoot, branchB]);

    const forkMat = planMaterialization(
      ensureDefined(buildLogicalPlan(forkIndex, 'branch_a', undefined)),
    );
    expect(forkMat.steps.length).toBe(1);
    expect(forkMat.steps[0]?.nodeIds).toEqual(['flt_shared', 'branch_a']);

    // The cheap Limit branch needs no table at all.
    const limitMat = planMaterialization(
      ensureDefined(buildLogicalPlan(forkIndex, 'branch_b', undefined)),
    );
    expect(limitMat.steps.length).toBe(0);
  });
});

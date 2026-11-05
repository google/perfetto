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
import {buildDisplaySql, buildIR, buildLogicalPlan, type IrEntry} from './ir';

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

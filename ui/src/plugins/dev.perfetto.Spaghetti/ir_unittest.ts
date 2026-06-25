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

import type {NodeData, RootNodeData} from './graph_model';
import {buildIR, buildDisplaySql, type IrEntry} from './ir';
import {flattenNodes} from './graph_utils';

// Helper to build a graph index from root nodes.
function makeNodes(nodeList: RootNodeData[]) {
  return flattenNodes(nodeList);
}

describe('buildIR', () => {
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
    const entries = buildIR(nodes, 'f1', undefined);
    expect(entries).toBeDefined();
    expect(entries!.length).toBe(1);
    expect(entries![0].sql).toBe('SELECT *\nFROM slice');
    expect(entries![0].deps.length).toBe(0);
  });

  it('creates one IR entry for from -> filter chain', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        filterExpression: '',
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
    const entries = buildIR(nodes, 'flt1', undefined);
    expect(entries).toBeDefined();
    expect(entries!.length).toBe(1);
    expect(entries![0].sql).toContain('WHERE dur > 1000');
    expect(entries![0].sql).toContain('FROM slice');
    expect(entries![0].deps.length).toBe(0); // from node = no dep
    expect(entries![0].hash).toMatch(/^_qb_[0-9a-f]{8}$/);
  });

  it('folds select into the same statement as filter when possible', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        filterExpression: '',
        conditions: [{column: 'dur', op: '>', value: '100'}],
      },
    };
    const select: NodeData = {
      type: 'select',
      id: 's1',
      next: filter,
      config: {columns: {name: true, dur: true, ts: false}, expressions: []},
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
    const entries = buildIR(nodes, 'flt1', undefined);
    expect(entries).toBeDefined();
    // select + filter should fold into one entry
    expect(entries!.length).toBe(1);
    expect(entries![0].sql).toContain('SELECT name, dur');
    expect(entries![0].sql).toContain('WHERE dur > 100');
  });

  it('creates separate entries when folding is not possible', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        filterExpression: '',
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
    const entries = buildIR(nodes, 'flt1', undefined);
    expect(entries).toBeDefined();
    expect(entries!.length).toBe(2);
    // First entry is the groupby
    expect(entries![0].sql).toContain('GROUP BY name');
    // Second entry is filter, depends on groupby hash
    expect(entries![1].deps).toContain(entries![0].hash);
    expect(entries![1].sql).toContain('WHERE cnt > 5');
  });

  it('computes stable hashes based on content and deps', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        filterExpression: '',
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

    const e1 = buildIR(nodes, 'flt1', undefined)!;
    const e2 = buildIR(nodes, 'flt1', undefined)!;
    expect(e1[0].hash).toBe(e2[0].hash);
  });

  it('hash changes when filter value changes', () => {
    const filter1: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        filterExpression: '',
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const filter2: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        filterExpression: '',
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

    const e1 = buildIR(nodes1, 'flt1', undefined)!;
    const e2 = buildIR(nodes2, 'flt1', undefined)!;
    expect(e1[0].hash).not.toBe(e2[0].hash);
  });

  it('hash changes when upstream from table changes', () => {
    const makeGraph = (table: string) => {
      const filter: NodeData = {
        type: 'filter',
        id: 'flt1',
        config: {
          filterExpression: '',
          conditions: [{column: 'dur', op: '>', value: '1000'}],
        },
      };
      return makeNodes([
        {type: 'from', id: 'f1', x: 0, y: 0, next: filter, config: {table}},
      ]);
    };

    const g1 = makeGraph('slice');
    const g2 = makeGraph('sched');
    const e1 = buildIR(g1, 'flt1', undefined)!;
    const e2 = buildIR(g2, 'flt1', undefined)!;

    // The SQL differs (FROM slice vs FROM sched), so hashes differ.
    expect(e1[0].hash).not.toBe(e2[0].hash);
  });

  it('returns undefined when a node is invalid', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {filterExpression: '', conditions: []},
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
        filterExpression: '',
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
    const sql = buildDisplaySql(entries)!;
    expect(sql).toContain('SELECT *');
    expect(sql).toContain('FROM slice');
    expect(sql).toContain('WHERE dur > 1000');
    // No WITH clause for a single entry
    expect(sql).not.toContain('WITH');
  });

  it('generates WITH clause for multi-entry chains', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      config: {
        filterExpression: '',
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
    const sql = buildDisplaySql(entries)!;
    expect(sql).toContain('WITH');
    expect(sql).toContain('_qb_');
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
        filterExpression: '',
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
    const filterSql = buildDisplaySql(filterEntries)!;
    expect(filterSql).toContain('WHERE dur > 1000');
    expect(filterSql).not.toContain('ORDER BY');

    // Clicking on sort: build IR from sort upstream (includes filter)
    const sortEntries = getEntries([from], 'srt1');
    const sortSql = buildDisplaySql(sortEntries)!;
    expect(sortSql).toContain('WHERE dur > 1000');
    expect(sortSql).toContain('ORDER BY dur DESC');
  });
});

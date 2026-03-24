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

import {Connection} from '../../widgets/nodegraph';
import {buildIR, buildDisplaySql, IrEntry} from './ir';
import {NodeData} from './node_types';

// Helper to build a store from node arrays and connections.
function makeStore(
  nodeList: NodeData[],
  connections: Connection[] = [],
): {nodes: Map<string, NodeData>; connections: Connection[]} {
  const nodes = new Map<string, NodeData>();
  for (const n of nodeList) {
    nodes.set(n.id, n);
  }
  return {nodes, connections};
}

describe('buildIR', () => {
  it('returns undefined for nonexistent node', () => {
    const {nodes, connections} = makeStore([]);
    expect(buildIR(nodes, connections, 'missing', undefined)).toBeUndefined();
  });

  it('produces one entry for a single from node', () => {
    const from: NodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const {nodes, connections} = makeStore([from]);
    const entries = buildIR(nodes, connections, 'f1', undefined);
    expect(entries).toBeDefined();
    expect(entries!.length).toBe(1);
    expect(entries![0].sql).toBe('SELECT *\nFROM slice');
    expect(entries![0].deps.length).toBe(0);
  });

  it('creates one IR entry for from -> filter chain', () => {
    const from: NodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      nextId: 'flt1',
      config: {table: 'slice'},
    };
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      config: {
        filterExpression: '',
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const {nodes, connections} = makeStore([from, filter]);
    const entries = buildIR(nodes, connections, 'flt1', undefined);
    expect(entries).toBeDefined();
    expect(entries!.length).toBe(1);
    expect(entries![0].sql).toContain('WHERE dur > 1000');
    expect(entries![0].sql).toContain('FROM slice');
    expect(entries![0].deps.length).toBe(0); // from node = no dep
    expect(entries![0].hash).toMatch(/^_qb_[0-9a-f]{8}$/);
  });

  it('folds select into the same statement as filter when possible', () => {
    const from: NodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      nextId: 's1',
      config: {table: 'slice'},
    };
    const select: NodeData = {
      type: 'select',
      id: 's1',
      x: 0,
      y: 0,
      nextId: 'flt1',
      config: {columns: {name: true, dur: true, ts: false}, expressions: []},
    };
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      config: {
        filterExpression: '',
        conditions: [{column: 'dur', op: '>', value: '100'}],
      },
    };
    const {nodes, connections} = makeStore([from, select, filter]);
    const entries = buildIR(nodes, connections, 'flt1', undefined);
    expect(entries).toBeDefined();
    // select + filter should fold into one entry
    expect(entries!.length).toBe(1);
    expect(entries![0].sql).toContain('SELECT name, dur');
    expect(entries![0].sql).toContain('WHERE dur > 100');
  });

  it('creates separate entries when folding is not possible', () => {
    const from: NodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      nextId: 'gb1',
      config: {table: 'slice'},
    };
    const groupby: NodeData = {
      type: 'groupby',
      id: 'gb1',
      x: 0,
      y: 0,
      nextId: 'flt1',
      config: {
        groupColumns: ['name'],
        aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
      },
    };
    // Filter after groupby can't fold into the groupby statement
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      config: {
        filterExpression: '',
        conditions: [{column: 'cnt', op: '>', value: '5'}],
      },
    };
    const {nodes, connections} = makeStore([from, groupby, filter]);
    const entries = buildIR(nodes, connections, 'flt1', undefined);
    expect(entries).toBeDefined();
    expect(entries!.length).toBe(2);
    // First entry is the groupby
    expect(entries![0].sql).toContain('GROUP BY name');
    // Second entry is filter, depends on groupby hash
    expect(entries![1].deps).toContain(entries![0].hash);
    expect(entries![1].sql).toContain('WHERE cnt > 5');
  });

  it('computes stable hashes based on content and deps', () => {
    const from: NodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      nextId: 'flt1',
      config: {table: 'slice'},
    };
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      config: {
        filterExpression: '',
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const {nodes, connections} = makeStore([from, filter]);

    const e1 = buildIR(nodes, connections, 'flt1', undefined)!;
    const e2 = buildIR(nodes, connections, 'flt1', undefined)!;
    expect(e1[0].hash).toBe(e2[0].hash);
  });

  it('hash changes when filter value changes', () => {
    const filter1: NodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      config: {
        filterExpression: '',
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const filter2: NodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      config: {
        filterExpression: '',
        conditions: [{column: 'dur', op: '>', value: '2000'}],
      },
    };

    const store1 = makeStore([
      {
        type: 'from',
        id: 'f1',
        x: 0,
        y: 0,
        nextId: 'flt1',
        config: {table: 'slice'},
      },
      filter1,
    ]);
    const store2 = makeStore([
      {
        type: 'from',
        id: 'f1',
        x: 0,
        y: 0,
        nextId: 'flt1',
        config: {table: 'slice'},
      },
      filter2,
    ]);

    const e1 = buildIR(store1.nodes, store1.connections, 'flt1', undefined)!;
    const e2 = buildIR(store2.nodes, store2.connections, 'flt1', undefined)!;
    expect(e1[0].hash).not.toBe(e2[0].hash);
  });

  it('hash changes when upstream from table changes', () => {
    const makeGraph = (table: string) => {
      const from: NodeData = {
        type: 'from',
        id: 'f1',
        x: 0,
        y: 0,
        nextId: 'flt1',
        config: {table},
      };
      const filter: NodeData = {
        type: 'filter',
        id: 'flt1',
        x: 0,
        y: 0,
        config: {
          filterExpression: '',
          conditions: [{column: 'dur', op: '>', value: '1000'}],
        },
      };
      return makeStore([from, filter]);
    };

    const g1 = makeGraph('slice');
    const g2 = makeGraph('sched');
    const e1 = buildIR(g1.nodes, g1.connections, 'flt1', undefined)!;
    const e2 = buildIR(g2.nodes, g2.connections, 'flt1', undefined)!;

    // The SQL differs (FROM slice vs FROM sched), so hashes differ.
    expect(e1[0].hash).not.toBe(e2[0].hash);
  });

  it('returns undefined when a node is invalid', () => {
    const from: NodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      nextId: 'flt1',
      config: {table: ''},
    };
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      config: {filterExpression: '', conditions: []},
    };
    const {nodes, connections} = makeStore([from, filter]);
    expect(buildIR(nodes, connections, 'flt1', undefined)).toBeUndefined();
  });
});

describe('buildDisplaySql', () => {
  function getEntries(
    nodeList: NodeData[],
    nodeId: string,
    connections: Connection[] = [],
  ): IrEntry[] {
    const {nodes} = makeStore(nodeList, connections);
    const entries = buildIR(nodes, connections, nodeId, undefined);
    if (!entries) throw new Error('buildIR returned undefined');
    return entries;
  }

  it('returns undefined for empty entries', () => {
    expect(buildDisplaySql([])).toBeUndefined();
  });

  it('returns simple query for from -> filter', () => {
    const from: NodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      nextId: 'flt1',
      config: {table: 'slice'},
    };
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      config: {
        filterExpression: '',
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const entries = getEntries([from, filter], 'flt1');
    const sql = buildDisplaySql(entries)!;
    expect(sql).toContain('SELECT *');
    expect(sql).toContain('FROM slice');
    expect(sql).toContain('WHERE dur > 1000');
    // No WITH clause for a single entry
    expect(sql).not.toContain('WITH');
  });

  it('generates WITH clause for multi-entry chains', () => {
    const from: NodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      nextId: 'gb1',
      config: {table: 'slice'},
    };
    const groupby: NodeData = {
      type: 'groupby',
      id: 'gb1',
      x: 0,
      y: 0,
      nextId: 'flt1',
      config: {
        groupColumns: ['name'],
        aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
      },
    };
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      config: {
        filterExpression: '',
        conditions: [{column: 'cnt', op: '>', value: '5'}],
      },
    };
    const entries = getEntries([from, groupby, filter], 'flt1');
    const sql = buildDisplaySql(entries)!;
    expect(sql).toContain('WITH');
    expect(sql).toContain('_qb_');
    expect(sql).toContain('GROUP BY name');
    expect(sql).toContain('WHERE cnt > 5');
  });

  it('shows correct SQL when clicking intermediate node', () => {
    const from: NodeData = {
      type: 'from',
      id: 'f1',
      x: 0,
      y: 0,
      nextId: 'flt1',
      config: {table: 'slice'},
    };
    const filter: NodeData = {
      type: 'filter',
      id: 'flt1',
      x: 0,
      y: 0,
      nextId: 'srt1',
      config: {
        filterExpression: '',
        conditions: [{column: 'dur', op: '>', value: '1000'}],
      },
    };
    const sort: NodeData = {
      type: 'sort',
      id: 'srt1',
      x: 0,
      y: 0,
      config: {sortColumn: 'dur', sortOrder: 'DESC'},
    };
    const allNodes = [from, filter, sort];

    // Clicking on filter: build IR from filter upstream only
    const filterEntries = getEntries(allNodes, 'flt1');
    const filterSql = buildDisplaySql(filterEntries)!;
    expect(filterSql).toContain('WHERE dur > 1000');
    expect(filterSql).not.toContain('ORDER BY');

    // Clicking on sort: build IR from sort upstream (includes filter)
    const sortEntries = getEntries(allNodes, 'srt1');
    const sortSql = buildDisplaySql(sortEntries)!;
    expect(sortSql).toContain('WHERE dur > 1000');
    expect(sortSql).toContain('ORDER BY dur DESC');
  });
});

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

import type {RootNodeData} from './graph_model';
import {
  deserializeStore,
  extractNodesForCopy,
  fnvHash,
  getManifest,
  instantiateCopiedNodes,
  removeNodeFromGraph,
  serializeStore,
} from './graph_utils';

describe('fnvHash', () => {
  it('returns an 8-char hex string', () => {
    const h = fnvHash('hello');
    expect(h.length).toBe(8);
    expect(/^[0-9a-f]{8}$/.test(h)).toBe(true);
  });

  it('produces different hashes for different inputs', () => {
    expect(fnvHash('hello')).not.toBe(fnvHash('world'));
  });

  it('produces the same hash for the same input', () => {
    expect(fnvHash('test')).toBe(fnvHash('test'));
  });
});

describe('deserializeStore', () => {
  const allNodeTypes = [
    'from',
    'time_range',
    'select',
    'extend',
    'drop',
    'rename',
    'set',
    'distinct',
    'filter',
    'sort',
    'limit',
    'groupby',
    'join',
    'extract_arg',
    'interval_intersect',
    'interval_flatten',
    'interval_fill_gaps',
    'tree_accumulate',
    'graph_tree',
    'union',
    'chart',
    'sql',
  ];

  it('validates defaultConfig for every registered node type', () => {
    for (const type of allNodeTypes) {
      const manifest = getManifest(type);
      expect(manifest).toBeDefined();
      const defaultConfig = manifest.defaultConfig();
      expect(manifest.schema.safeParse(defaultConfig).success).toBe(true);
    }
  });

  it('round-trips a valid graph with chained nodes and labels', () => {
    const input = {
      nodes: [
        {
          id: 'n1',
          type: 'from',
          x: 100,
          y: 200,
          config: {table: 'slice'},
          next: {
            id: 'n2',
            type: 'filter',
            config: {
              conditions: [{column: 'dur', op: '>', value: '1000'}],
              conjunction: 'AND',
            },
            next: {
              id: 'n3',
              type: 'limit',
              config: {limitCount: '50'},
            },
          },
        },
      ],
      labels: [
        {
          id: 'l1',
          x: 10,
          y: 20,
          width: 150,
          content: 'My note',
        },
      ],
    };

    const parsed = deserializeStore(JSON.stringify(input));
    expect(JSON.parse(serializeStore(parsed))).toEqual(input);
  });

  it('rejects unknown node types and Object.prototype keys', () => {
    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [{id: '1', type: 'nonexistent', x: 0, y: 0, config: {}}],
          labels: [],
        }),
      ),
    ).toThrow(/Unknown node type/);

    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [{id: '1', type: 'toString', x: 0, y: 0, config: {}}],
          labels: [],
        }),
      ),
    ).toThrow(/Unknown node type/);
  });

  it('rejects invalid config on root or chained nodes', () => {
    // Root node with wrong config type (number instead of string for table)
    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [{id: '1', type: 'from', x: 0, y: 0, config: {table: 123}}],
          labels: [],
        }),
      ),
    ).toThrow();

    // Chained node with invalid filter operator
    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [
            {
              id: '1',
              type: 'from',
              x: 0,
              y: 0,
              config: {table: 'slice'},
              next: {
                id: '2',
                type: 'filter',
                config: {
                  conditions: [{column: 'id', op: 'INVALID_OP', value: '1'}],
                },
              },
            },
          ],
          labels: [],
        }),
      ),
    ).toThrow();
  });

  it('rejects fake Mithril vnodes in labels and node configs', () => {
    const fakeVnode = {tag: '<', children: '<img src=x onerror=alert(1)>'};

    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [],
          labels: [{id: 'l1', x: 0, y: 0, width: 100, content: fakeVnode}],
        }),
      ),
    ).toThrow();

    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [
            {
              id: '1',
              type: 'sql',
              x: 0,
              y: 0,
              config: {sql: 'SELECT 1', inputPorts: [fakeVnode]},
            },
          ],
          labels: [],
        }),
      ),
    ).toThrow();
  });
});

describe('removeNodeFromGraph', () => {
  function makeStack(): RootNodeData[] {
    return [
      {
        id: 'n1',
        type: 'from',
        x: 120,
        y: 240,
        config: {table: 'slice'},
        next: {
          id: 'n2',
          type: 'filter',
          config: {conditions: [], conjunction: 'AND'},
          next: {
            id: 'n3',
            type: 'limit',
            config: {limitCount: '50'},
          },
        },
      },
      {
        id: 'j1',
        type: 'join',
        x: 500,
        y: 240,
        inputs: ['n3', 'other'],
        config: {
          joinType: 'INNER',
          leftCol: 'id',
          rightCol: 'id',
          rightAssignments: [],
        },
      },
    ];
  }

  it('splices out a node from the middle of a stack', () => {
    const nodes = makeStack();
    expect(removeNodeFromGraph(nodes, 'n2')).toBe(true);
    expect(nodes[0]?.id).toBe('n1');
    expect(nodes[0]?.next?.id).toBe('n3');
    expect(nodes[0]?.next?.next).toBeUndefined();
    expect(nodes[1]?.inputs).toEqual(['n3', 'other']);
  });

  it('removes a tail node from a stack and rewires downstream consumers to its docked parent', () => {
    const nodes = makeStack();
    expect(removeNodeFromGraph(nodes, 'n3')).toBe(true);
    expect(nodes[0]?.id).toBe('n1');
    expect(nodes[0]?.next?.id).toBe('n2');
    expect(nodes[0]?.next?.next).toBeUndefined();
    expect(nodes[1]?.inputs).toEqual(['n2', 'other']);
  });

  it('promotes the next node in place when deleting the root of a stack', () => {
    const nodes = makeStack();
    expect(removeNodeFromGraph(nodes, 'n1')).toBe(true);
    expect(nodes).toHaveLength(2);
    expect(nodes[0]).toMatchObject({
      id: 'n2',
      type: 'filter',
      x: 120,
      y: 240,
    });
    expect(nodes[0]?.next?.id).toBe('n3');
  });

  it('removes a standalone root node and clears wired inputs pointing to it', () => {
    const nodes: RootNodeData[] = [
      {id: 'src', type: 'from', x: 0, y: 0, config: {table: 'slice'}},
      {
        id: 'flt',
        type: 'filter',
        x: 200,
        y: 0,
        inputs: ['src'],
        config: {conditions: [], conjunction: 'AND'},
      },
    ];
    expect(removeNodeFromGraph(nodes, 'src')).toBe(true);
    expect(nodes).toHaveLength(1);
    expect(nodes[0]?.id).toBe('flt');
    expect(nodes[0]?.inputs).toEqual([]);
  });
});

describe('extractNodesForCopy and instantiateCopiedNodes', () => {
  it('copies a single docked node out of the middle of a stack', () => {
    const nodes: RootNodeData[] = [
      {
        id: 'n1',
        type: 'from',
        x: 100,
        y: 200,
        config: {table: 'slice'},
        next: {
          id: 'n2',
          type: 'filter',
          config: {conditions: [], conjunction: 'AND'},
          next: {
            id: 'n3',
            type: 'select',
            config: {entries: [], expressions: []},
          },
        },
      },
    ];

    const copied = extractNodesForCopy(nodes, new Set(['n2']));
    expect(copied).toHaveLength(1);
    expect(copied[0]).toMatchObject({
      id: 'n2',
      type: 'filter',
      x: 100,
      y: 260,
    });
    expect(copied[0]?.next).toBeUndefined();

    const pasted = instantiateCopiedNodes(copied, {dx: 40, dy: 40});
    expect(pasted).toHaveLength(1);
    expect(pasted[0]?.id).not.toBe('n2');
    expect(pasted[0]?.x).toBe(140);
    expect(pasted[0]?.y).toBe(300);
  });

  it('preserves contiguous docked selections and internal wires while stripping external wires', () => {
    const nodes: RootNodeData[] = [
      {id: 'ext', type: 'from', x: 0, y: 0, config: {table: 'thread'}},
      {
        id: 'n1',
        type: 'from',
        x: 100,
        y: 100,
        config: {table: 'slice'},
        next: {
          id: 'n2',
          type: 'filter',
          config: {conditions: [], conjunction: 'AND'},
        },
      },
      {
        id: 'join1',
        type: 'join',
        x: 350,
        y: 150,
        inputs: ['n2', 'ext'],
        config: {
          joinType: 'INNER',
          leftCol: 'id',
          rightCol: 'id',
          rightAssignments: [],
        },
      },
    ];

    const copied = extractNodesForCopy(nodes, new Set(['n1', 'n2', 'join1']));
    expect(copied).toHaveLength(2);
    expect(copied[0]?.id).toBe('n1');
    expect(copied[0]?.next?.id).toBe('n2');
    // Internal wire to 'n2' is preserved; external wire to 'ext' is trimmed.
    expect(copied[1]?.inputs).toEqual(['n2']);

    const pasted = instantiateCopiedNodes(copied, {dx: 40, dy: 40});
    const newChainTailId = pasted[0]?.next?.id;
    expect(newChainTailId).toBeDefined();
    expect(newChainTailId).not.toBe('n2');
    expect(pasted[1]?.inputs).toEqual([newChainTailId]);
  });
});

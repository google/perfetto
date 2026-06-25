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
import {
  flattenNodes,
  chainTail,
  findConnectedInputs,
  getPrimaryInput,
  collectUpstream,
  findDockedParent,
} from './graph_utils';

// --- flattenNodes ---

describe('flattenNodes', () => {
  function makeRoots(nodes: RootNodeData[]) {
    return flattenNodes(nodes);
  }

  it('returns a single node for a single root', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 10,
      y: 20,
      config: {table: 'slice'},
    };
    const index = makeRoots([root]);
    expect(index.nodes['r1']).toBe(root);
    expect(Object.keys(index.nodes).length).toBe(1);
  });

  it('flattens a chain of nodes', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'f1',
      config: {conditions: []},
    };
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: filter,
    };
    const index = makeRoots([root]);
    expect(index.nodes['r1']).toBe(root);
    expect(index.nodes['f1']).toBe(filter);
    expect(Object.keys(index.nodes).length).toBe(2);
  });

  it('handles multiple independent chains', () => {
    const root1: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const root2: RootNodeData = {
      type: 'from',
      id: 'r2',
      x: 100,
      y: 100,
      config: {table: 'sched'},
    };
    const index = makeRoots([root1, root2]);
    expect(index.nodes['r1']).toBe(root1);
    expect(index.nodes['r2']).toBe(root2);
    expect(Object.keys(index.nodes).length).toBe(2);
  });

  it('childToParent gives correct inverse mapping', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'f1',
      config: {conditions: []},
    };
    const select: NodeData = {
      type: 'select',
      id: 's1',
      config: {},
      next: filter,
    };
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: select,
    };
    const index = makeRoots([root]);

    expect(findDockedParent(index, 'f1')).toBe(select);
    expect(findDockedParent(index, 's1')).toBe(root);
    expect(findDockedParent(index, 'r1')).toBeUndefined();
  });

  it('handles deeply nested chains', () => {
    const n3: NodeData = {type: 'limit', id: 'n3', config: {limit: 10}};
    const n2: NodeData = {
      type: 'sort',
      id: 'n2',
      config: {sortColumn: '', sortOrder: 'ASC'},
      next: n3,
    };
    const n1: NodeData = {
      type: 'filter',
      id: 'n1',
      config: {conditions: []},
      next: n2,
    };
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: n1,
    };
    const index = makeRoots([root]);

    expect(Object.keys(index.nodes).length).toBe(4);
    expect(findDockedParent(index, 'n3')).toBe(n2);
    expect(findDockedParent(index, 'n2')).toBe(n1);
    expect(findDockedParent(index, 'n1')).toBe(root);
  });

  it('handles nodes with no next (singleton)', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const index = makeRoots([root]);
    expect(findDockedParent(index, 'r1')).toBeUndefined();
  });
});

// --- chainTail ---

describe('chainTail', () => {
  it('returns the node itself when no next', () => {
    const node: NodeData = {type: 'from', id: 'n1', config: {table: 'slice'}};
    expect(chainTail(node)).toBe(node);
  });

  it('returns the last node in a chain', () => {
    const tail: NodeData = {type: 'limit', id: 't1', config: {limit: 10}};
    const head: NodeData = {
      type: 'from',
      id: 'h1',
      config: {table: 'slice'},
      next: tail,
    };
    expect(chainTail(head)).toBe(tail);
  });

  it('handles long chains', () => {
    const n3: NodeData = {type: 'limit', id: 'n3', config: {limit: 10}};
    const n2: NodeData = {type: 'sort', id: 'n2', config: {}, next: n3};
    const n1: NodeData = {
      type: 'filter',
      id: 'n1',
      config: {conditions: []},
      next: n2,
    };
    expect(chainTail(n1)).toBe(n3);
  });
});

// --- findConnectedInputs ---

describe('findConnectedInputs', () => {
  it('returns empty map when no inputs array', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const index = flattenNodes([root]);
    const result = findConnectedInputs(index, 'r1');
    expect(result.size).toBe(0);
  });

  it('returns connected nodes by port index for wired inputs', () => {
    // Build a graph where a join node has wired inputs from two from nodes.
    // The join node must be reachable from a root chain.
    const join: NodeData = {
      type: 'join',
      id: 'join',
      config: {joinType: 'LEFT', leftColumn: '', rightColumn: ''},
      inputs: ['left', 'right'],
    };
    // left is the root; left.next = join; right is a separate root
    const roots: RootNodeData[] = [
      {
        type: 'from',
        id: 'left',
        x: 0,
        y: 0,
        config: {table: 'slice'},
        next: join,
      },
      {type: 'from', id: 'right', x: 100, y: 100, config: {table: 'sched'}},
    ];
    const index = flattenNodes(roots);
    const result = findConnectedInputs(index, 'join');
    expect(result.size).toBe(2);
    // Get nodes from the index (they are the actual objects)
    expect(result.get(0)?.id).toBe('left');
    expect(result.get(1)?.id).toBe('right');
  });

  it('skips null entries in inputs array', () => {
    const roots: RootNodeData[] = [
      {type: 'from', id: 'right', x: 100, y: 100, config: {table: 'sched'}},
      {
        type: 'join',
        id: 'join',
        x: 50,
        y: 50,
        config: {joinType: 'LEFT', leftColumn: '', rightColumn: ''},
        inputs: [null, 'right'] as (string | null)[],
      },
    ];
    const index = flattenNodes(roots);
    const result = findConnectedInputs(index, 'join');
    expect(result.size).toBe(1);
    expect(result.get(1)?.id).toBe('right');
  });

  it('skips undefined entries', () => {
    const roots = [
      {
        type: 'filter',
        id: 'n1',
        x: 0,
        y: 0,
        config: {conditions: []},
        inputs: [null, undefined] as (string | null | undefined)[],
      },
    ] as RootNodeData[];
    const result = findConnectedInputs(flattenNodes(roots), 'n1');
    expect(result.size).toBe(0);
  });
});

// --- getPrimaryInput ---

describe('getPrimaryInput', () => {
  it('returns the docked parent for a chain node', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'f1',
      config: {conditions: []},
    };
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: filter,
    };
    const index = flattenNodes([root]);
    expect(getPrimaryInput(index, 'f1')).toBe(root);
  });

  it('returns undefined for root nodes (no docked parent, no wired input)', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const index = flattenNodes([root]);
    expect(getPrimaryInput(index, 'r1')).toBeUndefined();
  });

  it('returns wired input port 0 when no docked parent', () => {
    const roots: RootNodeData[] = [
      {type: 'from', id: 'up', x: 0, y: 0, config: {table: 'slice'}},
      {
        type: 'filter',
        id: 'down',
        x: 100,
        y: 100,
        config: {conditions: []},
        inputs: ['up'],
      },
    ];
    const index = flattenNodes(roots);
    expect(getPrimaryInput(index, 'down')?.id).toBe('up');
  });
});

// --- collectUpstream ---

describe('collectUpstream', () => {
  it('returns single node for a root with no inputs', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const index = flattenNodes([root]);
    const order: NodeData[] = [];
    collectUpstream(index, 'r1', new Set(), order);
    expect(order).toHaveLength(1);
    expect(order[0].id).toBe('r1');
  });

  it('returns nodes in topological order (dependencies first) for a chain', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'f1',
      config: {conditions: []},
    };
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: filter,
    };
    const index = flattenNodes([root]);
    const order: NodeData[] = [];
    collectUpstream(index, 'f1', new Set(), order);
    expect(order).toHaveLength(2);
    expect(order[0].id).toBe('r1');
    expect(order[1].id).toBe('f1');
  });

  it('returns nodes in topological order for a long chain', () => {
    const limit: NodeData = {type: 'limit', id: 'l1', config: {limit: 10}};
    const sort: NodeData = {
      type: 'sort',
      id: 's1',
      config: {sortColumn: '', sortOrder: 'ASC'},
      next: limit,
    };
    const filter: NodeData = {
      type: 'filter',
      id: 'f1',
      config: {conditions: []},
      next: sort,
    };
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: filter,
    };
    const index = flattenNodes([root]);
    const order: NodeData[] = [];
    collectUpstream(index, 'l1', new Set(), order);
    expect(order).toHaveLength(4);
    expect(order[0].id).toBe('r1');
    expect(order[1].id).toBe('f1');
    expect(order[2].id).toBe('s1');
    expect(order[3].id).toBe('l1');
  });

  it('handles multi-input nodes (join) with wired inputs', () => {
    // Build: left(root) → join → filter(root2), with right as separate root
    const join: NodeData = {
      type: 'join',
      id: 'join',
      config: {joinType: 'LEFT', leftColumn: '', rightColumn: ''},
      inputs: ['left', 'right'],
    };
    const f1: RootNodeData = {
      type: 'filter',
      id: 'f1',
      x: 200,
      y: 200,
      config: {conditions: []},
      inputs: ['join'],
    };
    const roots: RootNodeData[] = [
      {
        type: 'from',
        id: 'left',
        x: 0,
        y: 0,
        config: {table: 'slice'},
        next: join,
      },
      {type: 'from', id: 'right', x: 100, y: 100, config: {table: 'sched'}},
      f1,
    ];
    const index = flattenNodes(roots);
    const order: NodeData[] = [];
    collectUpstream(index, 'f1', new Set(), order);
    // f1 → join → left, right
    expect(order).toHaveLength(4);
    expect(order[3].id).toBe('f1');
    expect(order[2].id).toBe('join');
    expect(order.map((n) => n.id)).toContain('left');
    expect(order.map((n) => n.id)).toContain('right');
  });

  it('does not visit the same node twice', () => {
    const filter: NodeData = {
      type: 'filter',
      id: 'f1',
      config: {conditions: []},
    };
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
      next: filter,
    };
    const index = flattenNodes([root]);

    const order1: NodeData[] = [];
    const visited1 = new Set<string>();
    collectUpstream(index, 'f1', visited1, order1);

    const order2: NodeData[] = [];
    const visited2 = new Set<string>();
    collectUpstream(index, 'f1', visited2, order2);

    expect(order1).toEqual(order2);
  });

  it('handles non-existent node', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const index = flattenNodes([root]);
    const order: NodeData[] = [];
    collectUpstream(index, 'nonexistent', new Set(), order);
    expect(order).toHaveLength(0);
  });
});

// --- trimInputs helper (used in spaghetti_page.ts) ---

function trimInputs(inputs: (string | null)[]): (string | null)[] {
  let end = inputs.length;
  while (end > 0 && inputs[end - 1] === null) end--;
  return inputs.slice(0, end);
}

describe('trimInputs', () => {
  it('trims trailing nulls', () => {
    expect(trimInputs(['a', 'b', null])).toEqual(['a', 'b']);
  });

  it('handles all nulls', () => {
    expect(trimInputs([null, null])).toEqual([]);
  });

  it('handles empty array', () => {
    expect(trimInputs([])).toEqual([]);
  });

  it('handles no trailing nulls', () => {
    expect(trimInputs(['a', 'b', 'c'])).toEqual(['a', 'b', 'c']);
  });

  it('handles single non-null', () => {
    expect(trimInputs(['a'])).toEqual(['a']);
  });

  it('handles single null', () => {
    expect(trimInputs([null])).toEqual([]);
  });
});

// --- Node removal logic ---

describe('Node removal', () => {
  it('removes a root node and clears references', () => {
    const join: NodeData = {
      type: 'join',
      id: 'join',
      config: {joinType: 'LEFT', leftColumn: '', rightColumn: ''},
      inputs: ['left', 'right'],
    };
    const roots: RootNodeData[] = [
      {
        type: 'from',
        id: 'left',
        x: 0,
        y: 0,
        config: {table: 'slice'},
        next: join,
      },
      {type: 'from', id: 'right', x: 100, y: 100, config: {table: 'sched'}},
    ];
    const index = flattenNodes(roots);

    // Remove "right" node
    for (const node of Object.values(index.nodes)) {
      if (!node.inputs) continue;
      for (let i = 0; i < node.inputs.length; i++) {
        if (node.inputs[i] === 'right') node.inputs[i] = null;
      }
      node.inputs = trimInputs(node.inputs);
    }

    expect(join.inputs).toEqual(['left']);
  });

  it('handles removal when node is not referenced', () => {
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 0,
      y: 0,
      config: {table: 'slice'},
    };
    const index = flattenNodes([root]);

    // Remove "r2" which doesn't exist — no error
    for (const node of Object.values(index.nodes)) {
      if (!node.inputs) continue;
      for (let i = 0; i < node.inputs.length; i++) {
        if (node.inputs[i] === 'r2') node.inputs[i] = null;
      }
      node.inputs = trimInputs(node.inputs);
    }

    expect(Object.keys(index.nodes).length).toBe(1);
  });
});

// --- Docking / Undocking logic ---

describe('Docking and undocking', () => {
  it('docking appends to chain via chainTail', () => {
    const target: NodeData = {
      type: 'filter',
      id: 'target',
      config: {conditions: []},
    };
    const child: NodeData = {
      type: 'limit',
      id: 'child',
      config: {limit: 10},
    };

    // Simulate docking: chainTail(target).next = child
    chainTail(target).next = child;

    expect(target.next).toBe(child);
    expect(chainTail(target)).toBe(child);
  });

  it('undocking splits chain at the right node', () => {
    const parent: NodeData = {
      type: 'filter',
      id: 'parent',
      config: {conditions: []},
    };
    const child: NodeData = {
      type: 'limit',
      id: 'child',
      config: {limit: 10},
    };
    const grandchild: NodeData = {
      type: 'sort',
      id: 'grandchild',
      config: {sortColumn: '', sortOrder: 'ASC'},
    };
    child.next = grandchild;
    parent.next = child;

    // Simulate undocking: find child in parent's chain and split
    let cur: NodeData = parent;
    while (cur.next) {
      if (cur.next.id === 'child') {
        const detached = cur.next;
        cur.next = undefined;
        // detached is now {limit, next: sort}
        expect(detached.id).toBe('child');
        expect(detached.next).toBe(grandchild);
        expect(parent.next).toBeUndefined();
        break;
      }
      cur = cur.next;
    }
  });

  it('undocking handles child at end of chain', () => {
    const parent: NodeData = {
      type: 'filter',
      id: 'parent',
      config: {conditions: []},
    };
    const child: NodeData = {
      type: 'limit',
      id: 'child',
      config: {limit: 10},
    };
    parent.next = child;

    // Undock child (it's the last node)
    let cur: NodeData = parent;
    while (cur.next) {
      if (cur.next.id === 'child') {
        const detached = cur.next;
        cur.next = undefined;
        expect(detached.id).toBe('child');
        expect(detached.next).toBeUndefined();
        expect(parent.next).toBeUndefined();
        break;
      }
      cur = cur.next;
    }
  });
});

// --- Root node positioning ---

describe('Root node positioning', () => {
  it('only root nodes have x/y coordinates', () => {
    const child: NodeData = {
      type: 'filter',
      id: 'c1',
      config: {conditions: []},
    };
    const root: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 100,
      y: 200,
      config: {table: 'slice'},
      next: child,
    };

    // root has coordinates
    expect(root.x).toBe(100);
    expect(root.y).toBe(200);
    // child is a chain node, not a RootNodeData — no x/y properties
    expect('x' in child).toBe(false);
  });

  it('multiple roots have independent positions', () => {
    const root1: RootNodeData = {
      type: 'from',
      id: 'r1',
      x: 10,
      y: 20,
      config: {table: 'slice'},
    };
    const root2: RootNodeData = {
      type: 'from',
      id: 'r2',
      x: 100,
      y: 200,
      config: {table: 'sched'},
    };
    expect((flattenNodes([root1, root2]).nodes['r1'] as RootNodeData).x).toBe(
      10,
    );
    expect((flattenNodes([root1, root2]).nodes['r2'] as RootNodeData).x).toBe(
      100,
    );
  });
});

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

import {
  getAllNodes,
  getAllDownstreamNodes,
  getAllUpstreamNodes,
  findNodeById,
  findDockedChildren,
  wouldCreateCycle,
  createGroupFromSelection,
  applyGroupRewiring,
  ungroupNode,
} from './graph_utils';
import {QueryNode, NodeType} from '../query_node';
import {TableSourceNode} from './nodes/sources/table_source';
import {Trace} from '../../../public/trace';
import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {
  createMockNode,
  connectNodes,
  connectSecondary,
  createUnboundedSecondaryInputs,
} from './testing/test_utils';
import {GroupNode} from './nodes/group_node';
import {unwrapResult} from '../../../base/result';

describe('graph_utils', () => {
  let mockTrace: Trace;
  let mockSqlModules: SqlModules;

  beforeEach(() => {
    mockTrace = {
      traceInfo: {
        traceTitle: 'test_trace',
      },
    } as Trace;

    mockSqlModules = {
      listTables: () => [],
      getTable: () => null,
      listModules: () => [],
      listTablesNames: () => [],
      getModuleForTable: () => undefined,
    } as unknown as SqlModules;
  });

  // Helper to create a simple test node (source node)
  function createTestNode(): QueryNode {
    return new TableSourceNode({
      trace: mockTrace,
      sqlModules: mockSqlModules,
    }) as QueryNode;
  }

  describe('getAllNodes', () => {
    it('should return empty array for empty root nodes', () => {
      const result = getAllNodes([]);
      expect(result).toEqual([]);
    });

    it('should return single node', () => {
      const node = createTestNode();
      const result = getAllNodes([node]);
      expect(result).toHaveLength(1);
      expect(result[0]).toBe(node);
    });

    it('should traverse forward edges (nextNodes)', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node1.nextNodes = [node2];
      node2.nextNodes = [node3];

      const result = getAllNodes([node1]);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });

    it('should traverse backward edges (primaryInput)', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node3.primaryInput = node2;
      node2.primaryInput = node1;

      const result = getAllNodes([node3]);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });

    it('should traverse backward edges (secondaryInputs)', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node3.secondaryInputs = {
        connections: new Map([
          [0, node1],
          [1, node2],
        ]),
        min: 2,
        max: 'unbounded',
        portNames: (portIndex: number) => `Input ${portIndex}`,
      };

      const result = getAllNodes([node3]);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });

    it('should handle cycles without infinite loop', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();

      node1.nextNodes = [node2];
      node2.nextNodes = [node1]; // Cycle

      const result = getAllNodes([node1]);
      expect(result).toHaveLength(2);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
    });

    it('should deduplicate nodes in complex graph', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      // Diamond pattern: 1 -> 2, 1 -> 3, 2 -> 4, 3 -> 4
      const node4 = createTestNode();
      node1.nextNodes = [node2, node3];
      node2.nextNodes = [node4];
      node3.nextNodes = [node4];

      const result = getAllNodes([node1]);
      expect(result).toHaveLength(4);
      // node4 should only appear once
      const node4Count = result.filter((n) => n === node4).length;
      expect(node4Count).toBe(1);
    });

    it('should handle multiple root nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      const result = getAllNodes([node1, node2, node3]);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });
  });

  describe('getAllDownstreamNodes', () => {
    it('should return only the node itself if no children', () => {
      const node = createTestNode();
      const result = getAllDownstreamNodes(node);
      expect(result).toHaveLength(1);
      expect(result[0]).toBe(node);
    });

    it('should return all downstream nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node1.nextNodes = [node2];
      node2.nextNodes = [node3];

      const result = getAllDownstreamNodes(node1);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });

    it('should not traverse backward edges', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node2.primaryInput = node1;
      node2.nextNodes = [node3];

      const result = getAllDownstreamNodes(node2);
      expect(result).toHaveLength(2);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
      expect(result).not.toContain(node1); // Should not go backwards
    });

    it('should handle cycles', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();

      node1.nextNodes = [node2];
      node2.nextNodes = [node1];

      const result = getAllDownstreamNodes(node1);
      expect(result).toHaveLength(2);
    });
  });

  describe('getAllUpstreamNodes', () => {
    it('should return empty array if no inputs', () => {
      const node = createTestNode();
      const result = getAllUpstreamNodes(node);
      expect(result).toEqual([]);
    });

    it('should return all upstream nodes via primaryInput', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node3.primaryInput = node2;
      node2.primaryInput = node1;

      const result = getAllUpstreamNodes(node3);
      expect(result).toHaveLength(2);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).not.toContain(node3); // Should not include starting node
    });

    it('should return all upstream nodes via secondaryInputs', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node3.secondaryInputs = {
        connections: new Map([
          [0, node1],
          [1, node2],
        ]),
        min: 2,
        max: 'unbounded',
        portNames: (portIndex: number) => `Input ${portIndex}`,
      };

      const result = getAllUpstreamNodes(node3);
      expect(result).toHaveLength(2);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).not.toContain(node3);
    });

    it('should not traverse forward edges', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node2.primaryInput = node1;
      node2.nextNodes = [node3];

      const result = getAllUpstreamNodes(node2);
      expect(result).toHaveLength(1);
      expect(result).toContain(node1);
      expect(result).not.toContain(node3); // Should not go forward
    });

    it('should handle complex upstream graph', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();
      const node4 = createTestNode();

      // node4 depends on node2 and node3, which both depend on node1
      node4.secondaryInputs = {
        connections: new Map([
          [0, node2],
          [1, node3],
        ]),
        min: 2,
        max: 'unbounded',
        portNames: (portIndex: number) => `Input ${portIndex}`,
      };
      node2.primaryInput = node1;
      node3.primaryInput = node1;

      const result = getAllUpstreamNodes(node4);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });
  });

  describe('findNodeById', () => {
    it('should return undefined for empty root nodes', () => {
      const result = findNodeById('1', []);
      expect(result).toBeUndefined();
    });

    it('should find node in root nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();

      const result = findNodeById(node1.nodeId, [node1, node2]);
      expect(result).toBe(node1);
    });

    it('should find node in downstream nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node1.nextNodes = [node2];
      node2.nextNodes = [node3];

      const result = findNodeById(node3.nodeId, [node1]);
      expect(result).toBe(node3);
    });

    it('should find node in upstream nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node3.primaryInput = node2;
      node2.primaryInput = node1;

      const result = findNodeById(node1.nodeId, [node3]);
      expect(result).toBe(node1);
    });

    it('should return undefined if node not found', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();

      const result = findNodeById('999', [node1, node2]);
      expect(result).toBeUndefined();
    });
  });

  describe('wouldCreateCycle', () => {
    it('should detect self-loop', () => {
      const node = createTestNode();
      expect(wouldCreateCycle(node, node)).toBe(true);
    });

    it('should detect direct cycle between two nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      // node1 -> node2 already exists
      node1.nextNodes = [node2];
      // Connecting node2 -> node1 would create a cycle
      expect(wouldCreateCycle(node2, node1)).toBe(true);
    });

    it('should detect indirect cycle through chain', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();
      // node1 -> node2 -> node3
      node1.nextNodes = [node2];
      node2.nextNodes = [node3];
      // Connecting node3 -> node1 would create a cycle
      expect(wouldCreateCycle(node3, node1)).toBe(true);
    });

    it('should allow valid connection with no cycle', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();
      // node1 -> node2
      node1.nextNodes = [node2];
      // Connecting node2 -> node3 is fine
      expect(wouldCreateCycle(node2, node3)).toBe(false);
    });

    it('should allow connection between unrelated nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      expect(wouldCreateCycle(node1, node2)).toBe(false);
    });
  });

  describe('findDockedChildren', () => {
    it('should return empty array when parent has no children', () => {
      const parent = createMockNode({nodeId: 'parent', type: NodeType.kTable});
      const nodeLayouts = new Map<string, {x: number; y: number}>();

      const result = findDockedChildren(parent, nodeLayouts);
      expect(result).toEqual([]);
    });

    it('should return docked child when child has no layout', () => {
      const parent = createMockNode({nodeId: 'parent', type: NodeType.kTable});
      // Create a modification node (single-node operation)
      const child = createMockNode({nodeId: 'child', type: NodeType.kFilter});
      child.primaryInput = parent;

      // Setup: child is connected to parent via primaryInput
      parent.nextNodes = [child];

      // Child has no layout (docked)
      const nodeLayouts = new Map<string, {x: number; y: number}>();
      nodeLayouts.set(parent.nodeId, {x: 100, y: 100});

      const result = findDockedChildren(parent, nodeLayouts);
      expect(result).toHaveLength(1);
      expect(result[0]).toBe(child);
    });

    it('should not return child when child has layout (undocked)', () => {
      const parent = createMockNode({nodeId: 'parent', type: NodeType.kTable});
      const child = createMockNode({nodeId: 'child', type: NodeType.kFilter});
      child.primaryInput = parent;

      // Setup: child is connected to parent
      parent.nextNodes = [child];

      // Both have layouts (child is undocked)
      const nodeLayouts = new Map<string, {x: number; y: number}>();
      nodeLayouts.set(parent.nodeId, {x: 100, y: 100});
      nodeLayouts.set(child.nodeId, {x: 200, y: 200});

      const result = findDockedChildren(parent, nodeLayouts);
      expect(result).toEqual([]);
    });

    it('should identify docked vs undocked children correctly', () => {
      const parent = createMockNode({nodeId: 'parent', type: NodeType.kTable});
      // Create modification nodes (single-node operations)
      const child1 = createMockNode({nodeId: 'child1', type: NodeType.kFilter});
      const child2 = createMockNode({nodeId: 'child2', type: NodeType.kSort});

      child1.primaryInput = parent;
      child2.primaryInput = parent;

      // Setup: both children are connected to parent
      parent.nextNodes = [child1, child2];

      // child1 is docked (no layout), child2 is undocked (has layout)
      const nodeLayouts = new Map<string, {x: number; y: number}>();
      nodeLayouts.set(parent.nodeId, {x: 100, y: 100});
      nodeLayouts.set(child2.nodeId, {x: 250, y: 130});
      // child1 has no layout (docked)

      // Only child1 should be identified as docked
      const result = findDockedChildren(parent, nodeLayouts);
      expect(result).toHaveLength(1);
      expect(result).toContain(child1);
      expect(result).not.toContain(child2);
    });
  });

  describe('createGroupFromSelection', () => {
    it('should reject fewer than 2 nodes', () => {
      const node = createMockNode({nodeId: 'a'});
      const result = createGroupFromSelection(new Set(['a']), [node]);
      expect(result.ok).toBe(false);
      if (!result.ok) {
        expect(result.error).toContain('at least 2 nodes');
      }
    });

    it('should create group from a simple chain A→B', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(a, b);

      const result = createGroupFromSelection(new Set(['a', 'b']), [a, b]);
      expect(result.ok).toBe(true);
      if (!result.ok) return;

      const group = result.value;
      expect(group).toBeInstanceOf(GroupNode);
      expect(group.innerNodes).toHaveLength(2);
      expect(group.endNode).toBe(b);
      expect(group.nextNodes).toHaveLength(0);
    });

    it('should identify external connections', () => {
      const ext = createMockNode({nodeId: 'ext'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(ext, a);
      connectNodes(a, b);

      const result = createGroupFromSelection(new Set(['a', 'b']), [ext, a, b]);
      expect(result.ok).toBe(true);
      if (!result.ok) return;

      const group = result.value;
      expect(group.externalConnections).toHaveLength(1);
      expect(group.externalConnections[0].sourceNode).toBe(ext);
      expect(group.externalConnections[0].innerTargetNode).toBe(a);
    });

    it('should set outer nodes as nextNodes on the group', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      const outer = createMockNode({nodeId: 'outer'});
      connectNodes(a, b);
      connectNodes(b, outer);

      const result = createGroupFromSelection(new Set(['a', 'b']), [
        a,
        b,
        outer,
      ]);
      expect(result.ok).toBe(true);
      if (!result.ok) return;

      const group = result.value;
      expect(group.nextNodes).toHaveLength(1);
      expect(group.nextNodes[0]).toBe(outer);
    });

    it('should reject multiple output nodes', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      // a and b are disconnected — both are end nodes
      const result = createGroupFromSelection(new Set(['a', 'b']), [a, b]);
      expect(result.ok).toBe(false);
      if (!result.ok) {
        expect(result.error).toContain('output nodes');
      }
    });

    it('should reject selection containing an existing group node', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(a, b);

      // Create a group from a→b
      const groupResult = createGroupFromSelection(new Set(['a', 'b']), [a, b]);
      expect(groupResult.ok).toBe(true);
      if (!groupResult.ok) return;
      const group = groupResult.value;
      applyGroupRewiring(group);

      // Try to group the group node with another node
      const c = createMockNode({nodeId: 'c'});
      connectNodes(group, c);

      const result = createGroupFromSelection(new Set([group.nodeId, 'c']), [
        group,
        c,
      ]);
      expect(result.ok).toBe(false);
      if (!result.ok) {
        expect(result.error).toContain('existing group');
      }
    });
  });

  describe('applyGroupRewiring', () => {
    it('should rewire external source to point to group node', () => {
      const ext = createMockNode({nodeId: 'ext'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(ext, a);
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [ext, a, b]),
      );

      applyGroupRewiring(group);

      // ext should now point to group, not a
      expect(ext.nextNodes).toContain(group);
      expect(ext.nextNodes).not.toContain(a);
    });

    it('should rewire outer node primaryInput to group node', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      const outer = createMockNode({nodeId: 'outer'});
      connectNodes(a, b);
      connectNodes(b, outer);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [a, b, outer]),
      );

      applyGroupRewiring(group);

      // outer's primaryInput should now be group, not b
      expect(outer.primaryInput).toBe(group);
      // endNode (b) should no longer have outer in nextNodes
      expect(b.nextNodes).not.toContain(outer);
    });

    it('should handle full chain: ext→A→B→outer', () => {
      const ext = createMockNode({nodeId: 'ext'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      const outer = createMockNode({nodeId: 'outer'});
      connectNodes(ext, a);
      connectNodes(a, b);
      connectNodes(b, outer);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [ext, a, b, outer]),
      );

      applyGroupRewiring(group);

      // ext → group → outer
      expect(ext.nextNodes).toEqual([group]);
      expect(group.nextNodes).toEqual([outer]);
      expect(outer.primaryInput).toBe(group);
      // Inner connections preserved for SQL generation
      expect(b.primaryInput).toBe(a);
    });
  });

  describe('ungroupNode', () => {
    it('should restore external source connections', () => {
      const ext = createMockNode({nodeId: 'ext'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(ext, a);
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [ext, a, b]),
      );
      applyGroupRewiring(group);

      // After grouping: ext → group
      expect(ext.nextNodes).toContain(group);
      expect(ext.nextNodes).not.toContain(a);

      ungroupNode(group);

      // After ungrouping: ext → a (restored)
      expect(ext.nextNodes).toContain(a);
      expect(ext.nextNodes).not.toContain(group);
    });

    it('should restore outer node primaryInput to endNode', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      const outer = createMockNode({nodeId: 'outer'});
      connectNodes(a, b);
      connectNodes(b, outer);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [a, b, outer]),
      );
      applyGroupRewiring(group);

      expect(outer.primaryInput).toBe(group);

      ungroupNode(group);

      expect(outer.primaryInput).toBe(b);
      expect(b.nextNodes).toContain(outer);
    });

    it('should deep clone inner nodes independently', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [a, b]),
      );

      const cloned = group.clone();

      // Cloned group has a different nodeId
      expect(cloned.nodeId).not.toBe(group.nodeId);

      // Inner nodes are deep-cloned — different object references
      expect(cloned.innerNodes).toHaveLength(2);
      for (const clonedInner of cloned.innerNodes) {
        expect(group.innerNodes.find((n) => n === clonedInner)).toBeUndefined();
      }

      // End node is also remapped to a cloned inner node
      expect(cloned.endNode).toBeDefined();
      expect(cloned.endNode).not.toBe(group.endNode);
      expect(cloned.innerNodes).toContain(cloned.endNode);

      // Modifying cloned group does not affect original
      cloned.innerNodes.pop();
      expect(group.innerNodes).toHaveLength(2);
    });

    it('should fully reverse group+ungroup: ext→A→B→outer', () => {
      const ext = createMockNode({nodeId: 'ext'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      const outer = createMockNode({nodeId: 'outer'});
      connectNodes(ext, a);
      connectNodes(a, b);
      connectNodes(b, outer);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [ext, a, b, outer]),
      );
      applyGroupRewiring(group);

      ungroupNode(group);

      // Original connections restored
      expect(ext.nextNodes).toEqual([a]);
      expect(a.primaryInput).toBe(ext);
      expect(b.nextNodes).toContain(outer);
      expect(outer.primaryInput).toBe(b);
      // Inner connection preserved
      expect(b.primaryInput).toBe(a);
    });
  });

  describe('createGroupFromSelection - edge cases', () => {
    it('should reject empty selection', () => {
      const result = createGroupFromSelection(new Set(), []);
      expect(result.ok).toBe(false);
    });

    it('should reject single node selection', () => {
      const a = createMockNode({nodeId: 'a'});
      const result = createGroupFromSelection(new Set(['a']), [a]);
      expect(result.ok).toBe(false);
    });

    it('should handle 3-node chain A→B→C', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      const c = createMockNode({nodeId: 'c'});
      connectNodes(a, b);
      connectNodes(b, c);

      const result = createGroupFromSelection(new Set(['a', 'b', 'c']), [
        a,
        b,
        c,
      ]);
      expect(result.ok).toBe(true);
      if (!result.ok) return;

      expect(result.value.innerNodes).toHaveLength(3);
      expect(result.value.endNode).toBe(c);
      expect(result.value.externalConnections).toHaveLength(0);
    });

    it('should handle grouping middle of chain: ext→A→B→C→outer', () => {
      const ext = createMockNode({nodeId: 'ext'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      const c = createMockNode({nodeId: 'c'});
      const outer = createMockNode({nodeId: 'outer'});
      connectNodes(ext, a);
      connectNodes(a, b);
      connectNodes(b, c);
      connectNodes(c, outer);

      // Group only B and C
      const result = createGroupFromSelection(new Set(['b', 'c']), [
        ext,
        a,
        b,
        c,
        outer,
      ]);
      expect(result.ok).toBe(true);
      if (!result.ok) return;

      const group = result.value;
      expect(group.innerNodes).toHaveLength(2);
      expect(group.endNode).toBe(c);
      // A feeds into B from outside
      expect(group.externalConnections).toHaveLength(1);
      expect(group.externalConnections[0].sourceNode).toBe(a);
      expect(group.externalConnections[0].innerTargetNode).toBe(b);
      // outer is the successor
      expect(group.nextNodes).toHaveLength(1);
      expect(group.nextNodes[0]).toBe(outer);
    });

    it('should handle multiple external sources into the group', () => {
      const ext1 = createMockNode({nodeId: 'ext1'});
      const ext2 = createMockNode({nodeId: 'ext2'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({
        nodeId: 'b',
        secondaryInputs: createUnboundedSecondaryInputs(),
      });
      connectNodes(ext1, a);
      connectNodes(a, b);
      connectSecondary(ext2, b, 0);

      const result = createGroupFromSelection(new Set(['a', 'b']), [
        ext1,
        ext2,
        a,
        b,
      ]);
      expect(result.ok).toBe(true);
      if (!result.ok) return;

      const group = result.value;
      // Two external connections: ext1→a (primary), ext2→b (secondary)
      expect(group.externalConnections).toHaveLength(2);

      const sources = group.externalConnections.map((c) => c.sourceNode);
      expect(sources).toContain(ext1);
      expect(sources).toContain(ext2);
    });

    it('should ignore nodes not in allNodes when selecting', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(a, b);

      // Select a, b, and a phantom 'x' that is not in allNodes
      const result = createGroupFromSelection(new Set(['a', 'b', 'x']), [a, b]);
      expect(result.ok).toBe(true);
      if (!result.ok) return;

      expect(result.value.innerNodes).toHaveLength(2);
    });
  });

  describe('applyGroupRewiring - edge cases', () => {
    it('should be a no-op when endNode is undefined', () => {
      const group = new GroupNode([], undefined, []);
      // Should not throw
      applyGroupRewiring(group);
    });

    it('should deduplicate source→group connections', () => {
      // When one external source feeds into two inner nodes,
      // it should only appear once in source.nextNodes after rewiring.
      const ext = createMockNode({nodeId: 'ext'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({
        nodeId: 'b',
        secondaryInputs: createUnboundedSecondaryInputs(),
      });
      connectNodes(ext, a);
      connectNodes(a, b);
      connectSecondary(ext, b, 0);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [ext, a, b]),
      );
      applyGroupRewiring(group);

      // ext should point to group exactly once
      const groupCount = ext.nextNodes.filter((n) => n === group).length;
      expect(groupCount).toBe(1);
    });
  });

  describe('ungroupNode - edge cases', () => {
    it('should handle group with no external connections', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [a, b]),
      );
      applyGroupRewiring(group);

      ungroupNode(group);

      // Inner connections should be preserved
      expect(b.primaryInput).toBe(a);
      expect(a.nextNodes).toContain(b);
    });

    it('should handle group with undefined endNode gracefully', () => {
      const a = createMockNode({nodeId: 'a'});

      const group = new GroupNode([a], undefined, []);

      // Should not throw
      ungroupNode(group);
    });

    it('should handle multiple outer nodes', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      const outer1 = createMockNode({nodeId: 'outer1'});
      const outer2 = createMockNode({nodeId: 'outer2'});
      connectNodes(a, b);
      connectNodes(b, outer1);
      connectNodes(b, outer2);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [a, b, outer1, outer2]),
      );
      applyGroupRewiring(group);

      expect(outer1.primaryInput).toBe(group);
      expect(outer2.primaryInput).toBe(group);

      ungroupNode(group);

      expect(outer1.primaryInput).toBe(b);
      expect(outer2.primaryInput).toBe(b);
      expect(b.nextNodes).toContain(outer1);
      expect(b.nextNodes).toContain(outer2);
    });
  });

  describe('getAllNodes with groups', () => {
    it('should include grouped inner nodes in traversal', () => {
      const ext = createMockNode({nodeId: 'ext'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(ext, a);
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [ext, a, b]),
      );
      applyGroupRewiring(group);

      // Inner nodes are NOT in rootNodes — they are discovered through
      // GroupNode.innerNodes traversal inside getAllNodes.
      const rootNodes = [ext, group];
      const allNodes = getAllNodes(rootNodes);

      const nodeIds = allNodes.map((n) => n.nodeId);
      expect(nodeIds).toContain('ext');
      expect(nodeIds).toContain('a');
      expect(nodeIds).toContain('b');
      expect(nodeIds).toContain(group.nodeId);
    });

    it('should exclude inner nodes with traverseGroups: false', () => {
      const ext = createMockNode({nodeId: 'ext'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(ext, a);
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [ext, a, b]),
      );
      applyGroupRewiring(group);

      const rootNodes = [ext, group];
      const allNodes = getAllNodes(rootNodes, {traverseGroups: false});

      const nodeIds = allNodes.map((n) => n.nodeId);
      expect(nodeIds).toContain('ext');
      expect(nodeIds).toContain(group.nodeId);
      expect(nodeIds).not.toContain('a');
      expect(nodeIds).not.toContain('b');
    });
  });
});

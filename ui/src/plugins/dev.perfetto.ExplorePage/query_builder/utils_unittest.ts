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
  isMultiSourceNode,
  findOverlappingNode,
  isOverlapping,
  findBlockOverlap,
  isOverlappingBottomPort,
} from './utils';
import {QueryNode, NodeType} from '../query_node';
import {NodeContainerLayout} from './graph/node_container';

describe('query_builder utils', () => {
  function createMockNode(nodeId: string, prevNodes?: QueryNode[]): QueryNode {
    const node: Partial<QueryNode> & {prevNodes?: QueryNode[]} = {
      nodeId,
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: [],
      state: {},
      validate: () => true,
      getTitle: () => 'Test',
      nodeSpecificModify: () => null,
      getStructuredQuery: () => undefined,
      serializeState: () => ({}),
    };
    node.clone = () => node as QueryNode;
    if (prevNodes !== undefined) {
      node.prevNodes = prevNodes;
    }
    return node as QueryNode;
  }

  describe('isMultiSourceNode', () => {
    it('should return true for node with prevNodes array', () => {
      const node = createMockNode('node1', []);

      expect(isMultiSourceNode(node)).toBe(true);
    });

    it('should return false for node without prevNodes', () => {
      const node = createMockNode('node1');

      expect(isMultiSourceNode(node)).toBe(false);
    });

    it('should return true even for empty prevNodes array', () => {
      const node = createMockNode('node1', []);

      expect(isMultiSourceNode(node)).toBe(true);
    });

    it('should return true for node with multiple prevNodes', () => {
      const prevNode1 = createMockNode('prev1');
      const prevNode2 = createMockNode('prev2');
      const node = createMockNode('node1', [prevNode1, prevNode2]);

      expect(isMultiSourceNode(node)).toBe(true);
    });
  });

  describe('isOverlapping', () => {
    it('should return true for overlapping layouts', () => {
      const layout1: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };
      const layout2: NodeContainerLayout = {
        x: 50,
        y: 50,
        width: 100,
        height: 100,
      };

      expect(isOverlapping(layout1, layout2, 0)).toBe(true);
    });

    it('should return false for non-overlapping layouts', () => {
      const layout1: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };
      const layout2: NodeContainerLayout = {
        x: 150,
        y: 150,
        width: 100,
        height: 100,
      };

      expect(isOverlapping(layout1, layout2, 0)).toBe(false);
    });

    it('should return true for layouts touching at edge (no padding)', () => {
      const layout1: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };
      const layout2: NodeContainerLayout = {
        x: 100,
        y: 0,
        width: 100,
        height: 100,
      };

      // Touching at x=100, so they don't overlap
      expect(isOverlapping(layout1, layout2, 0)).toBe(false);
    });

    it('should return false when applying padding separates them', () => {
      const layout1: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };
      const layout2: NodeContainerLayout = {
        x: 110,
        y: 0,
        width: 100,
        height: 100,
      };

      expect(isOverlapping(layout1, layout2, 5)).toBe(false);
    });

    it('should return true when padding causes overlap', () => {
      const layout1: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };
      const layout2: NodeContainerLayout = {
        x: 105,
        y: 0,
        width: 100,
        height: 100,
      };

      expect(isOverlapping(layout1, layout2, 10)).toBe(true);
    });

    it('should handle layouts without width/height (defaults to 0)', () => {
      const layout1: NodeContainerLayout = {x: 0, y: 0};
      const layout2: NodeContainerLayout = {x: 0, y: 0};

      expect(isOverlapping(layout1, layout2, 0)).toBe(false);
    });

    it('should detect vertical overlap only', () => {
      const layout1: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };
      const layout2: NodeContainerLayout = {
        x: 50,
        y: 150,
        width: 100,
        height: 100,
      };

      expect(isOverlapping(layout1, layout2, 0)).toBe(false);
    });

    it('should detect horizontal overlap only', () => {
      const layout1: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };
      const layout2: NodeContainerLayout = {
        x: 150,
        y: 50,
        width: 100,
        height: 100,
      };

      expect(isOverlapping(layout1, layout2, 0)).toBe(false);
    });

    it('should handle complete containment', () => {
      const layout1: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 200,
        height: 200,
      };
      const layout2: NodeContainerLayout = {
        x: 50,
        y: 50,
        width: 50,
        height: 50,
      };

      expect(isOverlapping(layout1, layout2, 0)).toBe(true);
    });
  });

  describe('findOverlappingNode', () => {
    it('should return overlapping node', () => {
      const node1 = createMockNode('node1');
      const node2 = createMockNode('node2');
      const node3 = createMockNode('node3');

      const dragLayout: NodeContainerLayout = {
        x: 50,
        y: 50,
        width: 100,
        height: 100,
      };
      const layouts = new Map<QueryNode, NodeContainerLayout>([
        [node1, {x: 0, y: 0, width: 100, height: 100}],
        [node2, {x: 200, y: 200, width: 100, height: 100}],
        [node3, {x: 75, y: 75, width: 100, height: 100}],
      ]);

      const result = findOverlappingNode(dragLayout, layouts, node2);

      // node1 overlaps with dragLayout (returns first overlapping node found)
      expect(result).toBe(node1);
    });

    it('should return undefined when no overlap', () => {
      const node1 = createMockNode('node1');
      const node2 = createMockNode('node2');

      const dragLayout: NodeContainerLayout = {
        x: 300,
        y: 300,
        width: 100,
        height: 100,
      };
      const layouts = new Map<QueryNode, NodeContainerLayout>([
        [node1, {x: 0, y: 0, width: 100, height: 100}],
        [node2, {x: 500, y: 500, width: 100, height: 100}],
      ]);

      const result = findOverlappingNode(dragLayout, layouts, node1);

      expect(result).toBeUndefined();
    });

    it('should exclude the drag node itself', () => {
      const dragNode = createMockNode('drag');
      const node2 = createMockNode('node2');

      const dragLayout: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };
      const layouts = new Map<QueryNode, NodeContainerLayout>([
        [dragNode, {x: 0, y: 0, width: 100, height: 100}],
        [node2, {x: 200, y: 200, width: 100, height: 100}],
      ]);

      const result = findOverlappingNode(dragLayout, layouts, dragNode);

      expect(result).toBeUndefined();
    });

    it('should return first overlapping node found', () => {
      const node1 = createMockNode('node1');
      const node2 = createMockNode('node2');
      const node3 = createMockNode('node3');

      const dragLayout: NodeContainerLayout = {
        x: 50,
        y: 50,
        width: 100,
        height: 100,
      };
      const layouts = new Map<QueryNode, NodeContainerLayout>([
        [node1, {x: 60, y: 60, width: 100, height: 100}],
        [node2, {x: 70, y: 70, width: 100, height: 100}],
        [node3, {x: 200, y: 200, width: 100, height: 100}],
      ]);

      const result = findOverlappingNode(dragLayout, layouts, node3);

      // Should return one of the overlapping nodes
      expect(result).toBeDefined();
      expect([node1, node2]).toContain(result);
    });
  });

  describe('findBlockOverlap', () => {
    it('should return overlapping node outside the block', () => {
      const blockNode1 = createMockNode('block1');
      const blockNode2 = createMockNode('block2');
      const outsideNode = createMockNode('outside');

      const layouts = new Map<QueryNode, NodeContainerLayout>([
        [blockNode1, {x: 0, y: 0, width: 100, height: 100}],
        [blockNode2, {x: 150, y: 0, width: 100, height: 100}],
        [outsideNode, {x: 50, y: 50, width: 100, height: 100}],
      ]);

      const result = findBlockOverlap([blockNode1, blockNode2], layouts);

      expect(result).toBe(outsideNode);
    });

    it('should return undefined when no overlap exists', () => {
      const blockNode1 = createMockNode('block1');
      const blockNode2 = createMockNode('block2');
      const outsideNode = createMockNode('outside');

      const layouts = new Map<QueryNode, NodeContainerLayout>([
        [blockNode1, {x: 0, y: 0, width: 100, height: 100}],
        [blockNode2, {x: 150, y: 0, width: 100, height: 100}],
        [outsideNode, {x: 300, y: 300, width: 100, height: 100}],
      ]);

      const result = findBlockOverlap([blockNode1, blockNode2], layouts);

      expect(result).toBeUndefined();
    });

    it('should ignore overlap within the block', () => {
      const blockNode1 = createMockNode('block1');
      const blockNode2 = createMockNode('block2');

      const layouts = new Map<QueryNode, NodeContainerLayout>([
        [blockNode1, {x: 0, y: 0, width: 100, height: 100}],
        [blockNode2, {x: 50, y: 50, width: 100, height: 100}],
      ]);

      const result = findBlockOverlap([blockNode1, blockNode2], layouts);

      expect(result).toBeUndefined();
    });

    it('should handle empty block', () => {
      const outsideNode = createMockNode('outside');

      const layouts = new Map<QueryNode, NodeContainerLayout>([
        [outsideNode, {x: 0, y: 0, width: 100, height: 100}],
      ]);

      const result = findBlockOverlap([], layouts);

      expect(result).toBeUndefined();
    });

    it('should handle node in block without layout', () => {
      const blockNode = createMockNode('block1');
      const outsideNode = createMockNode('outside');

      const layouts = new Map<QueryNode, NodeContainerLayout>([
        [outsideNode, {x: 0, y: 0, width: 100, height: 100}],
      ]);

      const result = findBlockOverlap([blockNode], layouts);

      expect(result).toBeUndefined();
    });
  });

  describe('isOverlappingBottomPort', () => {
    it('should return true when drag node overlaps bottom port', () => {
      const dragLayout: NodeContainerLayout = {
        x: 45,
        y: 95,
        width: 10,
        height: 10,
      };
      const targetLayout: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };

      // Bottom port is at (50, 100) - center bottom of target
      // Drag node at (45, 95) with size 10x10 covers (45-55, 95-105)
      expect(isOverlappingBottomPort(dragLayout, targetLayout, 5)).toBe(true);
    });

    it('should return false when drag node does not overlap bottom port', () => {
      const dragLayout: NodeContainerLayout = {
        x: 200,
        y: 200,
        width: 10,
        height: 10,
      };
      const targetLayout: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };

      expect(isOverlappingBottomPort(dragLayout, targetLayout, 5)).toBe(false);
    });

    it('should handle padding correctly', () => {
      const dragLayout: NodeContainerLayout = {
        x: 40,
        y: 90,
        width: 20,
        height: 20,
      };
      const targetLayout: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };

      // Bottom port at (50, 100)
      // With padding=10, port detection area is (40-60, 90-110)
      // Drag node covers (40-60, 90-110) - perfect match
      expect(isOverlappingBottomPort(dragLayout, targetLayout, 10)).toBe(true);
    });

    it('should return false when only horizontally aligned', () => {
      const dragLayout: NodeContainerLayout = {
        x: 45,
        y: 200,
        width: 10,
        height: 10,
      };
      const targetLayout: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };

      expect(isOverlappingBottomPort(dragLayout, targetLayout, 5)).toBe(false);
    });

    it('should return false when only vertically aligned', () => {
      const dragLayout: NodeContainerLayout = {
        x: 200,
        y: 95,
        width: 10,
        height: 10,
      };
      const targetLayout: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };

      expect(isOverlappingBottomPort(dragLayout, targetLayout, 5)).toBe(false);
    });

    it('should handle layouts without width/height', () => {
      const dragLayout: NodeContainerLayout = {x: 50, y: 100};
      const targetLayout: NodeContainerLayout = {x: 0, y: 0};

      // Bottom port at (0, 0), drag node at (50, 100) - no overlap
      expect(isOverlappingBottomPort(dragLayout, targetLayout, 5)).toBe(false);
    });

    it('should detect overlap with zero padding', () => {
      const dragLayout: NodeContainerLayout = {
        x: 49,
        y: 99,
        width: 2,
        height: 2,
      };
      const targetLayout: NodeContainerLayout = {
        x: 0,
        y: 0,
        width: 100,
        height: 100,
      };

      // Bottom port at (50, 100)
      // Drag node covers (49-51, 99-101)
      expect(isOverlappingBottomPort(dragLayout, targetLayout, 0)).toBe(true);
    });
  });
});

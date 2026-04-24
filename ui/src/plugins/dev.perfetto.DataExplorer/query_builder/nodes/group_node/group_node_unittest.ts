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

import {GroupNode, ExternalGroupConnection} from '.';
import {NodeType} from '../../../query_node';
import {
  createMockNode,
  connectNodes,
  STANDARD_TABLE_COLUMNS,
} from '../../testing/test_utils';
import {createGroupFromSelection} from '../../graph_utils';
import {unwrapResult} from '../../../../../base/result';

describe('GroupNode', () => {
  describe('constructor', () => {
    it('should set type to kGroup', () => {
      const group = new GroupNode({}, {}, [], undefined, []);
      expect(group.type).toBe(NodeType.kGroup);
    });

    it('should use default name "Group"', () => {
      const group = new GroupNode({}, {}, [], undefined, []);
      expect(group.attrs.name).toBe('Group');
    });

    it('should accept a custom name', () => {
      const group = new GroupNode({name: 'My Group'}, {}, [], undefined, []);
      expect(group.attrs.name).toBe('My Group');
    });

    it('should populate secondaryInputs from external connections', () => {
      const ext = createMockNode({nodeId: 'ext'});
      const inner = createMockNode({nodeId: 'inner'});

      const conns: ExternalGroupConnection[] = [
        {
          sourceNode: ext,
          innerTargetNode: inner,
          innerTargetPort: undefined,
          groupPort: 0,
        },
      ];

      const group = new GroupNode({}, {}, [inner], inner, conns);
      expect(group.secondaryInputs.connections.size).toBe(1);
      expect(group.secondaryInputs.connections.get(0)).toBe(ext);
    });
  });

  describe('finalCols', () => {
    it('should delegate to endNode finalCols', () => {
      const cols = STANDARD_TABLE_COLUMNS();
      const endNode = createMockNode({nodeId: 'end', columns: cols});
      const group = new GroupNode({}, {}, [endNode], endNode, []);

      expect(group.finalCols).toEqual(cols);
    });

    it('should return empty array when endNode is undefined', () => {
      const group = new GroupNode({}, {}, [], undefined, []);
      expect(group.finalCols).toEqual([]);
    });
  });

  describe('validate', () => {
    it('should return false when innerNodes is empty', () => {
      const group = new GroupNode(
        {},
        {},
        [],
        createMockNode({nodeId: 'end'}),
        [],
      );
      expect(group.validate()).toBe(false);
    });

    it('should return false when endNode is undefined', () => {
      const inner = createMockNode({nodeId: 'a'});
      const group = new GroupNode({}, {}, [inner], undefined, []);
      expect(group.validate()).toBe(false);
    });

    it('should return true when all inner nodes validate', () => {
      const a = createMockNode({nodeId: 'a', validate: () => true});
      const b = createMockNode({nodeId: 'b', validate: () => true});
      const group = new GroupNode({}, {}, [a, b], b, []);
      expect(group.validate()).toBe(true);
    });

    it('should return false when any inner node fails validation', () => {
      const a = createMockNode({nodeId: 'a', validate: () => true});
      const b = createMockNode({nodeId: 'b', validate: () => false});
      const group = new GroupNode({}, {}, [a, b], b, []);
      expect(group.validate()).toBe(false);
    });
  });

  describe('getTitle', () => {
    it('should return the group name', () => {
      const group = new GroupNode({name: 'Test Group'}, {}, [], undefined, []);
      expect(group.getTitle()).toBe('Test Group');
    });

    it('should reflect name changes', () => {
      const group = new GroupNode({}, {}, [], undefined, []);
      group.attrs.name = 'Renamed';
      expect(group.getTitle()).toBe('Renamed');
    });
  });

  describe('attrs', () => {
    it('should store name in attrs', () => {
      const inner = createMockNode({nodeId: 'a'});
      const group = new GroupNode(
        {name: 'Custom Name'},
        {},
        [inner],
        inner,
        [],
      );
      expect(group.attrs).toEqual({name: 'Custom Name'});
    });
  });

  describe('getStructuredQuery', () => {
    it('should return undefined when endNode is undefined', () => {
      const group = new GroupNode({}, {}, [], undefined, []);
      expect(group.getStructuredQuery()).toBeUndefined();
    });

    it('should return passthrough query referencing endNode', () => {
      const endNode = createMockNode({nodeId: 'end'});
      const group = new GroupNode({}, {}, [endNode], endNode, []);
      const sq = group.getStructuredQuery();
      expect(sq).toBeDefined();
      if (sq !== undefined) {
        expect(sq.id).toBe(group.nodeId);
        expect(sq.innerQueryId).toBe('end');
      }
    });
  });

  describe('clone', () => {
    it('should create a new GroupNode with different nodeId', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [a, b]),
      );
      const cloned = group.clone();

      expect(cloned.nodeId).not.toBe(group.nodeId);
      expect(cloned.type).toBe(NodeType.kGroup);
    });

    it('should preserve the group name', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [a, b]),
      );
      group.attrs.name = 'My Custom Group';
      const cloned = group.clone();

      expect(cloned.attrs.name).toBe('My Custom Group');
    });

    it('should deep clone inner nodes', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [a, b]),
      );
      const cloned = group.clone();

      // Inner nodes should be different objects
      expect(cloned.innerNodes).toHaveLength(2);
      for (const clonedInner of cloned.innerNodes) {
        const originalMatch = group.innerNodes.find((n) => n === clonedInner);
        expect(originalMatch).toBeUndefined();
      }
    });

    it('should remap endNode to cloned inner node', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [a, b]),
      );
      const cloned = group.clone();

      expect(cloned.endNode).toBeDefined();
      expect(cloned.endNode).not.toBe(group.endNode);
      expect(cloned.innerNodes).toContain(cloned.endNode);
    });

    it('should remap external connection inner targets to cloned nodes', () => {
      const ext = createMockNode({nodeId: 'ext'});
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(ext, a);
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [ext, a, b]),
      );
      const cloned = group.clone();

      expect(cloned.externalConnections).toHaveLength(1);
      // The inner target should be a cloned node, not the original
      const clonedTarget = cloned.externalConnections[0].innerTargetNode;
      expect(clonedTarget).not.toBe(a);
      expect(cloned.innerNodes).toContain(clonedTarget);
      // The source node should still reference the original external node
      expect(cloned.externalConnections[0].sourceNode).toBe(ext);
    });

    it('should not share state between original and clone', () => {
      const a = createMockNode({nodeId: 'a'});
      const b = createMockNode({nodeId: 'b'});
      connectNodes(a, b);

      const group = unwrapResult(
        createGroupFromSelection(new Set(['a', 'b']), [a, b]),
      );
      const cloned = group.clone();

      // Mutating cloned group should not affect original
      cloned.innerNodes.pop();
      expect(group.innerNodes).toHaveLength(2);

      cloned.attrs.name = 'Changed';
      expect(group.attrs.name).toBe('Group');
    });
  });

  describe('secondaryInputs port names', () => {
    it('should generate port names based on index', () => {
      const group = new GroupNode({}, {}, [], undefined, []);
      const portNames = group.secondaryInputs.portNames;
      if (typeof portNames === 'function') {
        expect(portNames(0)).toBe('Input 1');
        expect(portNames(1)).toBe('Input 2');
        expect(portNames(4)).toBe('Input 5');
      } else {
        // portNames is always a function for GroupNode, fail if not
        expect(typeof portNames).toBe('function');
      }
    });
  });
});

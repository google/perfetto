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

import {NodeType} from './query_node';
import {
  copySelectedNodes,
  pasteClipboardNodes,
  ClipboardEntry,
  ClipboardConnection,
} from './clipboard_operations';
import {createMockNode, connectNodes} from './query_builder/testing/test_utils';
import {TableSourceNode} from './query_builder/nodes/sources/table_source';
import {Trace} from '../../public/trace';
import {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';

describe('clipboard_operations', () => {
  describe('copySelectedNodes', () => {
    it('should return undefined when no nodes are selected', () => {
      const node = createMockNode({nodeId: 'n1'});
      const result = copySelectedNodes({
        rootNodes: [node],
        selectedNodes: new Set(),
        nodeLayouts: new Map(),
      });
      expect(result).toBeUndefined();
    });

    it('should return undefined when selected IDs do not match any nodes', () => {
      const node = createMockNode({nodeId: 'n1'});
      const result = copySelectedNodes({
        rootNodes: [node],
        selectedNodes: new Set(['nonexistent']),
        nodeLayouts: new Map(),
      });
      expect(result).toBeUndefined();
    });

    it('should copy a single selected node with relative position (0,0)', () => {
      const node = createMockNode({nodeId: 'n1'});
      const result = copySelectedNodes({
        rootNodes: [node],
        selectedNodes: new Set(['n1']),
        nodeLayouts: new Map([['n1', {x: 100, y: 200}]]),
      });

      expect(result).toBeDefined();
      expect(result?.clipboardNodes).toHaveLength(1);
      expect(result?.clipboardNodes[0].relativeX).toBe(0);
      expect(result?.clipboardNodes[0].relativeY).toBe(0);
      expect(result?.clipboardNodes[0].isDocked).toBe(false);
    });

    it('should compute relative positions for multiple nodes', () => {
      const node1 = createMockNode({nodeId: 'n1'});
      const node2 = createMockNode({nodeId: 'n2'});
      const result = copySelectedNodes({
        rootNodes: [node1, node2],
        selectedNodes: new Set(['n1', 'n2']),
        nodeLayouts: new Map([
          ['n1', {x: 50, y: 100}],
          ['n2', {x: 250, y: 300}],
        ]),
      });

      expect(result).toBeDefined();
      expect(result?.clipboardNodes).toHaveLength(2);
      // First node is at origin (min x/y)
      expect(result?.clipboardNodes[0].relativeX).toBe(0);
      expect(result?.clipboardNodes[0].relativeY).toBe(0);
      // Second node is offset
      expect(result?.clipboardNodes[1].relativeX).toBe(200);
      expect(result?.clipboardNodes[1].relativeY).toBe(200);
    });

    it('should mark nodes without layout as docked', () => {
      const node1 = createMockNode({nodeId: 'n1'});
      const node2 = createMockNode({nodeId: 'n2'});
      const result = copySelectedNodes({
        rootNodes: [node1, node2],
        selectedNodes: new Set(['n1', 'n2']),
        nodeLayouts: new Map([['n1', {x: 100, y: 100}]]),
        // n2 has no layout → docked
      });

      expect(result).toBeDefined();
      expect(result?.clipboardNodes[0].isDocked).toBe(false);
      expect(result?.clipboardNodes[1].isDocked).toBe(true);
    });

    it('should clone nodes for the clipboard', () => {
      const node = createMockNode({nodeId: 'n1'});
      const result = copySelectedNodes({
        rootNodes: [node],
        selectedNodes: new Set(['n1']),
        nodeLayouts: new Map([['n1', {x: 0, y: 0}]]),
      });

      expect(result).toBeDefined();
      // Cloned node should not be the same object reference
      expect(result?.clipboardNodes[0].node).not.toBe(node);
    });

    it('should capture primaryInput connections between selected nodes', () => {
      const parent = createMockNode({nodeId: 'p', type: NodeType.kTable});
      const child = createMockNode({nodeId: 'c', type: NodeType.kFilter});
      connectNodes(parent, child);

      const result = copySelectedNodes({
        rootNodes: [parent],
        selectedNodes: new Set(['p', 'c']),
        nodeLayouts: new Map([
          ['p', {x: 0, y: 0}],
          ['c', {x: 0, y: 100}],
        ]),
      });

      expect(result).toBeDefined();
      expect(result?.clipboardConnections).toHaveLength(1);
      expect(result?.clipboardConnections[0].fromIndex).toBe(0);
      expect(result?.clipboardConnections[0].toIndex).toBe(1);
      expect(result?.clipboardConnections[0].portIndex).toBeUndefined();
    });

    it('should not capture connections to nodes outside the selection', () => {
      const parent = createMockNode({nodeId: 'p', type: NodeType.kTable});
      const child = createMockNode({nodeId: 'c', type: NodeType.kFilter});
      connectNodes(parent, child);

      // Only select the child, not the parent
      const result = copySelectedNodes({
        rootNodes: [parent],
        selectedNodes: new Set(['c']),
        nodeLayouts: new Map([['c', {x: 0, y: 100}]]),
      });

      expect(result).toBeDefined();
      expect(result?.clipboardConnections).toHaveLength(0);
    });

    it('should capture secondaryInput connections between selected nodes', () => {
      const source = createMockNode({nodeId: 's', type: NodeType.kTable});
      const multi = createMockNode({nodeId: 'm', type: NodeType.kJoin});
      multi.secondaryInputs = {
        connections: new Map([[0, source]]),
        min: 2,
        max: 2,
        portNames: ['Left', 'Right'],
      };
      source.nextNodes = [multi];

      const result = copySelectedNodes({
        rootNodes: [source, multi],
        selectedNodes: new Set(['s', 'm']),
        nodeLayouts: new Map([
          ['s', {x: 0, y: 0}],
          ['m', {x: 100, y: 0}],
        ]),
      });

      expect(result).toBeDefined();
      expect(result?.clipboardConnections).toHaveLength(1);
      expect(result?.clipboardConnections[0].portIndex).toBe(0);
    });
  });

  describe('pasteClipboardNodes', () => {
    it('should return undefined when clipboard is empty', () => {
      const result = pasteClipboardNodes({
        rootNodes: [],
        nodeLayouts: new Map(),
        clipboardNodes: undefined,
      });
      expect(result).toBeUndefined();
    });

    it('should return undefined when clipboard has zero entries', () => {
      const result = pasteClipboardNodes({
        rootNodes: [],
        nodeLayouts: new Map(),
        clipboardNodes: [],
      });
      expect(result).toBeUndefined();
    });

    it('should append cloned nodes to rootNodes', () => {
      const existing = createMockNode({nodeId: 'existing'});
      const clipNode = createMockNode({nodeId: 'clip'});
      const clipboardNodes: ClipboardEntry[] = [
        {node: clipNode, relativeX: 0, relativeY: 0, isDocked: false},
      ];

      const result = pasteClipboardNodes({
        rootNodes: [existing],
        nodeLayouts: new Map(),
        clipboardNodes,
      });

      expect(result).toBeDefined();
      // Original node + pasted node
      expect(result?.rootNodes).toHaveLength(2);
      expect(result?.rootNodes[0]).toBe(existing);
      // Pasted node is cloned (not the same reference)
      expect(result?.rootNodes[1]).not.toBe(clipNode);
    });

    it('should select only the newly pasted nodes', () => {
      const clipNode = createMockNode({nodeId: 'clip'});
      const clipboardNodes: ClipboardEntry[] = [
        {node: clipNode, relativeX: 0, relativeY: 0, isDocked: false},
      ];

      const result = pasteClipboardNodes({
        rootNodes: [],
        nodeLayouts: new Map(),
        clipboardNodes,
      });

      expect(result).toBeDefined();
      expect(result?.selectedNodes.size).toBe(1);
      // The selected ID should be the NEW (cloned) node's ID, not the clipboard node
      const pastedNodeId = result?.rootNodes[0].nodeId;
      expect(result?.selectedNodes.has(pastedNodeId ?? '')).toBe(true);
    });

    it('should add layout positions for undocked nodes with offset', () => {
      const clipNode = createMockNode({nodeId: 'clip'});
      const clipboardNodes: ClipboardEntry[] = [
        {node: clipNode, relativeX: 100, relativeY: 200, isDocked: false},
      ];

      const result = pasteClipboardNodes({
        rootNodes: [],
        nodeLayouts: new Map(),
        clipboardNodes,
      });

      expect(result).toBeDefined();
      const pastedNodeId = result?.rootNodes[0].nodeId ?? '';
      const layout = result?.nodeLayouts.get(pastedNodeId);
      expect(layout).toBeDefined();
      // relativeX + pasteOffsetX (50), relativeY + pasteOffsetY (50)
      expect(layout?.x).toBe(150);
      expect(layout?.y).toBe(250);
    });

    it('should not add layout for docked nodes', () => {
      const clipNode = createMockNode({nodeId: 'clip'});
      const clipboardNodes: ClipboardEntry[] = [
        {node: clipNode, relativeX: 0, relativeY: 0, isDocked: true},
      ];

      const result = pasteClipboardNodes({
        rootNodes: [],
        nodeLayouts: new Map(),
        clipboardNodes,
      });

      expect(result).toBeDefined();
      const pastedNodeId = result?.rootNodes[0].nodeId ?? '';
      expect(result?.nodeLayouts.has(pastedNodeId)).toBe(false);
    });

    it('should allow multiple pastes from the same clipboard', () => {
      // Use real TableSourceNode since its clone() generates new IDs
      const mockTrace = {traceInfo: {traceTitle: 'test'}} as Trace;
      const mockSqlModules = {
        listTables: () => [],
        getTable: () => undefined,
      } as unknown as SqlModules;
      const realNode = new TableSourceNode({
        trace: mockTrace,
        sqlModules: mockSqlModules,
      });

      const clipboardNodes: ClipboardEntry[] = [
        {node: realNode, relativeX: 0, relativeY: 0, isDocked: false},
      ];

      const result1 = pasteClipboardNodes({
        rootNodes: [],
        nodeLayouts: new Map(),
        clipboardNodes,
      });

      const result2 = pasteClipboardNodes({
        rootNodes: result1?.rootNodes ?? [],
        nodeLayouts: result1?.nodeLayouts ?? new Map(),
        clipboardNodes,
      });

      expect(result2).toBeDefined();
      expect(result2?.rootNodes).toHaveLength(2);
      // The two pasted nodes should have different IDs
      expect(result2?.rootNodes[0].nodeId).not.toBe(
        result2?.rootNodes[1].nodeId,
      );
    });

    it('should restore connections between pasted nodes', () => {
      const parent = createMockNode({nodeId: 'p', type: NodeType.kTable});
      const child = createMockNode({nodeId: 'c', type: NodeType.kFilter});
      const clipboardNodes: ClipboardEntry[] = [
        {node: parent, relativeX: 0, relativeY: 0, isDocked: false},
        {node: child, relativeX: 0, relativeY: 100, isDocked: false},
      ];
      const clipboardConnections: ClipboardConnection[] = [
        {fromIndex: 0, toIndex: 1},
      ];

      const result = pasteClipboardNodes({
        rootNodes: [],
        nodeLayouts: new Map(),
        clipboardNodes,
        clipboardConnections,
      });

      expect(result).toBeDefined();
      expect(result?.rootNodes).toHaveLength(2);
      const pastedParent = result?.rootNodes[0];
      const pastedChild = result?.rootNodes[1];
      // addConnection should have connected them
      expect(pastedParent?.nextNodes).toContain(pastedChild);
    });

    it('should preserve existing nodeLayouts on paste', () => {
      const existing = createMockNode({nodeId: 'existing'});
      const clipNode = createMockNode({nodeId: 'clip'});
      const existingLayouts = new Map([['existing', {x: 500, y: 600}]]);
      const clipboardNodes: ClipboardEntry[] = [
        {node: clipNode, relativeX: 0, relativeY: 0, isDocked: false},
      ];

      const result = pasteClipboardNodes({
        rootNodes: [existing],
        nodeLayouts: existingLayouts,
        clipboardNodes,
      });

      expect(result).toBeDefined();
      expect(result?.nodeLayouts.get('existing')).toEqual({x: 500, y: 600});
    });
  });
});

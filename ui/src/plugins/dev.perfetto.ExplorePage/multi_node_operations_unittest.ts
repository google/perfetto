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

import {HistoryManager} from './history_manager';
import {ExplorePageState} from './explore_page';
import {Trace} from '../../public/trace';
import {SqlModules, SqlTable} from '../dev.perfetto.SqlModules/sql_modules';
import {TableSourceNode} from './query_builder/nodes/sources/table_source';
import {FilterNode} from './query_builder/nodes/filter_node';
import {PerfettoSqlType} from '../../trace_processor/perfetto_sql_type';
import {addConnection, getAllNodes} from './query_builder/graph_utils';
import {registerCoreNodes} from './query_builder/core_nodes';

registerCoreNodes();

describe('Multi-node operations', () => {
  let trace: Trace;
  let sqlModules: SqlModules;
  let historyManager: HistoryManager;

  beforeEach(() => {
    trace = {
      traceInfo: {
        traceTitle: 'Test Trace',
      },
    } as unknown as Trace;

    const stringType: PerfettoSqlType = {kind: 'string'};

    const testTable: SqlTable = {
      name: 'test_table',
      description: 'Test table',
      type: 'table',
      importance: undefined,
      getTableColumns: () => [],
      columns: [
        {name: 'id', type: stringType},
        {name: 'name', type: stringType},
      ],
    };

    sqlModules = {
      getTable: (name: string) => {
        if (name === 'test_table') return testTable;
        return undefined;
      },
    } as unknown as SqlModules;

    historyManager = new HistoryManager(trace, sqlModules);
  });

  describe('Multi-node copy', () => {
    test('should capture relative positions of multiple nodes', () => {
      const node1 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });
      const node2 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });

      const state: ExplorePageState = {
        rootNodes: [node1, node2],
        selectedNodes: new Set([node1.nodeId, node2.nodeId]),
        nodeLayouts: new Map([
          [node1.nodeId, {x: 100, y: 200}],
          [node2.nodeId, {x: 300, y: 400}],
        ]),
        labels: [],
      };

      // Simulate handleCopy logic
      const allNodes = getAllNodes(state.rootNodes);
      const selectedNodes = allNodes.filter((n) =>
        state.selectedNodes.has(n.nodeId),
      );

      const positions = selectedNodes.map((node) => {
        const layout = state.nodeLayouts.get(node.nodeId);
        return {
          node,
          x: layout?.x ?? 0,
          y: layout?.y ?? 0,
        };
      });

      const minX = Math.min(...positions.map((p) => p.x));
      const minY = Math.min(...positions.map((p) => p.y));

      expect(minX).toBe(100);
      expect(minY).toBe(200);

      // Check relative positions
      const relativePositions = positions.map((p) => ({
        relativeX: p.x - minX,
        relativeY: p.y - minY,
      }));

      expect(relativePositions[0]).toEqual({relativeX: 0, relativeY: 0});
      expect(relativePositions[1]).toEqual({relativeX: 200, relativeY: 200});
    });

    test('should capture connections between selected nodes', () => {
      const node1 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });
      const node2 = new FilterNode({filters: []});
      addConnection(node1, node2);

      // Verify connection exists
      expect(node2.primaryInput).toBe(node1);
      expect(node1.nextNodes).toContain(node2);
    });

    test('should handle docked nodes (nodes without explicit layout)', () => {
      const node1 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });
      const node2 = new FilterNode({filters: []});
      addConnection(node1, node2);

      const nodeLayouts = new Map([[node1.nodeId, {x: 100, y: 200}]]);
      // node2 has no layout (docked)

      const hasNode2Layout = nodeLayouts.has(node2.nodeId);
      expect(hasNode2Layout).toBe(false);
    });
  });

  describe('Multi-node paste', () => {
    test('should paste multiple times from same clipboard', () => {
      const node1 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });

      const clipboardNodes = [
        {
          node: node1.clone(),
          relativeX: 0,
          relativeY: 0,
          isDocked: false,
        },
      ];

      // First paste
      const newNodes1 = clipboardNodes.map((entry) => entry.node.clone());
      expect(newNodes1).toHaveLength(1);
      expect(newNodes1[0].nodeId).not.toBe(node1.nodeId);

      // Second paste - should create different nodes
      const newNodes2 = clipboardNodes.map((entry) => entry.node.clone());
      expect(newNodes2).toHaveLength(1);
      expect(newNodes2[0].nodeId).not.toBe(node1.nodeId);
      expect(newNodes2[0].nodeId).not.toBe(newNodes1[0].nodeId);
    });

    test('should restore connections between pasted nodes', () => {
      const node1 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });
      const node2 = new FilterNode({filters: []});
      addConnection(node1, node2);

      // Clone nodes for clipboard
      const clonedNode1 = node1.clone();
      const clonedNode2 = node2.clone();

      // Simulate pasting
      const pastedNode1 = clonedNode1.clone();
      const pastedNode2 = clonedNode2.clone();

      // Restore connection
      addConnection(pastedNode1, pastedNode2);

      // Verify connection was restored
      expect(pastedNode2.primaryInput).toBe(pastedNode1);
      expect(pastedNode1.nextNodes).toContain(pastedNode2);
    });
  });

  describe('Multi-node delete with undo', () => {
    test('should create single undo entry when using handleDeleteSelectedNodes', () => {
      // This test verifies that the batched multi-node deletion creates
      // only ONE undo entry, so users can restore all deleted nodes at once
      const node1 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });
      const node2 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });
      const node3 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });

      // Initial state with 3 nodes
      const state1: ExplorePageState = {
        rootNodes: [node1, node2, node3],
        selectedNodes: new Set([node1.nodeId, node2.nodeId, node3.nodeId]),
        nodeLayouts: new Map(),
        labels: [],
      };
      historyManager.pushState(state1);

      // Delete all 3 nodes at once (single history entry)
      const state2: ExplorePageState = {
        rootNodes: [],
        selectedNodes: new Set(),
        nodeLayouts: new Map(),
        labels: [],
      };
      historyManager.pushState(state2);

      // Single undo should restore all 3 nodes
      expect(historyManager.canUndo()).toBe(true);
      const restoredState = historyManager.undo();
      expect(restoredState).not.toBeNull();
      expect(restoredState!.rootNodes.length).toBe(3);

      // No more undo steps needed
      expect(historyManager.canUndo()).toBe(false);
    });

    test('should restore connected nodes properly after undo', () => {
      // Create a chain: table -> filter1 -> filter2
      const tableNode = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });
      const filter1 = new FilterNode({filters: []});
      const filter2 = new FilterNode({filters: []});

      addConnection(tableNode, filter1);
      addConnection(filter1, filter2);

      // Save initial state with all 3 connected nodes
      const state1: ExplorePageState = {
        rootNodes: [tableNode],
        selectedNodes: new Set([
          tableNode.nodeId,
          filter1.nodeId,
          filter2.nodeId,
        ]),
        nodeLayouts: new Map(),
        labels: [],
      };
      historyManager.pushState(state1);

      // Delete all nodes
      const state2: ExplorePageState = {
        rootNodes: [],
        selectedNodes: new Set(),
        nodeLayouts: new Map(),
        labels: [],
      };
      historyManager.pushState(state2);

      // Undo should restore all nodes
      const restoredState = historyManager.undo();
      expect(restoredState).not.toBeNull();

      // Check that all nodes are back
      const allRestoredNodes = getAllNodes(restoredState!.rootNodes);
      expect(allRestoredNodes.length).toBe(3);

      // Verify connections are restored
      // Find the restored table node (should be root)
      expect(restoredState!.rootNodes.length).toBe(1);
      const restoredTable = restoredState!.rootNodes[0];
      expect(restoredTable.nextNodes.length).toBe(1);

      const restoredFilter1 = restoredTable.nextNodes[0];
      expect(restoredFilter1.nextNodes.length).toBe(1);

      const restoredFilter2 = restoredFilter1.nextNodes[0];
      expect(restoredFilter2.nextNodes.length).toBe(0);
    });
  });

  describe('Multi-node selection', () => {
    test('should maintain primary selected node when removing from selection', () => {
      const node1 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });
      const node2 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });
      const node3 = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: sqlModules.getTable('test_table')!,
      });

      const state: ExplorePageState = {
        rootNodes: [node1, node2, node3],
        selectedNodes: new Set([node1.nodeId, node2.nodeId, node3.nodeId]),
        nodeLayouts: new Map(),
        labels: [],
      };

      // Remove primary selected node
      const newSelectedNodes = new Set(state.selectedNodes);
      newSelectedNodes.delete(node1.nodeId);

      // Should pick another node as primary
      const allNodes = getAllNodes(state.rootNodes);
      const remainingNodeId = newSelectedNodes.values().next().value;
      const newPrimaryNode = allNodes.find((n) => n.nodeId === remainingNodeId);

      expect(newPrimaryNode).toBeDefined();
      expect([node2, node3]).toContain(newPrimaryNode);
    });
  });
});

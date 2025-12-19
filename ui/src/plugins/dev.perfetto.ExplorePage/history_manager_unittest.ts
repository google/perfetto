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

import {HistoryManager} from './history_manager';
import {ExplorePageState} from './explore_page';
import {Trace} from '../../public/trace';
import {SqlModules, SqlTable} from '../dev.perfetto.SqlModules/sql_modules';
import {TableSourceNode} from './query_builder/nodes/sources/table_source';
import {AggregationNode} from './query_builder/nodes/aggregation_node';
import {FilterNode} from './query_builder/nodes/filter_node';
import {PerfettoSqlType} from '../../trace_processor/perfetto_sql_type';
import {addConnection, removeConnection} from './query_builder/graph_utils';
import {UIFilter} from './query_builder/operations/filter';
import {ColumnInfo} from './query_builder/column_info';

describe('HistoryManager', () => {
  let trace: Trace;
  let sqlModules: SqlModules;
  let historyManager: HistoryManager;

  beforeEach(() => {
    trace = {
      traceInfo: {
        traceTitle: 'test_trace',
      },
    } as Trace;

    const stringType: PerfettoSqlType = {kind: 'string'};
    const timestampType: PerfettoSqlType = {kind: 'timestamp'};

    const testTable: SqlTable = {
      name: 'test_table',
      description: 'Test table',
      type: 'table',
      importance: undefined,
      getTableColumns: () => [],
      columns: [
        {
          name: 'id',
          type: stringType,
        },
        {
          name: 'ts',
          type: timestampType,
        },
        {
          name: 'name',
          type: stringType,
        },
      ],
    };

    sqlModules = {
      listTables: () => [testTable],
      getTable: (name: string) => (name === 'test_table' ? testTable : null),
    } as SqlModules;

    historyManager = new HistoryManager(trace, sqlModules);
  });

  test('should track initial state', () => {
    const emptyState: ExplorePageState = {
      rootNodes: [],
      nodeLayouts: new Map(),
    };

    historyManager.pushState(emptyState);
    expect(historyManager.canUndo()).toBe(false);
    expect(historyManager.canRedo()).toBe(false);
  });

  test('should track node addition', () => {
    // Create a table node
    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    // Initial state with table node
    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // Add an aggregation node
    const aggNode = new AggregationNode({
      groupByColumns: [],
      aggregations: [],
    });
    addConnection(tableNode, aggNode);

    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    // We should be able to undo once
    expect(historyManager.canUndo()).toBe(true);

    // Undo: remove aggregation node
    const undoneState = historyManager.undo();
    expect(undoneState).not.toBeNull();
    expect(undoneState!.rootNodes[0].nextNodes.length).toBe(0);
  });

  test('should ignore layout-only changes', () => {
    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // Change only layout
    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map([[tableNode.nodeId, {x: 100, y: 100}]]),
    };
    historyManager.pushState(state2);

    // Should not create a new history entry
    expect(historyManager.canUndo()).toBe(false);
  });

  test('should ignore selectedNode changes', () => {
    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const aggNode = new AggregationNode({
      groupByColumns: [],
      aggregations: [],
    });
    addConnection(tableNode, aggNode);

    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // Change only the selected node (to view different node data)
    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      selectedNode: tableNode,
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    // Should not create a new history entry
    expect(historyManager.canUndo()).toBe(false);

    // Change selection to a different node
    const state3: ExplorePageState = {
      rootNodes: [tableNode],
      selectedNode: aggNode,
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state3);

    // Still should not create a new history entry
    expect(historyManager.canUndo()).toBe(false);
  });

  test('should handle redo correctly', () => {
    const state1: ExplorePageState = {
      rootNodes: [],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    // Undo
    historyManager.undo();
    expect(historyManager.canRedo()).toBe(true);

    // Redo
    const redoneState = historyManager.redo();
    expect(redoneState).not.toBeNull();
    expect(redoneState!.rootNodes.length).toBe(1);
  });

  test('should clear redo stack when new state is pushed', () => {
    const state1: ExplorePageState = {
      rootNodes: [],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    // Undo
    historyManager.undo();
    expect(historyManager.canRedo()).toBe(true);

    // Push a new state
    const state3: ExplorePageState = {
      rootNodes: [],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state3);

    // Redo should no longer be available
    expect(historyManager.canRedo()).toBe(false);
  });

  test('should maintain maximum history size', () => {
    // Push 15 different states by adding different numbers of nodes
    for (let i = 0; i < 15; i++) {
      const nodes = [];
      // Create i nodes to make each state unique
      for (let j = 0; j <= i; j++) {
        nodes.push(
          new TableSourceNode({
            trace,
            sqlModules,
            sqlTable: sqlModules.getTable('test_table')!,
          }),
        );
      }
      const state: ExplorePageState = {
        rootNodes: nodes,
        nodeLayouts: new Map(),
      };
      historyManager.pushState(state);
    }

    // Should only keep last 10 states, allowing 9 undo operations
    let undoCount = 0;
    while (historyManager.canUndo()) {
      historyManager.undo();
      undoCount++;
    }

    expect(undoCount).toBe(9);
  });

  // ========================================
  // Connection Operation Tests
  // ========================================

  test('should track connection removal', () => {
    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const aggNode = new AggregationNode({
      groupByColumns: [],
      aggregations: [],
    });

    // State 1: Table node connected to aggregation node
    addConnection(tableNode, aggNode);
    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // State 2: Remove connection
    removeConnection(tableNode, aggNode);
    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    expect(historyManager.canUndo()).toBe(true);

    // Undo: connection should be restored
    const undoneState = historyManager.undo();
    expect(undoneState).not.toBeNull();
    expect(undoneState!.rootNodes[0].nextNodes.length).toBe(1);
    expect(undoneState!.rootNodes[0].nextNodes[0].type).toBe(aggNode.type);

    // Redo: connection should be removed again
    const redoneState = historyManager.redo();
    expect(redoneState).not.toBeNull();
    expect(redoneState!.rootNodes[0].nextNodes.length).toBe(0);
  });

  test('should track multiple connection changes', () => {
    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const aggNode1 = new AggregationNode({
      groupByColumns: [],
      aggregations: [],
    });

    const aggNode2 = new AggregationNode({
      groupByColumns: [],
      aggregations: [],
    });

    // State 1: No connections
    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // State 2: Connect to first agg node
    addConnection(tableNode, aggNode1);
    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    // State 3: Add second agg node connection
    addConnection(tableNode, aggNode2);
    const state3: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state3);

    // Should have 2 connections
    expect(tableNode.nextNodes.length).toBe(2);

    // Undo once: should have 1 connection
    const undoneState1 = historyManager.undo();
    expect(undoneState1).not.toBeNull();
    expect(undoneState1!.rootNodes[0].nextNodes.length).toBe(1);

    // Undo again: should have 0 connections
    const undoneState2 = historyManager.undo();
    expect(undoneState2).not.toBeNull();
    expect(undoneState2!.rootNodes[0].nextNodes.length).toBe(0);
  });

  // ========================================
  // Node Property Modification Tests
  // ========================================

  test('should track filter additions to FilterNode', () => {
    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const filterNode = new FilterNode({
      filters: [],
    });
    addConnection(tableNode, filterNode);

    // State 1: Filter node with no filters
    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // State 2: Add a filter
    const filter1: UIFilter = {
      column: 'id',
      op: '=',
      value: '123',
    };
    filterNode.state.filters = [filter1];
    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    expect(historyManager.canUndo()).toBe(true);

    // Undo: filters should be empty
    const undoneState = historyManager.undo();
    expect(undoneState).not.toBeNull();
    const restoredFilterNode = undoneState!.rootNodes[0]
      .nextNodes[0] as FilterNode;
    expect(restoredFilterNode.state.filters?.length ?? 0).toBe(0);

    // Redo: filter should be back
    const redoneState = historyManager.redo();
    expect(redoneState).not.toBeNull();
    const redoneFilterNode = redoneState!.rootNodes[0]
      .nextNodes[0] as FilterNode;
    expect(redoneFilterNode.state.filters?.length ?? 0).toBe(1);
    const redoneFilter = redoneFilterNode.state.filters?.[0];
    if (redoneFilter && 'value' in redoneFilter) {
      expect(redoneFilter.value).toBe('123');
    }
  });

  test('should track multiple filter additions', () => {
    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const filterNode = new FilterNode({
      filters: [],
    });
    addConnection(tableNode, filterNode);

    // State 1: No filters
    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // State 2: Add first filter
    const filter1: UIFilter = {
      column: 'id',
      op: '=',
      value: '123',
    };
    filterNode.state.filters = [filter1];
    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    // State 3: Add second filter
    const filter2: UIFilter = {
      column: 'name',
      op: '=',
      value: 'test',
    };
    filterNode.state.filters = [filter1, filter2];
    const state3: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state3);

    // Should have 2 filters
    expect(filterNode.state.filters?.length).toBe(2);

    // Undo once: should have 1 filter
    const undoneState1 = historyManager.undo();
    expect(undoneState1).not.toBeNull();
    const filterNode1 = undoneState1!.rootNodes[0].nextNodes[0] as FilterNode;
    expect(filterNode1.state.filters?.length ?? 0).toBe(1);
    const undoneFilter1 = filterNode1.state.filters?.[0];
    if (undoneFilter1 && 'value' in undoneFilter1) {
      expect(undoneFilter1.value).toBe('123');
    }

    // Undo again: should have 0 filters
    const undoneState2 = historyManager.undo();
    expect(undoneState2).not.toBeNull();
    const filterNode2 = undoneState2!.rootNodes[0].nextNodes[0] as FilterNode;
    expect(filterNode2.state.filters?.length ?? 0).toBe(0);

    // Redo twice: should have 2 filters back
    historyManager.redo();
    const redoneState = historyManager.redo();
    expect(redoneState).not.toBeNull();
    const filterNode3 = redoneState!.rootNodes[0].nextNodes[0] as FilterNode;
    expect(filterNode3.state.filters?.length ?? 0).toBe(2);
  });

  test('should track aggregation node property changes', () => {
    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const aggNode = new AggregationNode({
      groupByColumns: [],
      aggregations: [],
    });
    addConnection(tableNode, aggNode);

    // After addConnection, onPrevNodesUpdated populates columns from input (all unchecked)
    // State 1: All columns unchecked
    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // State 2: Check the 'id' column for grouping
    aggNode.state.groupByColumns = aggNode.state.groupByColumns.map((c) => ({
      ...c,
      checked: c.name === 'id',
    }));
    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    expect(historyManager.canUndo()).toBe(true);

    // Undo: no columns should be checked
    const undoneState = historyManager.undo();
    expect(undoneState).not.toBeNull();
    const restoredAggNode = undoneState!.rootNodes[0]
      .nextNodes[0] as AggregationNode;
    const uncheckedCols = restoredAggNode.state.groupByColumns?.filter(
      (c) => c.checked,
    );
    expect(uncheckedCols?.length ?? 0).toBe(0);

    // Redo: 'id' column should be checked
    const redoneState = historyManager.redo();
    expect(redoneState).not.toBeNull();
    const redoneAggNode = redoneState!.rootNodes[0]
      .nextNodes[0] as AggregationNode;
    const checkedCols = redoneAggNode.state.groupByColumns?.filter(
      (c) => c.checked,
    );
    expect(checkedCols?.length ?? 0).toBe(1);
    expect(checkedCols?.[0].name).toBe('id');
  });

  // ========================================
  // Node Deletion Tests
  // ========================================

  test('should track node deletion from root', () => {
    const tableNode1 = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const tableNode2 = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    // State 1: Two root nodes
    const state1: ExplorePageState = {
      rootNodes: [tableNode1, tableNode2],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // State 2: Remove one node
    const state2: ExplorePageState = {
      rootNodes: [tableNode1],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    expect(historyManager.canUndo()).toBe(true);

    // Undo: both nodes should be back
    const undoneState = historyManager.undo();
    expect(undoneState).not.toBeNull();
    expect(undoneState!.rootNodes.length).toBe(2);

    // Redo: back to one node
    const redoneState = historyManager.redo();
    expect(redoneState).not.toBeNull();
    expect(redoneState!.rootNodes.length).toBe(1);
  });

  test('should track deletion of connected node', () => {
    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    const aggNode = new AggregationNode({
      groupByColumns: [],
      aggregations: [],
    });
    addConnection(tableNode, aggNode);

    // State 1: Table with aggregation
    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // State 2: Remove aggregation node
    removeConnection(tableNode, aggNode);
    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    expect(historyManager.canUndo()).toBe(true);

    // Undo: aggregation should be restored
    const undoneState = historyManager.undo();
    expect(undoneState).not.toBeNull();
    expect(undoneState!.rootNodes[0].nextNodes.length).toBe(1);

    // Verify the restored node is an aggregation
    const restoredNode = undoneState!.rootNodes[0].nextNodes[0];
    expect(restoredNode.type).toBe(aggNode.type);
  });

  // ========================================
  // Complex Scenario Tests
  // ========================================

  test('should handle complex multi-step operation', () => {
    // This test simulates: add table -> add filter -> modify filter -> add aggregation
    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    // State 1: Just table
    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // State 2: Add filter node
    const filterNode = new FilterNode({
      filters: [],
    });
    addConnection(tableNode, filterNode);
    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    // State 3: Add filter
    const filter: UIFilter = {
      column: 'id',
      op: '=',
      value: '123',
    };
    filterNode.state.filters = [filter];
    const state3: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state3);

    // State 4: Add aggregation after filter
    const groupByColumn: ColumnInfo = {
      name: 'id',
      type: 'STRING',
      checked: true,
      column: {name: 'id', type: {kind: 'string'}},
    };
    const aggNode = new AggregationNode({
      groupByColumns: [groupByColumn],
      aggregations: [],
    });
    addConnection(filterNode, aggNode);
    const state4: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state4);

    // Verify final state
    expect(tableNode.nextNodes.length).toBe(1);
    expect(tableNode.nextNodes[0]).toBe(filterNode);
    expect(filterNode.nextNodes.length).toBe(1);
    expect(filterNode.nextNodes[0]).toBe(aggNode);

    // Undo to state 3: aggregation should be gone
    const state3Restored = historyManager.undo();
    expect(state3Restored).not.toBeNull();
    const filterNode3 = state3Restored!.rootNodes[0].nextNodes[0] as FilterNode;
    expect(filterNode3.nextNodes.length).toBe(0);
    expect(filterNode3.state.filters?.length).toBe(1);

    // Undo to state 2: filter should be empty
    const state2Restored = historyManager.undo();
    expect(state2Restored).not.toBeNull();
    const filterNode2 = state2Restored!.rootNodes[0].nextNodes[0] as FilterNode;
    expect(filterNode2.state.filters?.length ?? 0).toBe(0);

    // Undo to state 1: no filter node
    const state1Restored = historyManager.undo();
    expect(state1Restored).not.toBeNull();
    expect(state1Restored!.rootNodes[0].nextNodes.length).toBe(0);

    // Redo all the way forward
    historyManager.redo(); // State 2
    historyManager.redo(); // State 3
    const state4Restored = historyManager.redo(); // State 4
    expect(state4Restored).not.toBeNull();
    expect(state4Restored!.rootNodes[0].nextNodes[0].nextNodes.length).toBe(1);
  });

  // ========================================
  // Regression Tests for handleFilterAdd Bug
  // ========================================

  test('should create single undo point when operation is atomic', () => {
    // This test shows the CORRECT pattern: multiple mutations with single state update

    const tableNode = new TableSourceNode({
      trace,
      sqlModules,
      sqlTable: sqlModules.getTable('test_table')!,
    });

    // State 1: Just table
    const state1: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // Perform multiple mutations THEN single state update (correct pattern)
    const filterNode = new FilterNode({
      filters: [],
    });
    addConnection(tableNode, filterNode);

    const filter: UIFilter = {
      column: 'id',
      op: '=',
      value: '123',
    };
    filterNode.state.filters = [filter];

    // Single state update captures all mutations
    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
      selectedNode: filterNode,
    };
    historyManager.pushState(state2);

    // Should have exactly 1 undo point (from state1 to state2)
    expect(historyManager.canUndo()).toBe(true);

    // Single undo returns to state1
    const undoneState = historyManager.undo();
    expect(undoneState).not.toBeNull();
    expect(undoneState!.rootNodes[0].nextNodes.length).toBe(0); // Back to just table

    // No more undo points
    expect(historyManager.canUndo()).toBe(false);
  });
});

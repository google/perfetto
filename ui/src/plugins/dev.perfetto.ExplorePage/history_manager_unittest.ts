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
import {PerfettoSqlType} from '../../trace_processor/perfetto_sql_type';
import {addConnection} from './query_node';

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
    // Initial empty state
    const state1: ExplorePageState = {
      rootNodes: [],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state1);

    // Add a table node
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

    expect(historyManager.canUndo()).toBe(true);
    expect(historyManager.canRedo()).toBe(false);

    // Undo should go back to empty state
    const undoneState = historyManager.undo();
    expect(undoneState).not.toBeNull();
    expect(undoneState!.rootNodes.length).toBe(0);
  });

  test('should track granular property changes', () => {
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
      prevNode: tableNode,
      groupByColumns: [],
      aggregations: [],
    });
    addConnection(tableNode, aggNode);

    const state2: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state2);

    // Add first group by column (check one column)
    aggNode.state.groupByColumns[0].checked = true;
    const state3: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state3);

    // Add second group by column (check another column)
    aggNode.state.groupByColumns[2].checked = true;
    const state4: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };
    historyManager.pushState(state4);

    // We should be able to undo 3 times
    expect(historyManager.canUndo()).toBe(true);

    // First undo: remove second group by column
    const undoneState1 = historyManager.undo();
    expect(undoneState1).not.toBeNull();
    const restoredAggNode1 = undoneState1!.rootNodes[0]
      .nextNodes[0] as AggregationNode;
    const checkedCols1 = restoredAggNode1.state.groupByColumns.filter(
      (c) => c.checked,
    );
    expect(checkedCols1.length).toBe(1);
    expect(checkedCols1[0].name).toBe('id');

    // Second undo: remove first group by column
    const undoneState2 = historyManager.undo();
    expect(undoneState2).not.toBeNull();
    const restoredAggNode2 = undoneState2!.rootNodes[0]
      .nextNodes[0] as AggregationNode;
    const checkedCols2 = restoredAggNode2.state.groupByColumns.filter(
      (c) => c.checked,
    );
    expect(checkedCols2.length).toBe(0);

    // Third undo: remove aggregation node entirely
    const undoneState3 = historyManager.undo();
    expect(undoneState3).not.toBeNull();
    expect(undoneState3!.rootNodes[0].nextNodes.length).toBe(0);
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
});

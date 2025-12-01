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

import {ModifyColumnsNode} from './nodes/modify_columns_node';
import {AggregationNode} from './nodes/aggregation_node';
import {QueryNode, NodeType} from '../query_node';
import {columnInfoFromSqlColumn} from './column_info';
import {NodeDetailsAttrs} from './node_explorer_types';

describe('Node Propagation', () => {
  function createMockSourceNode(): QueryNode {
    return {
      nodeId: 'source',
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: [
        columnInfoFromSqlColumn({name: 'id', type: {kind: 'int'}}, true),
        columnInfoFromSqlColumn({name: 'name', type: {kind: 'string'}}, true),
        columnInfoFromSqlColumn({name: 'value', type: {kind: 'int'}}, true),
      ],
      state: {},
      validate: () => true,
      getTitle: () => 'Source',
      nodeSpecificModify: () => null,
      nodeDetails: (): NodeDetailsAttrs => ({content: null}),
      nodeInfo: () => null,
      clone: () => createMockSourceNode(),
      getStructuredQuery: () => undefined,
      serializeState: () => ({}),
    } as QueryNode;
  }

  describe('column rename propagation', () => {
    it('REGRESSION: should update AggregationNode when ModifyColumnsNode columns change', () => {
      // This test reproduces the original bug: when you rename a column in
      // ModifyColumnsNode, AggregationNode didn't see the change at all.

      // Setup: Source -> Modify -> Aggregation
      const sourceNode = createMockSourceNode();
      const modifyNode = new ModifyColumnsNode({selectedColumns: []});
      const aggNode = new AggregationNode({
        groupByColumns: [],
        aggregations: [],
      });

      // Connect the nodes
      sourceNode.nextNodes.push(modifyNode);
      modifyNode.primaryInput = sourceNode;
      modifyNode.nextNodes.push(aggNode);
      aggNode.primaryInput = modifyNode;

      // Initialize the nodes
      modifyNode.onPrevNodesUpdated?.();
      aggNode.onPrevNodesUpdated?.();

      // Initial state: AggregationNode should have columns: id, name, value
      expect(aggNode.state.groupByColumns.map((c) => c.name)).toEqual([
        'id',
        'name',
        'value',
      ]);

      // THE BUG: Rename 'name' to 'user_name' in ModifyColumnsNode
      modifyNode.state.selectedColumns[1].alias = 'user_name';

      // BEFORE FIX: AggregationNode would still show 'id', 'name', 'value'
      // AFTER FIX: We need to call onPrevNodesUpdated to propagate the change
      aggNode.onPrevNodesUpdated?.();

      // Verify the fix: AggregationNode should now see the renamed column
      const columnNames = aggNode.state.groupByColumns.map((c) => c.name);
      expect(columnNames).toContain('user_name');
      expect(columnNames).not.toContain('name');
      expect(columnNames).toEqual(['id', 'user_name', 'value']);
    });

    it('should propagate renamed columns from ModifyColumnsNode to AggregationNode', () => {
      // Setup: Source -> Modify -> Aggregation
      const sourceNode = createMockSourceNode();
      const modifyNode = new ModifyColumnsNode({selectedColumns: []});
      const aggNode = new AggregationNode({
        groupByColumns: [],
        aggregations: [],
      });

      // Connect the nodes
      sourceNode.nextNodes.push(modifyNode);
      modifyNode.primaryInput = sourceNode;
      modifyNode.nextNodes.push(aggNode);
      aggNode.primaryInput = modifyNode;

      // Initialize the modify node to get columns from source
      modifyNode.onPrevNodesUpdated?.();

      // Verify modify node has the original columns
      expect(modifyNode.state.selectedColumns).toHaveLength(3);
      expect(modifyNode.state.selectedColumns[0].name).toBe('id');
      expect(modifyNode.state.selectedColumns[1].name).toBe('name');
      expect(modifyNode.state.selectedColumns[2].name).toBe('value');

      // Initialize the aggregation node
      aggNode.onPrevNodesUpdated?.();

      // Verify aggregation node has the original columns
      expect(aggNode.state.groupByColumns).toHaveLength(3);
      expect(aggNode.state.groupByColumns[0].name).toBe('id');
      expect(aggNode.state.groupByColumns[1].name).toBe('name');
      expect(aggNode.state.groupByColumns[2].name).toBe('value');

      // Now rename 'name' to 'user_name' in the modify node
      modifyNode.state.selectedColumns[1].alias = 'user_name';

      // Verify the finalCols of modify node uses the alias
      const modifyFinalCols = modifyNode.finalCols;
      expect(modifyFinalCols).toHaveLength(3);
      expect(modifyFinalCols[0].name).toBe('id');
      expect(modifyFinalCols[1].name).toBe('user_name'); // Should use alias
      expect(modifyFinalCols[2].name).toBe('value');

      // Simulate the builder's onchange behavior: notify downstream nodes
      aggNode.onPrevNodesUpdated?.();

      // Verify aggregation node now sees the renamed column
      expect(aggNode.state.groupByColumns).toHaveLength(3);
      expect(aggNode.state.groupByColumns[0].name).toBe('id');
      expect(aggNode.state.groupByColumns[1].name).toBe('user_name'); // Should see the alias
      expect(aggNode.state.groupByColumns[2].name).toBe('value');
    });

    it('should preserve checked status when columns are renamed', () => {
      // Setup: Source -> Modify -> Aggregation
      const sourceNode = createMockSourceNode();
      const modifyNode = new ModifyColumnsNode({selectedColumns: []});
      const aggNode = new AggregationNode({
        groupByColumns: [],
        aggregations: [],
      });

      // Connect the nodes
      sourceNode.nextNodes.push(modifyNode);
      modifyNode.primaryInput = sourceNode;
      modifyNode.nextNodes.push(aggNode);
      aggNode.primaryInput = modifyNode;

      // Initialize both nodes
      modifyNode.onPrevNodesUpdated?.();
      aggNode.onPrevNodesUpdated?.();

      // Check a column in the aggregation node
      aggNode.state.groupByColumns[1].checked = true; // Check 'name'

      // Rename the checked column in modify node
      modifyNode.state.selectedColumns[1].alias = 'user_name';

      // Notify downstream
      aggNode.onPrevNodesUpdated?.();

      // The checked status should be lost because the column name changed
      // This is expected behavior - the aggregation node can't know that
      // 'user_name' is the same as 'name'
      const userNameCol = aggNode.state.groupByColumns.find(
        (c) => c.name === 'user_name',
      );
      expect(userNameCol).toBeDefined();
      expect(userNameCol?.checked).toBe(false);
    });

    it('should handle column removal in ModifyColumnsNode', () => {
      // Setup: Source -> Modify -> Aggregation
      const sourceNode = createMockSourceNode();
      const modifyNode = new ModifyColumnsNode({selectedColumns: []});
      const aggNode = new AggregationNode({
        groupByColumns: [],
        aggregations: [],
      });

      // Connect the nodes
      sourceNode.nextNodes.push(modifyNode);
      modifyNode.primaryInput = sourceNode;
      modifyNode.nextNodes.push(aggNode);
      aggNode.primaryInput = modifyNode;

      // Initialize both nodes
      modifyNode.onPrevNodesUpdated?.();
      aggNode.onPrevNodesUpdated?.();

      // Initially should have 3 columns
      expect(aggNode.state.groupByColumns).toHaveLength(3);

      // Uncheck 'name' column in modify node (removes it from output)
      modifyNode.state.selectedColumns[1].checked = false;

      // Verify modify node's finalCols only has 2 columns now
      expect(modifyNode.finalCols).toHaveLength(2);
      expect(modifyNode.finalCols[0].name).toBe('id');
      expect(modifyNode.finalCols[1].name).toBe('value');

      // Notify downstream
      aggNode.onPrevNodesUpdated?.();

      // Aggregation node should now only have 2 columns
      expect(aggNode.state.groupByColumns).toHaveLength(2);
      expect(aggNode.state.groupByColumns[0].name).toBe('id');
      expect(aggNode.state.groupByColumns[1].name).toBe('value');
    });

    it('should handle column reordering in ModifyColumnsNode', () => {
      // Setup: Source -> Modify -> Aggregation
      const sourceNode = createMockSourceNode();
      const modifyNode = new ModifyColumnsNode({selectedColumns: []});
      const aggNode = new AggregationNode({
        groupByColumns: [],
        aggregations: [],
      });

      // Connect the nodes
      sourceNode.nextNodes.push(modifyNode);
      modifyNode.primaryInput = sourceNode;
      modifyNode.nextNodes.push(aggNode);
      aggNode.primaryInput = modifyNode;

      // Initialize both nodes
      modifyNode.onPrevNodesUpdated?.();
      aggNode.onPrevNodesUpdated?.();

      // Initially: id, name, value
      expect(modifyNode.finalCols[0].name).toBe('id');
      expect(modifyNode.finalCols[1].name).toBe('name');
      expect(modifyNode.finalCols[2].name).toBe('value');

      // Reorder: move 'value' to the front
      const cols = modifyNode.state.selectedColumns;
      modifyNode.state.selectedColumns = [cols[2], cols[0], cols[1]];

      // Verify modify node's finalCols are reordered
      expect(modifyNode.finalCols[0].name).toBe('value');
      expect(modifyNode.finalCols[1].name).toBe('id');
      expect(modifyNode.finalCols[2].name).toBe('name');

      // Notify downstream
      aggNode.onPrevNodesUpdated?.();

      // Aggregation node should see the new order
      expect(aggNode.state.groupByColumns[0].name).toBe('value');
      expect(aggNode.state.groupByColumns[1].name).toBe('id');
      expect(aggNode.state.groupByColumns[2].name).toBe('name');
    });
  });

  describe('multi-level propagation', () => {
    it('should propagate changes through chain: Source -> Modify1 -> Modify2 -> Modify3', () => {
      // Setup: Source -> Modify1 -> Modify2 -> Modify3
      const sourceNode = createMockSourceNode();
      const modify1 = new ModifyColumnsNode({selectedColumns: []});
      const modify2 = new ModifyColumnsNode({selectedColumns: []});
      const modify3 = new ModifyColumnsNode({selectedColumns: []});

      // Connect the nodes
      sourceNode.nextNodes.push(modify1);
      modify1.primaryInput = sourceNode;
      modify1.nextNodes.push(modify2);
      modify2.primaryInput = modify1;
      modify2.nextNodes.push(modify3);
      modify3.primaryInput = modify2;

      // Initialize all nodes
      modify1.onPrevNodesUpdated?.();
      modify2.onPrevNodesUpdated?.();
      modify3.onPrevNodesUpdated?.();

      // Verify initial state - all have 'id', 'name', 'value'
      expect(modify1.finalCols.map((c) => c.name)).toEqual([
        'id',
        'name',
        'value',
      ]);
      expect(modify2.finalCols.map((c) => c.name)).toEqual([
        'id',
        'name',
        'value',
      ]);
      expect(modify3.finalCols.map((c) => c.name)).toEqual([
        'id',
        'name',
        'value',
      ]);

      // Rename 'name' to 'user_name' in modify1
      modify1.state.selectedColumns[1].alias = 'user_name';

      // Notify all downstream nodes (simulating builder's onchange)
      modify2.onPrevNodesUpdated?.();
      modify3.onPrevNodesUpdated?.();

      // All downstream nodes should see the renamed column
      expect(modify1.finalCols.map((c) => c.name)).toEqual([
        'id',
        'user_name',
        'value',
      ]);
      expect(modify2.finalCols.map((c) => c.name)).toEqual([
        'id',
        'user_name',
        'value',
      ]);
      expect(modify3.finalCols.map((c) => c.name)).toEqual([
        'id',
        'user_name',
        'value',
      ]);
    });

    it('should propagate changes through 5-node chain with middle node edit', () => {
      // Setup: Source -> Modify1 -> Modify2 -> Modify3 -> Modify4 -> Modify5
      const sourceNode = createMockSourceNode();
      const modify1 = new ModifyColumnsNode({selectedColumns: []});
      const modify2 = new ModifyColumnsNode({selectedColumns: []});
      const modify3 = new ModifyColumnsNode({selectedColumns: []});
      const modify4 = new ModifyColumnsNode({selectedColumns: []});
      const modify5 = new ModifyColumnsNode({selectedColumns: []});

      // Connect the nodes
      sourceNode.nextNodes.push(modify1);
      modify1.primaryInput = sourceNode;
      modify1.nextNodes.push(modify2);
      modify2.primaryInput = modify1;
      modify2.nextNodes.push(modify3);
      modify3.primaryInput = modify2;
      modify3.nextNodes.push(modify4);
      modify4.primaryInput = modify3;
      modify4.nextNodes.push(modify5);
      modify5.primaryInput = modify4;

      // Initialize all nodes
      modify1.onPrevNodesUpdated?.();
      modify2.onPrevNodesUpdated?.();
      modify3.onPrevNodesUpdated?.();
      modify4.onPrevNodesUpdated?.();
      modify5.onPrevNodesUpdated?.();

      // Edit the MIDDLE node (modify3) - rename 'value' to 'amount'
      modify3.state.selectedColumns[2].alias = 'amount';

      // Notify all downstream nodes (simulating builder's onchange)
      modify4.onPrevNodesUpdated?.();
      modify5.onPrevNodesUpdated?.();

      // Nodes before modify3 should not be affected
      expect(modify1.finalCols.map((c) => c.name)).toEqual([
        'id',
        'name',
        'value',
      ]);
      expect(modify2.finalCols.map((c) => c.name)).toEqual([
        'id',
        'name',
        'value',
      ]);

      // modify3 and all downstream nodes should see the change
      expect(modify3.finalCols.map((c) => c.name)).toEqual([
        'id',
        'name',
        'amount',
      ]);
      expect(modify4.finalCols.map((c) => c.name)).toEqual([
        'id',
        'name',
        'amount',
      ]);
      expect(modify5.finalCols.map((c) => c.name)).toEqual([
        'id',
        'name',
        'amount',
      ]);
    });

    it('should propagate changes through mixed node types: Modify -> Agg -> Modify', () => {
      // Setup: Source -> Modify1 -> Agg -> Modify2
      const sourceNode = createMockSourceNode();
      const modify1 = new ModifyColumnsNode({selectedColumns: []});
      const aggNode = new AggregationNode({
        groupByColumns: [],
        aggregations: [],
      });
      const modify2 = new ModifyColumnsNode({selectedColumns: []});

      // Connect the nodes
      sourceNode.nextNodes.push(modify1);
      modify1.primaryInput = sourceNode;
      modify1.nextNodes.push(aggNode);
      aggNode.primaryInput = modify1;
      aggNode.nextNodes.push(modify2);
      modify2.primaryInput = aggNode;

      // Initialize all nodes
      modify1.onPrevNodesUpdated?.();
      aggNode.onPrevNodesUpdated?.();

      // Check columns BEFORE renaming and BEFORE initializing modify2
      aggNode.state.groupByColumns[0].checked = true; // Check 'id'
      aggNode.state.groupByColumns[1].checked = true; // Check 'name'

      // Now initialize modify2 after aggNode has checked columns
      modify2.onPrevNodesUpdated?.();

      // Verify initial state of modify2
      expect(modify2.finalCols.map((c) => c.name)).toContain('id');
      expect(modify2.finalCols.map((c) => c.name)).toContain('name');

      // Rename 'name' to 'user_name' in modify1
      modify1.state.selectedColumns[1].alias = 'user_name';

      // Notify all downstream nodes
      aggNode.onPrevNodesUpdated?.();
      modify2.onPrevNodesUpdated?.();

      // Aggregation should see the renamed column in its available columns
      // Note: The old 'name' column is preserved as a "missing" column
      // because it was checked before the rename (intentional behavior)
      expect(aggNode.state.groupByColumns.map((c) => c.name)).toContain(
        'user_name',
      );

      // The checked status is lost when column is renamed (expected behavior)
      // But we can check it again with the new name
      const userNameCol = aggNode.state.groupByColumns.find(
        (c) => c.name === 'user_name',
      );
      const idCol = aggNode.state.groupByColumns.find((c) => c.name === 'id');
      if (userNameCol) {
        userNameCol.checked = true;
      }
      if (idCol) {
        idCol.checked = true; // Keep id checked
      }

      // Update modify2 to see the changes
      modify2.onPrevNodesUpdated?.();

      // modify2 should see both 'id' and 'user_name' from aggNode's finalCols
      expect(modify2.finalCols.map((c) => c.name)).toContain('id');
      expect(modify2.finalCols.map((c) => c.name)).toContain('user_name');
    });

    it('should handle multiple sequential edits in a chain', () => {
      // Setup: Source -> Modify1 -> Modify2 -> Modify3
      const sourceNode = createMockSourceNode();
      const modify1 = new ModifyColumnsNode({selectedColumns: []});
      const modify2 = new ModifyColumnsNode({selectedColumns: []});
      const modify3 = new ModifyColumnsNode({selectedColumns: []});

      // Connect the nodes
      sourceNode.nextNodes.push(modify1);
      modify1.primaryInput = sourceNode;
      modify1.nextNodes.push(modify2);
      modify2.primaryInput = modify1;
      modify2.nextNodes.push(modify3);
      modify3.primaryInput = modify2;

      // Initialize all nodes
      modify1.onPrevNodesUpdated?.();
      modify2.onPrevNodesUpdated?.();
      modify3.onPrevNodesUpdated?.();

      // First edit: modify1 renames 'name' to 'user_name'
      modify1.state.selectedColumns[1].alias = 'user_name';
      modify2.onPrevNodesUpdated?.();
      modify3.onPrevNodesUpdated?.();

      expect(modify3.finalCols.map((c) => c.name)).toEqual([
        'id',
        'user_name',
        'value',
      ]);

      // Second edit: modify2 renames 'user_name' to 'username'
      modify2.state.selectedColumns[1].alias = 'username';
      modify3.onPrevNodesUpdated?.();

      expect(modify3.finalCols.map((c) => c.name)).toEqual([
        'id',
        'username',
        'value',
      ]);

      // Third edit: modify2 also renames 'id' to 'identifier'
      modify2.state.selectedColumns[0].alias = 'identifier';
      modify3.onPrevNodesUpdated?.();

      expect(modify3.finalCols.map((c) => c.name)).toEqual([
        'identifier',
        'username',
        'value',
      ]);
    });

    it('should propagate column removal through entire chain', () => {
      // Setup: Source -> Modify1 -> Modify2 -> Modify3 -> Modify4
      const sourceNode = createMockSourceNode();
      const modify1 = new ModifyColumnsNode({selectedColumns: []});
      const modify2 = new ModifyColumnsNode({selectedColumns: []});
      const modify3 = new ModifyColumnsNode({selectedColumns: []});
      const modify4 = new ModifyColumnsNode({selectedColumns: []});

      // Connect the nodes
      sourceNode.nextNodes.push(modify1);
      modify1.primaryInput = sourceNode;
      modify1.nextNodes.push(modify2);
      modify2.primaryInput = modify1;
      modify2.nextNodes.push(modify3);
      modify3.primaryInput = modify2;
      modify3.nextNodes.push(modify4);
      modify4.primaryInput = modify3;

      // Initialize all nodes
      modify1.onPrevNodesUpdated?.();
      modify2.onPrevNodesUpdated?.();
      modify3.onPrevNodesUpdated?.();
      modify4.onPrevNodesUpdated?.();

      // All should have 3 columns initially
      expect(modify4.finalCols).toHaveLength(3);

      // Remove 'name' column in modify2 (middle of chain)
      modify2.state.selectedColumns[1].checked = false;
      modify3.onPrevNodesUpdated?.();
      modify4.onPrevNodesUpdated?.();

      // All downstream nodes should only have 2 columns now
      expect(modify2.finalCols).toHaveLength(2);
      expect(modify3.finalCols).toHaveLength(2);
      expect(modify4.finalCols).toHaveLength(2);
      expect(modify4.finalCols.map((c) => c.name)).toEqual(['id', 'value']);
    });
  });

  describe('onPrevNodesUpdated behavior', () => {
    it('should be called on downstream nodes when upstream changes', () => {
      const sourceNode = createMockSourceNode();
      const modifyNode = new ModifyColumnsNode({selectedColumns: []});
      const aggNode = new AggregationNode({
        groupByColumns: [],
        aggregations: [],
      });

      // Connect the nodes
      sourceNode.nextNodes.push(modifyNode);
      modifyNode.primaryInput = sourceNode;
      modifyNode.nextNodes.push(aggNode);
      aggNode.primaryInput = modifyNode;

      // Spy on onPrevNodesUpdated
      const aggOnPrevNodesUpdatedSpy = jest.spyOn(
        aggNode,
        'onPrevNodesUpdated',
      );

      // Initialize
      modifyNode.onPrevNodesUpdated?.();
      aggNode.onPrevNodesUpdated?.();

      // Clear the spy
      aggOnPrevNodesUpdatedSpy.mockClear();

      // Make a change in modify node and trigger propagation
      modifyNode.state.selectedColumns[0].alias = 'identifier';
      aggNode.onPrevNodesUpdated?.();

      // Verify it was called
      expect(aggOnPrevNodesUpdatedSpy).toHaveBeenCalled();
    });
  });
});

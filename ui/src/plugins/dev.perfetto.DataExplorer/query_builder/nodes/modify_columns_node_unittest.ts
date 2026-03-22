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
  ModifyColumnsNode,
  ModifyColumnsSerializedState,
  ModifyColumnsState,
} from './modify_columns_node';
import {QueryNode} from '../../query_node';
import {
  createMockNode,
  createColumnInfo,
  connectNodes,
} from '../testing/test_utils';
import {PerfettoSqlTypes} from '../../../../trace_processor/perfetto_sql_type';
import {SqlModules} from '../../../dev.perfetto.SqlModules/sql_modules';

describe('ModifyColumnsNode', () => {
  function createMockPrevNode(): QueryNode {
    return createMockNode({
      nodeId: 'mock',
      columns: [
        createColumnInfo('id', 'int'),
        createColumnInfo('status', 'string'),
        createColumnInfo('value', 'int'),
      ],
    });
  }

  function createModifyColumnsNodeWithInput(
    state: ModifyColumnsState,
    inputNode?: QueryNode,
  ): ModifyColumnsNode {
    const node = new ModifyColumnsNode(state);
    if (inputNode) {
      // Directly set the connection without triggering onPrevNodesUpdated
      // to preserve the test's explicitly provided selectedColumns
      connectNodes(inputNode, node);
    }
    return node;
  }

  describe('validation', () => {
    it('should validate when at least one column is selected', () => {
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [createColumnInfo('id', 'int')],
        },
        createMockPrevNode(),
      );

      expect(node.validate()).toBe(true);
    });

    it('should fail validation when no columns selected', () => {
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [],
        },
        createMockPrevNode(),
      );

      // Uncheck all auto-populated columns
      node.state.selectedColumns.forEach((col) => {
        col.checked = false;
      });

      expect(node.validate()).toBe(false);
    });

    it('should allow empty alias (uses original column name)', () => {
      const col = createColumnInfo('id', 'int');
      col.alias = '';
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col],
        },
        createMockPrevNode(),
      );

      // Empty alias is allowed - it just means use the original column name
      expect(node.validate()).toBe(true);
    });

    it('should fail validation for duplicate column names', () => {
      const col1 = createColumnInfo('id', 'int');
      const col2 = createColumnInfo('status', 'string', {alias: 'id'});
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col1, col2],
        },
        createMockPrevNode(),
      );

      expect(node.validate()).toBe(false);
    });

    it('should allow columns with different names', () => {
      const col1 = createColumnInfo('id', 'int');
      const col2 = createColumnInfo('status', 'string', {
        alias: 'status_renamed',
      });
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col1, col2],
        },
        createMockPrevNode(),
      );

      expect(node.validate()).toBe(true);
    });
  });

  describe('serialization', () => {
    it('should serialize selected columns correctly', () => {
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [
            createColumnInfo('id', 'int'),
            createColumnInfo('status', 'string'),
          ],
        },
        createMockPrevNode(),
      );

      const serialized = node.serializeState();

      expect(serialized.selectedColumns).toBeDefined();
      expect(serialized.selectedColumns.length).toBe(2);
      expect(serialized.selectedColumns[0].name).toBe('id');
      expect(serialized.selectedColumns[1].name).toBe('status');
    });

    it('should serialize column aliases correctly', () => {
      const col1 = createColumnInfo('id', 'int');
      const col2 = createColumnInfo('status', 'string', {
        alias: 'status_renamed',
      });
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col1, col2],
        },
        createMockPrevNode(),
      );

      const serialized = node.serializeState();

      expect(serialized.selectedColumns[0].alias).toBeUndefined();
      expect(serialized.selectedColumns[1].alias).toBe('status_renamed');
    });

    it('should serialize checked status correctly', () => {
      const col1 = createColumnInfo('id', 'int');
      const col2 = createColumnInfo('status', 'string', {checked: false});
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col1, col2],
        },
        createMockPrevNode(),
      );

      const serialized = node.serializeState();

      expect(serialized.selectedColumns[0].checked).toBe(true);
      expect(serialized.selectedColumns[1].checked).toBe(false);
    });

    it('should serialize column types as PerfettoSqlType objects', () => {
      const col1 = createColumnInfo('id', 'int');
      const col2 = createColumnInfo('ts', 'timestamp');
      const node = createModifyColumnsNodeWithInput(
        {selectedColumns: [col1, col2]},
        createMockPrevNode(),
      );

      const serialized = node.serializeState();

      expect(serialized.selectedColumns[0].type).toEqual({kind: 'int'});
      expect(serialized.selectedColumns[1].type).toEqual({kind: 'timestamp'});
    });
  });

  describe('legacy deserialization', () => {
    const mockSqlModules = {
      listTables: () => [],
      listModules: () => [],
    } as unknown as SqlModules;

    it('should deserialize legacy string types into PerfettoSqlType', () => {
      // Simulate old serialized state where type was a string like "INT"
      const legacyState = {
        selectedColumns: [
          {name: 'id', type: 'INT', checked: true},
          {name: 'name', type: 'STRING', checked: true},
          {name: 'ts', type: 'TIMESTAMP', checked: false},
        ],
      } as unknown as ModifyColumnsSerializedState;

      const state = ModifyColumnsNode.deserializeState(
        mockSqlModules,
        legacyState,
      );

      expect(state.selectedColumns[0].column.type).toEqual(
        PerfettoSqlTypes.INT,
      );
      expect(state.selectedColumns[1].column.type).toEqual(
        PerfettoSqlTypes.STRING,
      );
      expect(state.selectedColumns[2].column.type).toEqual(
        PerfettoSqlTypes.TIMESTAMP,
      );
    });

    it('should deserialize new PerfettoSqlType objects correctly', () => {
      const newState: ModifyColumnsSerializedState = {
        selectedColumns: [
          {name: 'id', type: {kind: 'int'}, checked: true},
          {name: 'dur', type: {kind: 'duration'}, checked: true},
        ],
      };

      const state = ModifyColumnsNode.deserializeState(
        mockSqlModules,
        newState,
      );

      expect(state.selectedColumns[0].column.type).toEqual(
        PerfettoSqlTypes.INT,
      );
      expect(state.selectedColumns[1].column.type).toEqual(
        PerfettoSqlTypes.DURATION,
      );
    });

    it('should handle undefined types gracefully', () => {
      const stateWithNoTypes = {
        selectedColumns: [{name: 'col1', checked: true}],
      } as unknown as ModifyColumnsSerializedState;

      const state = ModifyColumnsNode.deserializeState(
        mockSqlModules,
        stateWithNoTypes,
      );

      expect(state.selectedColumns[0].column.type).toBeUndefined();
    });

    it('should handle unrecognized legacy string types', () => {
      const stateWithUnknown = {
        selectedColumns: [{name: 'col1', type: 'NA', checked: true}],
      } as unknown as ModifyColumnsSerializedState;

      const state = ModifyColumnsNode.deserializeState(
        mockSqlModules,
        stateWithUnknown,
      );

      expect(state.selectedColumns[0].column.type).toBeUndefined();
    });
  });

  describe('finalCols computation', () => {
    it('should include only checked columns', () => {
      const col1 = createColumnInfo('id', 'int');
      const col2 = createColumnInfo('status', 'string', {checked: false});
      const col3 = createColumnInfo('value', 'int');
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col1, col2, col3],
        },
        createMockPrevNode(),
      );

      const finalCols = node.finalCols;

      expect(finalCols.length).toBe(2);
      expect(finalCols[0].name).toBe('id');
      expect(finalCols[1].name).toBe('value');
    });

    it('should use alias as column name in finalCols', () => {
      const col1 = createColumnInfo('id', 'int', {alias: 'identifier'});
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col1],
        },
        createMockPrevNode(),
      );

      const finalCols = node.finalCols;

      expect(finalCols.length).toBe(1);
      expect(finalCols[0].name).toBe('identifier');
    });
  });

  describe('onPrevNodesUpdated', () => {
    it('should preserve modified column types when input changes', () => {
      // Create initial source node (simulating a slices table)
      const sourceNode = createMockNode({
        nodeId: 'slices',
        columns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('ts', 'timestamp'),
          createColumnInfo('value', 'int'), // Original type is int
        ],
      });

      // Create modify columns node and connect it
      const modifyNode = new ModifyColumnsNode({
        selectedColumns: [],
      });
      connectNodes(sourceNode, modifyNode);

      // Initialize the node to populate columns from source
      modifyNode.onPrevNodesUpdated();

      // Verify initial state - all columns should be from source
      expect(modifyNode.state.selectedColumns.length).toBe(3);
      expect(modifyNode.state.selectedColumns[2].name).toBe('value');
      expect(modifyNode.state.selectedColumns[2].column.type).toEqual(
        PerfettoSqlTypes.INT,
      );

      // User modifies the type of 'value' column to 'duration'
      modifyNode.state.selectedColumns[2].column = {
        ...modifyNode.state.selectedColumns[2].column,
        type: {kind: 'duration'},
      };
      modifyNode.state.selectedColumns[2].typeUserModified = true;

      // Verify the type was modified
      expect(modifyNode.state.selectedColumns[2].column.type).toEqual(
        PerfettoSqlTypes.DURATION,
      );

      // Simulate inserting a new node between source and modify
      // (e.g., user adds a Limit node between slices and modify)
      const intermediateNode = createMockNode({
        nodeId: 'limit',
        columns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('ts', 'timestamp'),
          createColumnInfo('value', 'int'), // Still has original type
        ],
      });

      // Reconnect: source -> intermediate -> modify
      sourceNode.nextNodes = [intermediateNode];
      intermediateNode.nextNodes = [modifyNode];
      modifyNode.primaryInput = intermediateNode;

      // This should trigger when the graph structure changes
      modifyNode.onPrevNodesUpdated();

      // The modified type should be preserved!
      expect(modifyNode.state.selectedColumns[2].name).toBe('value');
      expect(modifyNode.state.selectedColumns[2].column.type).toEqual(
        PerfettoSqlTypes.DURATION,
      );
    });

    it('should preserve checked status and aliases when input changes', () => {
      const sourceNode = createMockNode({
        nodeId: 'source',
        columns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('name', 'string'),
          createColumnInfo('value', 'int'),
        ],
      });

      const modifyNode = new ModifyColumnsNode({
        selectedColumns: [],
      });
      connectNodes(sourceNode, modifyNode);
      modifyNode.onPrevNodesUpdated();

      // User customizes the node
      modifyNode.state.selectedColumns[0].checked = false; // Uncheck 'id'
      modifyNode.state.selectedColumns[1].alias = 'full_name'; // Rename 'name'

      // Insert intermediate node
      const intermediateNode = createMockNode({
        nodeId: 'filter',
        columns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('name', 'string'),
          createColumnInfo('value', 'int'),
        ],
      });

      modifyNode.primaryInput = intermediateNode;
      modifyNode.onPrevNodesUpdated();

      // Customizations should be preserved
      expect(modifyNode.state.selectedColumns[0].checked).toBe(false);
      expect(modifyNode.state.selectedColumns[1].alias).toBe('full_name');
    });
  });

  describe('clone', () => {
    it('should preserve aliases when cloning', () => {
      const col1 = createColumnInfo('id', 'int');
      const col2 = createColumnInfo('name', 'string', {alias: 'full_name'});
      const col3 = createColumnInfo('value', 'int', {alias: 'amount'});
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col1, col2, col3],
        },
        createMockPrevNode(),
      );

      const clonedNode = node.clone() as ModifyColumnsNode;

      // Aliases should be preserved in the cloned node
      expect(clonedNode.state.selectedColumns[0].alias).toBeUndefined();
      expect(clonedNode.state.selectedColumns[1].alias).toBe('full_name');
      expect(clonedNode.state.selectedColumns[2].alias).toBe('amount');
    });

    it('should preserve checked status when cloning', () => {
      const col1 = createColumnInfo('id', 'int');
      const col2 = createColumnInfo('name', 'string', {checked: false});
      const col3 = createColumnInfo('value', 'int');
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col1, col2, col3],
        },
        createMockPrevNode(),
      );

      const clonedNode = node.clone() as ModifyColumnsNode;

      expect(clonedNode.state.selectedColumns[0].checked).toBe(true);
      expect(clonedNode.state.selectedColumns[1].checked).toBe(false);
      expect(clonedNode.state.selectedColumns[2].checked).toBe(true);
    });

    it('should preserve original column names when cloning with aliases', () => {
      // This is a regression test: cloning should NOT apply aliases as new
      // column names. The clone should preserve the exact internal state.
      const col = createColumnInfo('id', 'int', {alias: 'identifier'});
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col],
        },
        createMockPrevNode(),
      );

      const clonedNode = node.clone() as ModifyColumnsNode;

      // The original column name should be preserved, NOT the alias
      expect(clonedNode.state.selectedColumns[0].column.name).toBe('id');
      expect(clonedNode.state.selectedColumns[0].alias).toBe('identifier');
    });

    it('should preserve typeUserModified flag when cloning', () => {
      const col = createColumnInfo('value', 'int');
      col.column = {...col.column, type: PerfettoSqlTypes.DURATION};
      col.typeUserModified = true;
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col],
        },
        createMockPrevNode(),
      );

      const clonedNode = node.clone() as ModifyColumnsNode;

      expect(clonedNode.state.selectedColumns[0].column.type).toEqual(
        PerfettoSqlTypes.DURATION,
      );
      expect(clonedNode.state.selectedColumns[0].typeUserModified).toBe(true);
    });

    it('should create independent copies (mutations do not affect original)', () => {
      const col = createColumnInfo('id', 'int', {alias: 'original_alias'});
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col],
        },
        createMockPrevNode(),
      );

      const clonedNode = node.clone() as ModifyColumnsNode;

      // Modify the cloned node
      clonedNode.state.selectedColumns[0].alias = 'modified_alias';
      clonedNode.state.selectedColumns[0].checked = false;

      // Original should be unchanged
      expect(node.state.selectedColumns[0].alias).toBe('original_alias');
      expect(node.state.selectedColumns[0].checked).toBe(true);
    });
  });
});

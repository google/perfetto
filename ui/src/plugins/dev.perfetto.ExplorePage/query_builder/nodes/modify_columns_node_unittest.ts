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

import {ModifyColumnsNode, ModifyColumnsState} from './modify_columns_node';
import {QueryNode, NodeType} from '../../query_node';
import {ColumnInfo} from '../column_info';

describe('ModifyColumnsNode', () => {
  function createMockPrevNode(): QueryNode {
    return {
      nodeId: 'mock',
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: [
        {
          name: 'id',
          type: 'INT',
          checked: true,
          column: {name: 'id'},
        },
        {
          name: 'status',
          type: 'STRING',
          checked: true,
          column: {name: 'status'},
        },
        {
          name: 'value',
          type: 'INT',
          checked: true,
          column: {name: 'value'},
        },
      ],
      state: {},
      validate: () => true,
      getTitle: () => 'Mock',
      nodeSpecificModify: () => null,
      nodeInfo: () => null,
      clone: () => createMockPrevNode(),
      getStructuredQuery: () => undefined,
      serializeState: () => ({}),
    } as QueryNode;
  }

  function createModifyColumnsNodeWithInput(
    state: ModifyColumnsState,
    inputNode?: QueryNode,
  ): ModifyColumnsNode {
    const node = new ModifyColumnsNode(state);
    if (inputNode) {
      // Directly set the connection without triggering onPrevNodesUpdated
      // to preserve the test's explicitly provided selectedColumns
      inputNode.nextNodes.push(node);
      node.primaryInput = inputNode;
    }
    return node;
  }

  function createColumnInfo(name: string, type: string): ColumnInfo {
    return {
      name,
      type,
      checked: true,
      column: {name},
    };
  }

  describe('validation', () => {
    it('should validate when at least one column is selected', () => {
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [createColumnInfo('id', 'INT')],
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

    it('should fail validation for empty alias', () => {
      const col = createColumnInfo('id', 'INT');
      col.alias = '';
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col],
        },
        createMockPrevNode(),
      );

      expect(node.validate()).toBe(false);
    });

    it('should fail validation for duplicate column names', () => {
      const col1 = createColumnInfo('id', 'INT');
      const col2 = createColumnInfo('status', 'STRING');
      col2.alias = 'id'; // Same as col1's name
      const node = createModifyColumnsNodeWithInput(
        {
          selectedColumns: [col1, col2],
        },
        createMockPrevNode(),
      );

      expect(node.validate()).toBe(false);
    });

    it('should allow columns with different names', () => {
      const col1 = createColumnInfo('id', 'INT');
      const col2 = createColumnInfo('status', 'STRING');
      col2.alias = 'status_renamed';
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
            createColumnInfo('id', 'INT'),
            createColumnInfo('status', 'STRING'),
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
      const col1 = createColumnInfo('id', 'INT');
      const col2 = createColumnInfo('status', 'STRING');
      col2.alias = 'status_renamed';
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
      const col1 = createColumnInfo('id', 'INT');
      const col2 = createColumnInfo('status', 'STRING');
      col2.checked = false;
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
  });

  describe('finalCols computation', () => {
    it('should include only checked columns', () => {
      const col1 = createColumnInfo('id', 'INT');
      const col2 = createColumnInfo('status', 'STRING');
      col2.checked = false;
      const col3 = createColumnInfo('value', 'INT');
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
      const col1 = createColumnInfo('id', 'INT');
      col1.alias = 'identifier';
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
});

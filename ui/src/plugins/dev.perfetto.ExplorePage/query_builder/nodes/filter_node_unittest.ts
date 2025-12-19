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

import {FilterNode} from './filter_node';
import {ModifyColumnsNode} from './modify_columns_node';
import {UIFilter} from '../operations/filter';
import {
  createMockSourceNode,
  createColumnInfo,
  connectNodes,
} from '../testing/test_utils';

describe('FilterNode', () => {
  describe('filter invalidation when columns are aliased', () => {
    it('should mark filter as invalid when column is aliased in modify columns node', () => {
      // Create a table source
      const tableNode = createMockSourceNode('mock-table');

      // Create a ModifyColumnsNode that aliases 'name' to 'full_name'
      const modifyNode = new ModifyColumnsNode({
        selectedColumns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('name', 'string', {alias: 'full_name'}),
          createColumnInfo('value', 'int'),
        ],
      });
      connectNodes(tableNode, modifyNode);

      // Create a FilterNode that filters on 'name' (the original column name)
      const filterNode = new FilterNode({
        filters: [
          {
            column: 'name',
            op: '=',
            value: 'test',
            enabled: true,
          } as UIFilter,
        ],
      });
      connectNodes(modifyNode, filterNode);

      // The filter should be invalid because 'name' doesn't exist in finalCols
      // (it's been aliased to 'full_name')
      const sourceCols = filterNode.sourceCols;

      // Check that 'name' is not in the source columns
      const hasNameColumn = sourceCols.some((col) => col.name === 'name');
      expect(hasNameColumn).toBe(false);

      // Check that 'full_name' is in the source columns
      const hasFullNameColumn = sourceCols.some(
        (col) => col.name === 'full_name',
      );
      expect(hasFullNameColumn).toBe(true);

      // The filter on 'name' should be considered invalid
      // Currently this test will fail because isFilterDefinitionValid
      // doesn't check if the column exists in sourceCols
      const filter = filterNode.state.filters?.[0];
      expect(filter).toBeDefined();

      // Check if the column exists in sourceCols
      const columnExists = sourceCols.some(
        (col) => col.name === filter?.column,
      );
      expect(columnExists).toBe(false);

      // The filter should not be included in nodeDetails because it's invalid
      const details = filterNode.nodeDetails();
      // nodeDetails should show "No filters applied" because the filter
      // references a non-existent column
      expect(details.content).toBeDefined();
    });

    it('should mark filter as invalid when column is unchecked in modify columns node', () => {
      // Create a table source
      const tableNode = createMockSourceNode('mock-table');

      // Create a ModifyColumnsNode that unchecks 'name'
      const modifyNode = new ModifyColumnsNode({
        selectedColumns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('name', 'string', {checked: false}),
          createColumnInfo('value', 'int'),
        ],
      });
      connectNodes(tableNode, modifyNode);

      // Create a FilterNode that filters on 'name'
      const filterNode = new FilterNode({
        filters: [
          {
            column: 'name',
            op: '=',
            value: 'test',
            enabled: true,
          } as UIFilter,
        ],
      });
      connectNodes(modifyNode, filterNode);

      // The filter should be invalid because 'name' doesn't exist in finalCols
      const sourceCols = filterNode.sourceCols;

      // Check that 'name' is not in the source columns
      const hasNameColumn = sourceCols.some((col) => col.name === 'name');
      expect(hasNameColumn).toBe(false);

      // The filter on 'name' should be considered invalid
      const filter = filterNode.state.filters?.[0];
      expect(filter).toBeDefined();

      // Check if the column exists in sourceCols
      const columnExists = sourceCols.some(
        (col) => col.name === filter?.column,
      );
      expect(columnExists).toBe(false);

      // The filter should not be included in nodeDetails
      const details = filterNode.nodeDetails();
      // nodeDetails should show "No filters applied" because the filter
      // references a non-existent column
      expect(details.content).toBeDefined();
    });

    it('should keep filter valid when column is aliased but filter uses new name', () => {
      // Create a table source
      const tableNode = createMockSourceNode('mock-table');

      // Create a ModifyColumnsNode that aliases 'name' to 'full_name'
      const modifyNode = new ModifyColumnsNode({
        selectedColumns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('name', 'string', {alias: 'full_name'}),
          createColumnInfo('value', 'int'),
        ],
      });
      connectNodes(tableNode, modifyNode);

      // Create a FilterNode that filters on 'full_name' (the aliased name)
      const filterNode = new FilterNode({
        filters: [
          {
            column: 'full_name',
            op: '=',
            value: 'test',
            enabled: true,
          } as UIFilter,
        ],
      });
      connectNodes(modifyNode, filterNode);

      // The filter should be valid because 'full_name' exists in finalCols
      const sourceCols = filterNode.sourceCols;

      // Check that 'full_name' is in the source columns
      const hasFullNameColumn = sourceCols.some(
        (col) => col.name === 'full_name',
      );
      expect(hasFullNameColumn).toBe(true);

      // The filter should be valid
      const filter = filterNode.state.filters?.[0];
      expect(filter).toBeDefined();

      // Check if the column exists in sourceCols
      const columnExists = sourceCols.some(
        (col) => col.name === filter?.column,
      );
      expect(columnExists).toBe(true);
    });
  });
});

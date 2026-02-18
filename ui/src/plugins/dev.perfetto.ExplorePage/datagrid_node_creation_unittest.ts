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

import {NodeType, QueryNode} from './query_node';
import {
  parseJoinidColumnField,
  findMatchingAddColumnsNode,
  addFilter,
  DatagridNodeCreationDeps,
} from './datagrid_node_creation';
import {ExplorePageState} from './explore_page';
import {FilterNode} from './query_builder/nodes/filter_node';
import {AddColumnsNode} from './query_builder/nodes/add_columns_node';
import {
  createMockNode,
  createColumnInfo,
  connectNodes,
} from './query_builder/testing/test_utils';
import {ColumnInfo} from './query_builder/column_info';
import {Trace} from '../../public/trace';
import {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {UIFilter} from './query_builder/operations/filter';

describe('datagrid_node_creation', () => {
  // Helper to create a joinid column for testing
  function createJoinidColumn(
    name: string,
    targetTable: string,
    targetColumn: string,
  ): ColumnInfo {
    return {
      name,
      type: 'JOINID',
      checked: true,
      column: {
        name,
        type: {
          kind: 'joinid',
          source: {table: targetTable, column: targetColumn},
        },
      },
    };
  }

  describe('parseJoinidColumnField', () => {
    it('should return undefined for a field without a dot', () => {
      const node = createMockNode({
        nodeId: 'n1',
        columns: [createColumnInfo('name', 'string')],
      });
      const result = parseJoinidColumnField('name', node);
      expect(result).toBeUndefined();
    });

    it('should return undefined when the column is not a joinid type', () => {
      const node = createMockNode({
        nodeId: 'n1',
        columns: [createColumnInfo('parent_id', 'int')],
      });
      const result = parseJoinidColumnField('parent_id.name', node);
      expect(result).toBeUndefined();
    });

    it('should return undefined when the joinid column does not exist', () => {
      const node = createMockNode({
        nodeId: 'n1',
        columns: [createColumnInfo('id', 'int')],
      });
      const result = parseJoinidColumnField('nonexistent.name', node);
      expect(result).toBeUndefined();
    });

    it('should parse a valid joinid column field', () => {
      const node = createMockNode({
        nodeId: 'n1',
        columns: [
          createColumnInfo('id', 'int'),
          createJoinidColumn('track_id', 'track', 'id'),
        ],
      });

      const result = parseJoinidColumnField('track_id.name', node);

      expect(result).toBeDefined();
      expect(result?.joinidColumnName).toBe('track_id');
      expect(result?.targetColumnName).toBe('name');
      expect(result?.targetTable).toBe('track');
      expect(result?.targetJoinColumn).toBe('id');
    });

    it('should handle fields with multiple dots (first dot is the separator)', () => {
      const node = createMockNode({
        nodeId: 'n1',
        columns: [createJoinidColumn('ref', 'other_table', 'pk')],
      });

      const result = parseJoinidColumnField('ref.some.dotted.name', node);

      expect(result).toBeDefined();
      expect(result?.joinidColumnName).toBe('ref');
      expect(result?.targetColumnName).toBe('some.dotted.name');
    });
  });

  describe('findMatchingAddColumnsNode', () => {
    let mockTrace: Trace;
    let mockSqlModules: SqlModules;

    beforeEach(() => {
      mockTrace = {
        traceInfo: {traceTitle: 'test'},
      } as Trace;
      mockSqlModules = {
        listTables: () => [],
        getTable: () => undefined,
        listModules: () => [],
        listTablesNames: () => [],
        getModuleForTable: () => undefined,
      } as unknown as SqlModules;
    });

    it('should return undefined when source node is not an AddColumnsNode', () => {
      const node = createMockNode({nodeId: 'n1', type: NodeType.kTable});
      const result = findMatchingAddColumnsNode(node, 'track_id', 'id');
      expect(result).toBeUndefined();
    });

    it('should return the source node when it is an AddColumnsNode with matching join', () => {
      const addColumnsNode = new AddColumnsNode({
        leftColumn: 'track_id',
        rightColumn: 'id',
        sqlModules: mockSqlModules,
        trace: mockTrace,
      });

      const result = findMatchingAddColumnsNode(
        addColumnsNode,
        'track_id',
        'id',
      );
      expect(result).toBe(addColumnsNode);
    });

    it('should return undefined when source AddColumnsNode has different join columns', () => {
      const addColumnsNode = new AddColumnsNode({
        leftColumn: 'other_column',
        rightColumn: 'pk',
        sqlModules: mockSqlModules,
        trace: mockTrace,
      });

      const result = findMatchingAddColumnsNode(
        addColumnsNode,
        'track_id',
        'id',
      );
      expect(result).toBeUndefined();
    });

    it('should find matching AddColumnsNode in immediate child', () => {
      const parent = createMockNode({nodeId: 'parent', type: NodeType.kTable});
      const addColumnsChild = new AddColumnsNode({
        leftColumn: 'track_id',
        rightColumn: 'id',
        sqlModules: mockSqlModules,
        trace: mockTrace,
      });

      parent.nextNodes = [addColumnsChild as QueryNode];

      const result = findMatchingAddColumnsNode(parent, 'track_id', 'id');
      expect(result).toBe(addColumnsChild);
    });

    it('should return undefined when child AddColumnsNode has different join', () => {
      const parent = createMockNode({nodeId: 'parent', type: NodeType.kTable});
      const addColumnsChild = new AddColumnsNode({
        leftColumn: 'other',
        rightColumn: 'pk',
        sqlModules: mockSqlModules,
        trace: mockTrace,
      });

      parent.nextNodes = [addColumnsChild as QueryNode];

      const result = findMatchingAddColumnsNode(parent, 'track_id', 'id');
      expect(result).toBeUndefined();
    });

    it('should not check children when parent has multiple children', () => {
      const parent = createMockNode({nodeId: 'parent', type: NodeType.kTable});
      const child1 = new AddColumnsNode({
        leftColumn: 'track_id',
        rightColumn: 'id',
        sqlModules: mockSqlModules,
        trace: mockTrace,
      });
      const child2 = createMockNode({nodeId: 'c2', type: NodeType.kFilter});

      parent.nextNodes = [child1 as QueryNode, child2];

      // Should not find it because parent has more than 1 child
      const result = findMatchingAddColumnsNode(parent, 'track_id', 'id');
      expect(result).toBeUndefined();
    });
  });

  describe('addFilter', () => {
    let deps: DatagridNodeCreationDeps;
    let stateUpdates: Array<
      ExplorePageState | ((s: ExplorePageState) => ExplorePageState)
    >;

    beforeEach(() => {
      stateUpdates = [];
      deps = {
        trace: {traceInfo: {traceTitle: 'test'}} as Trace,
        sqlModules: {
          listTables: () => [],
          getTable: () => undefined,
        } as unknown as SqlModules,
        onStateUpdate: (update) => {
          stateUpdates.push(update);
        },
        initializedNodes: new Set<string>(),
        nodeActionHandlers: {
          onAddAndConnectTable: () => {},
          onInsertNodeAtPort: () => {},
        },
      };
    });

    function createTestFilter(column: string, value: string): UIFilter {
      return {
        column,
        op: '=',
        value,
      };
    }

    it('should add filters to a FilterNode source directly', async () => {
      const filterNode = new FilterNode({filters: []});
      const newFilter = createTestFilter('name', 'alice');

      await addFilter(deps, filterNode, newFilter);

      expect(filterNode.state.filters).toHaveLength(1);
      expect(stateUpdates).toHaveLength(1);
    });

    it('should add multiple filters at once to a FilterNode source', async () => {
      const filterNode = new FilterNode({filters: []});
      const filters = [
        createTestFilter('name', 'alice'),
        createTestFilter('name', 'bob'),
      ];

      await addFilter(deps, filterNode, filters);

      expect(filterNode.state.filters).toHaveLength(2);
    });

    it('should set filter operator on a FilterNode source', async () => {
      const filterNode = new FilterNode({filters: []});
      const newFilter = createTestFilter('name', 'alice');

      await addFilter(deps, filterNode, newFilter, 'OR');

      expect(filterNode.state.filterOperator).toBe('OR');
    });

    it('should add filters to child FilterNode when source has one', async () => {
      const source = createMockNode({nodeId: 'src', type: NodeType.kTable});
      const existingFilter = new FilterNode({filters: []});
      connectNodes(source, existingFilter as QueryNode);

      const newFilter = createTestFilter('name', 'alice');
      await addFilter(deps, source, newFilter);

      expect(existingFilter.state.filters).toHaveLength(1);
      // Should select the existing filter node
      expect(stateUpdates).toHaveLength(1);
    });

    it('should create a new FilterNode when source has no filter child', async () => {
      const source = createMockNode({nodeId: 'src', type: NodeType.kTable});
      const newFilter = createTestFilter('name', 'alice');

      await addFilter(deps, source, newFilter);

      // A new node should have been inserted
      expect(source.nextNodes).toHaveLength(1);
      expect(source.nextNodes[0].type).toBe(NodeType.kFilter);
      expect(deps.initializedNodes.size).toBe(1);
      expect(stateUpdates).toHaveLength(1);
    });

    it('should create a new FilterNode with pre-set filter operator', async () => {
      const source = createMockNode({nodeId: 'src', type: NodeType.kTable});
      const newFilter = createTestFilter('name', 'alice');

      await addFilter(deps, source, newFilter, 'OR');

      const createdFilter = source.nextNodes[0] as FilterNode;
      expect(createdFilter.state.filterOperator).toBe('OR');
    });

    it('should not create new FilterNode when source has non-filter child', async () => {
      const source = createMockNode({nodeId: 'src', type: NodeType.kTable});
      const sortChild = createMockNode({
        nodeId: 'sort',
        type: NodeType.kSort,
      });
      connectNodes(source, sortChild);

      const newFilter = createTestFilter('name', 'alice');
      await addFilter(deps, source, newFilter);

      // New filter node should be inserted between source and sort
      expect(source.nextNodes).toHaveLength(1);
      expect(source.nextNodes[0].type).toBe(NodeType.kFilter);
      // Sort should now be downstream of the new filter
      expect(source.nextNodes[0].nextNodes).toContain(sortChild);
    });
  });
});

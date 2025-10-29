// Copyright (C) 2024 The Android Open Source Project
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

import {AggregationNode} from './query_builder/nodes/aggregation_node';
import {ModifyColumnsNode} from './query_builder/nodes/modify_columns_node';
import {ExplorePageState} from './explore_page';
import {IntervalIntersectNode} from './query_builder/nodes/interval_intersect_node';
import {SlicesSourceNode} from './query_builder/nodes/sources/slices_source';
import {SqlSourceNode} from './query_builder/nodes/sources/sql_source';
import {TableSourceNode} from './query_builder/nodes/sources/table_source';
import {serializeState, deserializeState} from './json_handler';
import {Trace} from '../../public/trace';
import {
  SqlModules,
  SqlTable,
  SqlType,
} from '../../plugins/dev.perfetto.SqlModules/sql_modules';
import {AddColumnsNode} from './query_builder/nodes/dev/add_columns_node';
import {LimitAndOffsetNode} from './query_builder/nodes/dev/limit_and_offset_node';
import {SortNode} from './query_builder/nodes/dev/sort_node';

describe('JSON serialization/deserialization', () => {
  let trace: Trace;
  let sqlModules: SqlModules;

  beforeEach(() => {
    trace = {
      traceInfo: {
        traceTitle: 'test_trace',
      },
    } as Trace;

    const stringType: SqlType = {name: 'STRING', shortName: 'string'};
    const timestampType: SqlType = {
      name: 'TIMESTAMP_NS',
      shortName: 'timestamp_ns',
    };

    const tables = new Map<string, SqlTable>();
    tables.set('slice', {
      name: 'slice',
      description: '',
      type: 'table',
      includeKey: '',
      idColumn: undefined,
      linkedIdColumns: [],
      joinIdColumns: [],
      getTableColumns: () => [],
      getIdColumns: () => [],
      getJoinIdColumns: () => [],
      getIdTables: () => [],
      getJoinIdTables: () => [],
      columns: [
        {
          name: 'name',
          type: stringType,
        },
        {
          name: 'ts',
          type: timestampType,
        },
        {
          name: 'dur',
          type: timestampType,
        },
      ],
    });

    sqlModules = {
      getTable: (name: string) => tables.get(name),
    } as SqlModules;
  });

  test('serializes and deserializes a simple graph', () => {
    const sliceNode = new SlicesSourceNode({
      slice_name: 'test_slice',
    });
    const initialState: ExplorePageState = {
      rootNodes: [sliceNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedNode = deserializedState.rootNodes[0];
    expect((deserializedNode as SlicesSourceNode).state.slice_name).toBe(
      'test_slice',
    );
  });

  test('handles multiple nodes and connections', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });
    const modifyNode = new ModifyColumnsNode({
      prevNode: tableNode,
      newColumns: [],
      selectedColumns: [],
    });
    tableNode.nextNodes.push(modifyNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map([
        [tableNode.nodeId, {x: 10, y: 20}],
        [modifyNode.nodeId, {x: 100, y: 200}],
      ]),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedModifyNode = deserializedTableNode.nextNodes[0];
    expect((deserializedModifyNode as ModifyColumnsNode).prevNode?.nodeId).toBe(
      deserializedTableNode.nodeId,
    );
    expect(
      deserializedState.nodeLayouts.get(deserializedTableNode.nodeId),
    ).toEqual({x: 10, y: 20});
  });

  test('serializes and deserializes aggregation node', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const sliceTable = sqlModules.getTable('slice')!;
    const aggregationNode = new AggregationNode({
      prevNode: tableNode,
      groupByColumns: [
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: sliceTable.columns[0],
        },
      ],
      aggregations: [
        {
          column: {
            name: 'dur',
            type: 'TIMESTAMP_NS',
            checked: true,
            column: sliceTable.columns[2],
          },
          aggregationOp: 'SUM',
          newColumnName: 'total_dur',
        },
      ],
    });
    tableNode.nextNodes.push(aggregationNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedAggregationNode = deserializedTableNode
      .nextNodes[0] as AggregationNode;
    expect(deserializedAggregationNode.prevNode?.nodeId).toBe(
      deserializedTableNode.nodeId,
    );
    expect(deserializedAggregationNode.state.groupByColumns[0].name).toBe(
      'name',
    );
    expect(
      deserializedAggregationNode.state.aggregations[0].aggregationOp,
    ).toBe('SUM');
    expect(deserializedAggregationNode.state.aggregations[0].column?.name).toBe(
      'dur',
    );
  });

  test('serializes and deserializes sql source node', () => {
    const sqlNode = new SqlSourceNode({
      sql: 'SELECT * FROM slice',
      trace,
    });
    const initialState: ExplorePageState = {
      rootNodes: [sqlNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedNode = deserializedState.rootNodes[0] as SqlSourceNode;
    expect(deserializedNode.state.sql).toBe('SELECT * FROM slice');
    expect(deserializedNode.prevNodes).toEqual([]);
  });

  test('serializes and deserializes interval intersect node', () => {
    const tableNode1 = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const tableNode2 = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const intervalIntersectNode = new IntervalIntersectNode({
      prevNodes: [tableNode1, tableNode2],
      allNodes: [tableNode1, tableNode2],
    });
    tableNode1.nextNodes.push(intervalIntersectNode);
    tableNode2.nextNodes.push(intervalIntersectNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode1, tableNode2],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(2);
    const deserializedTableNode1 = deserializedState.rootNodes[0];
    const deserializedTableNode2 = deserializedState.rootNodes[1];
    expect(deserializedTableNode1.nextNodes.length).toBe(1);
    const deserializedIntervalIntersectNode = deserializedTableNode1
      .nextNodes[0] as IntervalIntersectNode;
    expect(deserializedIntervalIntersectNode.prevNodes).toBeDefined();
    expect(deserializedIntervalIntersectNode.prevNodes?.length).toBe(2);
    expect(deserializedIntervalIntersectNode.prevNodes?.[0].nodeId).toBe(
      deserializedTableNode1.nodeId,
    );
    expect(deserializedIntervalIntersectNode.prevNodes?.[1].nodeId).toBe(
      deserializedTableNode2.nodeId,
    );
  });

  test('serializes and deserializes sql source node with reference', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const modifyNode = new ModifyColumnsNode({
      prevNode: tableNode,
      newColumns: [],
      selectedColumns: [],
    });
    tableNode.nextNodes.push(modifyNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedModifyNode = deserializedTableNode
      .nextNodes[0] as ModifyColumnsNode;
    expect(deserializedModifyNode.prevNode?.nodeId).toBe(
      deserializedTableNode.nodeId,
    );
  });

  test('serializes and deserializes node with filters', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
      filters: [
        {
          column: 'name',
          op: '=',
          value: 'test',
        },
      ],
    });

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState
      .rootNodes[0] as TableSourceNode;
    expect(deserializedTableNode.state.filters?.length).toBe(1);
    const filter = deserializedTableNode.state.filters?.[0];
    expect(filter?.column).toBe('name');
    expect(filter?.op).toBe('=');
    if (filter !== undefined && 'value' in filter) {
      expect(filter.value).toBe('test');
    } else {
      fail('Filter value not found');
    }
  });

  test('serializes and deserializes node layouts', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const sliceNode = new SlicesSourceNode({
      slice_name: 'test_slice',
    });

    const initialState: ExplorePageState = {
      rootNodes: [tableNode, sliceNode],
      nodeLayouts: new Map([
        [tableNode.nodeId, {x: 10, y: 20}],
        [sliceNode.nodeId, {x: 100, y: 200}],
      ]),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(2);
    const deserializedTableNode = deserializedState.rootNodes[0];
    const deserializedSliceNode = deserializedState.rootNodes[1];

    expect(
      deserializedState.nodeLayouts.get(deserializedTableNode.nodeId),
    ).toEqual({x: 10, y: 20});
    expect(
      deserializedState.nodeLayouts.get(deserializedSliceNode.nodeId),
    ).toEqual({x: 100, y: 200});
  });

  test('serializes and deserializes node with BigInt filter', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
      filters: [
        {
          column: 'id',
          op: '=',
          value: 12345678901234567890n,
        },
      ],
    });

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState
      .rootNodes[0] as TableSourceNode;
    expect(deserializedTableNode.state.filters?.length).toBe(1);
    const filter = deserializedTableNode.state.filters?.[0];
    expect(filter?.column).toBe('id');
    expect(filter?.op).toBe('=');
    if (filter !== undefined && 'value' in filter) {
      expect(filter.value).toBe('12345678901234567890');
    } else {
      fail('Filter value not found');
    }
  });

  test('serializes and deserializes an empty graph', () => {
    const initialState: ExplorePageState = {
      rootNodes: [],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(0);
    expect(deserializedState.nodeLayouts.size).toBe(0);
  });

  test('throws error on invalid json', () => {
    const invalidJson = '{"invalid": "json"}';
    expect(() => deserializeState(invalidJson, trace, sqlModules)).toThrow();
  });

  test('deserializes graph with and without prevNodes', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });
    const modifyNode = new ModifyColumnsNode({
      prevNode: tableNode,
      newColumns: [],
      selectedColumns: [],
    });
    tableNode.nextNodes.push(modifyNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    // Test with prevNode
    const jsonWithPrevNode = serializeState(initialState);
    const deserializedStateWithPrevNode = deserializeState(
      jsonWithPrevNode,
      trace,
      sqlModules,
    );
    const deserializedTableNode1 = deserializedStateWithPrevNode.rootNodes[0];
    const deserializedModifyNode1 = deserializedTableNode1
      .nextNodes[0] as ModifyColumnsNode;
    expect(deserializedModifyNode1.prevNode?.nodeId).toBe(
      deserializedTableNode1.nodeId,
    );
  });

  test('serializes and deserializes modify columns node', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const modifyColumnsNode = new ModifyColumnsNode({
      prevNode: tableNode,
      newColumns: [{expression: '1', name: 'new_col'}],
      selectedColumns: [],
      filters: [
        {
          column: 'new_col',
          op: '=',
          value: '1',
        },
      ],
    });
    tableNode.nextNodes.push(modifyColumnsNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedNode = deserializedTableNode
      .nextNodes[0] as ModifyColumnsNode;
    expect(deserializedNode.state.newColumns.length).toBe(1);
    expect(deserializedNode.state.newColumns[0].name).toBe('new_col');
    const filters = deserializedNode.state.filters;
    expect(filters).toBeDefined();
    if (filters) {
      expect(filters.length).toBe(1);
      expect(filters[0].column).toBe('new_col');
    }
  });

  test('serializes and deserializes add columns node', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const addColumnsNode = new AddColumnsNode({
      prevNode: tableNode,
      sqlTable: sqlModules.getTable('slice')!,
      selectedColumns: ['name'],
    });
    tableNode.nextNodes.push(addColumnsNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedNode = deserializedTableNode
      .nextNodes[0] as AddColumnsNode;
    expect(deserializedNode.state.selectedColumns).toEqual(['name']);
    expect(deserializedNode.state.sqlTable?.name).toEqual('slice');
  });

  test('serializes and deserializes limit and offset node', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const limitAndOffsetNode = new LimitAndOffsetNode({
      prevNode: tableNode,
      limit: 100,
      offset: 20,
    });
    tableNode.nextNodes.push(limitAndOffsetNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedNode = deserializedTableNode
      .nextNodes[0] as LimitAndOffsetNode;
    expect(deserializedNode.state.limit).toEqual(100);
    expect(deserializedNode.state.offset).toEqual(20);
  });

  test('serializes and deserializes sort node', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const sortNode = new SortNode({
      prevNode: tableNode,
      sortColNames: ['name', 'ts'],
    });
    tableNode.nextNodes.push(sortNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedNode = deserializedTableNode.nextNodes[0] as SortNode;
    expect(deserializedNode.state.sortColNames).toEqual(['name', 'ts']);
  });
});

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
import {serializeState, deserializeState, SerializedNode} from './json_handler';
import {Trace} from '../../public/trace';
import {
  SqlModules,
  SqlTable,
} from '../../plugins/dev.perfetto.SqlModules/sql_modules';
import {AddColumnsNode} from './query_builder/nodes/add_columns_node';
import {LimitAndOffsetNode} from './query_builder/nodes/limit_and_offset_node';
import {SortNode} from './query_builder/nodes/sort_node';
import {FilterNode} from './query_builder/nodes/filter_node';
import {MergeNode} from './query_builder/nodes/merge_node';
import {UnionNode} from './query_builder/nodes/union_node';
import {PerfettoSqlType} from '../../trace_processor/perfetto_sql_type';
import {QueryNode, NodeType} from './query_node';

describe('JSON serialization/deserialization', () => {
  let trace: Trace;
  let sqlModules: SqlModules;

  beforeEach(() => {
    trace = {
      traceInfo: {
        traceTitle: 'test_trace',
      },
    } as Trace;

    const stringType: PerfettoSqlType = {kind: 'string'};
    const timestampType: PerfettoSqlType = {
      kind: 'timestamp',
    };

    const tables = new Map<string, SqlTable>();
    tables.set('slice', {
      name: 'slice',
      description: '',
      type: 'table',
      includeKey: '',
      getTableColumns: () => [],
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
    const sliceNode = new SlicesSourceNode({});
    const initialState: ExplorePageState = {
      rootNodes: [sliceNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedNode = deserializedState.rootNodes[0];
    expect(deserializedNode).toBeInstanceOf(SlicesSourceNode);
  });

  test('handles multiple nodes and connections', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });
    const modifyNode = new ModifyColumnsNode({
      prevNode: tableNode,
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

  test('serializes and deserializes interval intersect node with partition columns and filters', () => {
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

    const tableNode3 = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const intervalIntersectNode = new IntervalIntersectNode({
      prevNodes: [tableNode1, tableNode2, tableNode3],
      partitionColumns: ['name'],
      filterNegativeDur: [true, false, true],
      comment: 'Intersect intervals partitioned by name',
    });
    tableNode1.nextNodes.push(intervalIntersectNode);
    tableNode2.nextNodes.push(intervalIntersectNode);
    tableNode3.nextNodes.push(intervalIntersectNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode1, tableNode2, tableNode3],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(3);
    const deserializedTableNode1 = deserializedState.rootNodes[0];
    const deserializedTableNode2 = deserializedState.rootNodes[1];
    const deserializedTableNode3 = deserializedState.rootNodes[2];
    expect(deserializedTableNode1.nextNodes.length).toBe(1);
    const deserializedIntervalIntersectNode = deserializedTableNode1
      .nextNodes[0] as IntervalIntersectNode;

    // Verify prevNodes connections
    expect(deserializedIntervalIntersectNode.prevNodes).toBeDefined();
    expect(deserializedIntervalIntersectNode.prevNodes?.length).toBe(3);
    expect(deserializedIntervalIntersectNode.prevNodes?.[0].nodeId).toBe(
      deserializedTableNode1.nodeId,
    );
    expect(deserializedIntervalIntersectNode.prevNodes?.[1].nodeId).toBe(
      deserializedTableNode2.nodeId,
    );
    expect(deserializedIntervalIntersectNode.prevNodes?.[2].nodeId).toBe(
      deserializedTableNode3.nodeId,
    );

    // Verify partition columns
    expect(
      deserializedIntervalIntersectNode.state.partitionColumns,
    ).toBeDefined();
    expect(
      deserializedIntervalIntersectNode.state.partitionColumns?.length,
    ).toBe(1);
    expect(deserializedIntervalIntersectNode.state.partitionColumns?.[0]).toBe(
      'name',
    );

    // Verify filterNegativeDur array
    expect(
      deserializedIntervalIntersectNode.state.filterNegativeDur,
    ).toBeDefined();
    expect(
      deserializedIntervalIntersectNode.state.filterNegativeDur?.length,
    ).toBe(3);
    expect(deserializedIntervalIntersectNode.state.filterNegativeDur?.[0]).toBe(
      true,
    );
    expect(deserializedIntervalIntersectNode.state.filterNegativeDur?.[1]).toBe(
      false,
    );
    expect(deserializedIntervalIntersectNode.state.filterNegativeDur?.[2]).toBe(
      true,
    );

    // Verify comment
    expect(deserializedIntervalIntersectNode.state.comment).toBe(
      'Intersect intervals partitioned by name',
    );
  });

  test('interval intersect node initializes filter to true by default', () => {
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

    // Create interval intersect node WITHOUT specifying filterNegativeDur
    const intervalIntersectNode = new IntervalIntersectNode({
      prevNodes: [tableNode1, tableNode2],
    });
    tableNode1.nextNodes.push(intervalIntersectNode);

    // Verify that filterNegativeDur is initialized to true for all inputs
    // This is the key fix - the array should be initialized with explicit true values
    // so the UI checkbox state matches the actual filter behavior
    expect(intervalIntersectNode.state.filterNegativeDur).toBeDefined();
    expect(intervalIntersectNode.state.filterNegativeDur?.length).toBe(2);
    expect(intervalIntersectNode.state.filterNegativeDur?.[0]).toBe(true);
    expect(intervalIntersectNode.state.filterNegativeDur?.[1]).toBe(true);

    // Serialize and deserialize to ensure the filter persists
    const initialState: ExplorePageState = {
      rootNodes: [tableNode1, tableNode2],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    const deserializedTableNode1 = deserializedState.rootNodes[0];
    const deserializedIntervalIntersectNode = deserializedTableNode1
      .nextNodes[0] as IntervalIntersectNode;

    // Verify filterNegativeDur is still true after deserialization
    expect(
      deserializedIntervalIntersectNode.state.filterNegativeDur,
    ).toBeDefined();
    expect(
      deserializedIntervalIntersectNode.state.filterNegativeDur?.length,
    ).toBe(2);
    expect(deserializedIntervalIntersectNode.state.filterNegativeDur?.[0]).toBe(
      true,
    );
    expect(deserializedIntervalIntersectNode.state.filterNegativeDur?.[1]).toBe(
      true,
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
    });

    const filterNode = new FilterNode({
      prevNode: tableNode,
      filters: [
        {
          column: 'name',
          op: '=',
          value: 'test',
        },
      ],
    });
    tableNode.nextNodes.push(filterNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedFilterNode = deserializedTableNode
      .nextNodes[0] as FilterNode;
    expect(deserializedFilterNode.state.filters?.length).toBe(1);
    const filter = deserializedFilterNode.state.filters?.[0];
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

    const sliceNode = new SlicesSourceNode({});

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
    });

    const filterNode = new FilterNode({
      prevNode: tableNode,
      filters: [
        {
          column: 'id',
          op: '=',
          value: 12345678901234567890n,
        },
      ],
    });
    tableNode.nextNodes.push(filterNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedFilterNode = deserializedTableNode
      .nextNodes[0] as FilterNode;
    expect(deserializedFilterNode.state.filters?.length).toBe(1);
    const filter = deserializedFilterNode.state.filters?.[0];
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
      selectedColumns: [],
    });
    tableNode.nextNodes.push(modifyColumnsNode);

    const filterNode = new FilterNode({
      prevNode: modifyColumnsNode,
      filters: [
        {
          column: 'new_col',
          op: '=',
          value: '1',
        },
      ],
    });
    modifyColumnsNode.nextNodes.push(filterNode);

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
    expect(deserializedModifyNode.nextNodes.length).toBe(1);
    const deserializedFilterNode = deserializedModifyNode
      .nextNodes[0] as FilterNode;
    expect(deserializedFilterNode.state.filters).toBeDefined();
    expect(deserializedFilterNode.state.filters?.length).toBe(1);
    expect(deserializedFilterNode.state.filters?.[0].column).toBe('new_col');
  });

  test('serializes and deserializes add columns node', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const addColumnsNode = new AddColumnsNode({
      prevNode: tableNode,
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
    // Note: sqlTable is no longer part of AddColumnsNodeState
  });

  test('serializes and deserializes add columns node with inputNodes connection', () => {
    // Create the main data flow: tableNode1 -> addColumnsNode
    const tableNode1 = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const addColumnsNode = new AddColumnsNode({
      prevNode: tableNode1,
      selectedColumns: ['name', 'ts'],
      leftColumn: 'id',
      rightColumn: 'id',
    });
    tableNode1.nextNodes.push(addColumnsNode);

    // Create the side input: tableNode2 connected to inputNodes[0]
    const tableNode2 = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    // Connect tableNode2 to addColumnsNode's inputNodes[0] (left-side port)
    if (!addColumnsNode.inputNodes) {
      addColumnsNode.inputNodes = [];
    }
    addColumnsNode.inputNodes[0] = tableNode2;
    tableNode2.nextNodes.push(addColumnsNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode1, tableNode2],
      nodeLayouts: new Map([
        [tableNode1.nodeId, {x: 0, y: 0}],
        [addColumnsNode.nodeId, {x: 0, y: 100}],
        [tableNode2.nodeId, {x: -200, y: 100}],
      ]),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    // Verify root nodes
    expect(deserializedState.rootNodes.length).toBe(2);
    const deserializedTableNode1 = deserializedState.rootNodes[0];
    const deserializedTableNode2 = deserializedState.rootNodes[1];

    // Verify main connection (prevNode)
    expect(deserializedTableNode1.nextNodes.length).toBe(1);
    const deserializedAddColumnsNode = deserializedTableNode1
      .nextNodes[0] as AddColumnsNode;
    expect(deserializedAddColumnsNode.prevNode?.nodeId).toBe(
      deserializedTableNode1.nodeId,
    );

    // Verify inputNodes connection (THIS IS THE BUG - this will fail before the fix)
    expect(deserializedAddColumnsNode.inputNodes).toBeDefined();
    expect(deserializedAddColumnsNode.inputNodes?.length).toBeGreaterThan(0);
    expect(deserializedAddColumnsNode.inputNodes?.[0]?.nodeId).toBe(
      deserializedTableNode2.nodeId,
    );

    // Verify tableNode2 has the connection back to addColumnsNode
    expect(deserializedTableNode2.nextNodes.length).toBe(1);
    expect(deserializedTableNode2.nextNodes[0].nodeId).toBe(
      deserializedAddColumnsNode.nodeId,
    );

    // Verify node state
    expect(deserializedAddColumnsNode.state.selectedColumns).toEqual([
      'name',
      'ts',
    ]);
    expect(deserializedAddColumnsNode.state.leftColumn).toBe('id');
    expect(deserializedAddColumnsNode.state.rightColumn).toBe('id');

    // Verify layouts are preserved
    expect(
      deserializedState.nodeLayouts.get(deserializedTableNode1.nodeId),
    ).toEqual({x: 0, y: 0});
    expect(
      deserializedState.nodeLayouts.get(deserializedAddColumnsNode.nodeId),
    ).toEqual({x: 0, y: 100});
    expect(
      deserializedState.nodeLayouts.get(deserializedTableNode2.nodeId),
    ).toEqual({x: -200, y: 100});
  });

  test('add columns node uses renamed columns from modify columns node', () => {
    // Create table source
    const tableNode1 = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    // Get all columns from the table to find one to rename
    const allColumns = tableNode1.finalCols;
    // Find a column to rename (let's use the first one)
    const columnToRename = allColumns[0];

    // Create modify columns node that renames a column using alias
    const selectedColumnsWithAlias = [
      {
        ...allColumns[0],
        alias: 'renamed_column',
        checked: true,
      },
      ...allColumns.slice(1, 3).map((col) => ({...col, checked: true})),
    ];

    const modifyColumnsNode = new ModifyColumnsNode({
      prevNode: tableNode1,
      selectedColumns: selectedColumnsWithAlias,
    });
    tableNode1.nextNodes.push(modifyColumnsNode);

    // Create another table to join with
    const tableNode2 = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    // Create add columns node that should see the renamed column 'duration_ns'
    const addColumnsNode = new AddColumnsNode({
      prevNode: tableNode2,
      selectedColumns: [],
    });
    tableNode2.nextNodes.push(addColumnsNode);

    // Connect modifyColumnsNode to addColumnsNode's inputNodes[0]
    if (!addColumnsNode.inputNodes) {
      addColumnsNode.inputNodes = [];
    }
    addColumnsNode.inputNodes[0] = modifyColumnsNode;
    modifyColumnsNode.nextNodes.push(addColumnsNode);

    // Check that addColumnsNode can see the renamed column
    const rightCols = addColumnsNode.rightCols;
    const colNames = rightCols.map((c) => c.column.name);

    // Should see the renamed column
    expect(colNames).toContain('renamed_column');
    // Should NOT see the original column name
    expect(colNames).not.toContain(columnToRename.column.name);

    // Verify the type is preserved for the renamed column
    const renamedCol = rightCols.find(
      (c) => c.column.name === 'renamed_column',
    );
    expect(renamedCol).toBeDefined();
    expect(renamedCol?.type).toBe(columnToRename.type);
    expect(renamedCol?.type).not.toBe('NA'); // Type should be preserved, not 'NA'

    // Now test serialization/deserialization preserves this
    const initialState: ExplorePageState = {
      rootNodes: [tableNode1, tableNode2],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    // Find the deserialized add columns node
    const deserializedTableNode2 = deserializedState.rootNodes.find(
      (n) => n.nodeId === tableNode2.nodeId,
    );
    expect(deserializedTableNode2).toBeDefined();
    expect(deserializedTableNode2!.nextNodes.length).toBe(1);

    const deserializedAddColumnsNode = deserializedTableNode2!
      .nextNodes[0] as AddColumnsNode;

    // Verify the connection is restored
    expect(deserializedAddColumnsNode.inputNodes).toBeDefined();
    expect(deserializedAddColumnsNode.inputNodes?.length).toBeGreaterThan(0);
    expect(deserializedAddColumnsNode.inputNodes?.[0]).toBeDefined();

    // Most importantly: verify that renamed columns are still accessible
    const deserializedRightCols = deserializedAddColumnsNode.rightCols;
    const deserializedColNames = deserializedRightCols.map(
      (c) => c.column.name,
    );

    expect(deserializedColNames).toContain('renamed_column');
    expect(deserializedColNames).not.toContain(columnToRename.column.name);

    // Verify the type is preserved after serialization/deserialization
    const deserializedRenamedCol = deserializedRightCols.find(
      (c) => c.column.name === 'renamed_column',
    );
    expect(deserializedRenamedCol).toBeDefined();
    expect(deserializedRenamedCol?.type).toBe(columnToRename.type);
    expect(deserializedRenamedCol?.type).not.toBe('NA'); // Type should still be preserved
  });

  test('aggregation node can group by aliased column from modify columns node', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    // Create a ModifyColumnsNode that aliases a column
    const modifyNode = new ModifyColumnsNode({
      prevNode: tableNode,
      selectedColumns: tableNode.finalCols.map((col) => {
        // Alias the 'ts' column to 'timestamp_alias'
        if (col.name === 'ts') {
          return {...col, alias: 'timestamp_alias'};
        }
        return col;
      }),
    });
    tableNode.nextNodes.push(modifyNode);

    // Verify that the alias is in finalCols and the original name is not
    const tsColumn = modifyNode.finalCols.find((c) => c.name === 'ts');
    const aliasedColumn = modifyNode.finalCols.find(
      (c) => c.name === 'timestamp_alias',
    );
    expect(tsColumn).toBeUndefined(); // Original name should not be visible
    expect(aliasedColumn).toBeDefined(); // Aliased name should be visible
    expect(aliasedColumn?.column.name).toBe('timestamp_alias'); // column.name should also use alias

    // Create an AggregationNode that groups by the aliased column
    const aggregationNode = new AggregationNode({
      prevNode: modifyNode,
      groupByColumns: [aliasedColumn!],
      aggregations: [],
    });
    modifyNode.nextNodes.push(aggregationNode);

    // Verify the aggregation node sees the aliased column
    expect(aggregationNode.state.groupByColumns.length).toBe(1);
    expect(aggregationNode.state.groupByColumns[0].name).toBe(
      'timestamp_alias',
    );
    expect(aggregationNode.state.groupByColumns[0].column.name).toBe(
      'timestamp_alias',
    );

    // Serialize and deserialize
    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    // Navigate to the deserialized aggregation node
    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);

    const deserializedModifyNode = deserializedTableNode.nextNodes[0] as
      | ModifyColumnsNode
      | undefined;
    expect(deserializedModifyNode).toBeDefined();
    expect(deserializedModifyNode?.type).toBe(NodeType.kModifyColumns);

    // Verify the alias is still in finalCols after deserialization
    const deserializedTsColumn = deserializedModifyNode?.finalCols.find(
      (c) => c.name === 'ts',
    );
    const deserializedAliasedColumn = deserializedModifyNode?.finalCols.find(
      (c) => c.name === 'timestamp_alias',
    );
    expect(deserializedTsColumn).toBeUndefined(); // Original name should not be visible
    expect(deserializedAliasedColumn).toBeDefined(); // Aliased name should be visible
    expect(deserializedAliasedColumn?.column.name).toBe('timestamp_alias');

    // Verify the aggregation node still sees the aliased column
    expect(deserializedModifyNode?.nextNodes.length).toBe(1);
    const deserializedAggNode = deserializedModifyNode?.nextNodes[0] as
      | AggregationNode
      | undefined;
    expect(deserializedAggNode).toBeDefined();
    expect(deserializedAggNode?.type).toBe(NodeType.kAggregation);
    expect(deserializedAggNode?.state.groupByColumns.length).toBe(1);
    expect(deserializedAggNode?.state.groupByColumns[0].name).toBe(
      'timestamp_alias',
    );
    expect(deserializedAggNode?.state.groupByColumns[0].column.name).toBe(
      'timestamp_alias',
    );
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

  test('serializes and deserializes filter node', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const filterNode = new FilterNode({
      prevNode: tableNode,
      filters: [
        {
          column: 'name',
          op: '=',
          value: 'test',
        },
        {
          column: 'dur',
          op: '>',
          value: 1000,
        },
      ],
      filterOperator: 'AND',
      comment: 'Filter by name and duration',
    });
    tableNode.nextNodes.push(filterNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedNode = deserializedTableNode.nextNodes[0] as FilterNode;

    // Verify filters
    expect(deserializedNode.state.filters).toBeDefined();
    expect(deserializedNode.state.filters?.length).toBe(2);
    expect(deserializedNode.state.filters?.[0].column).toBe('name');
    expect(deserializedNode.state.filters?.[0].op).toBe('=');
    if (
      deserializedNode.state.filters?.[0] &&
      'value' in deserializedNode.state.filters[0]
    ) {
      expect(deserializedNode.state.filters[0].value).toBe('test');
    }
    expect(deserializedNode.state.filters?.[1].column).toBe('dur');
    expect(deserializedNode.state.filters?.[1].op).toBe('>');
    if (
      deserializedNode.state.filters?.[1] &&
      'value' in deserializedNode.state.filters[1]
    ) {
      expect(deserializedNode.state.filters[1].value).toBe(1000);
    }

    // Verify filter operator
    expect(deserializedNode.state.filterOperator).toBe('AND');

    // Verify comment
    expect(deserializedNode.state.comment).toBe('Filter by name and duration');

    // Verify prevNode connection
    expect(deserializedNode.prevNode?.nodeId).toBe(
      deserializedTableNode.nodeId,
    );
  });

  test('serializes and deserializes filter node with null filter', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const filterNode = new FilterNode({
      prevNode: tableNode,
      filters: [
        {
          column: 'name',
          op: 'is null',
        },
      ],
    });
    tableNode.nextNodes.push(filterNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(1);
    const deserializedNode = deserializedTableNode.nextNodes[0] as FilterNode;

    // Verify filter
    expect(deserializedNode.state.filters).toBeDefined();
    expect(deserializedNode.state.filters?.length).toBe(1);
    expect(deserializedNode.state.filters?.[0].column).toBe('name');
    expect(deserializedNode.state.filters?.[0].op).toBe('is null');
    // Null filters don't have a value property
    expect('value' in deserializedNode.state.filters![0]).toBe(false);
  });

  test('serializes and deserializes merge node with equality condition', () => {
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

    const mergeNode = new MergeNode({
      prevNodes: [tableNode1, tableNode2],
      leftQueryAlias: 'left',
      rightQueryAlias: 'right',
      conditionType: 'equality',
      leftColumn: 'name',
      rightColumn: 'name',
      sqlExpression: '',
    });
    tableNode1.nextNodes.push(mergeNode);
    tableNode2.nextNodes.push(mergeNode);

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
    const deserializedMergeNode = deserializedTableNode1
      .nextNodes[0] as MergeNode;
    expect(deserializedMergeNode.prevNodes).toBeDefined();
    expect(deserializedMergeNode.prevNodes?.length).toBe(2);
    expect(deserializedMergeNode.prevNodes?.[0].nodeId).toBe(
      deserializedTableNode1.nodeId,
    );
    expect(deserializedMergeNode.prevNodes?.[1].nodeId).toBe(
      deserializedTableNode2.nodeId,
    );
    expect(deserializedMergeNode.state.leftQueryAlias).toBe('left');
    expect(deserializedMergeNode.state.rightQueryAlias).toBe('right');
    expect(deserializedMergeNode.state.conditionType).toBe('equality');
    expect(deserializedMergeNode.state.leftColumn).toBe('name');
    expect(deserializedMergeNode.state.rightColumn).toBe('name');
  });

  test('serializes and deserializes merge node with freeform condition', () => {
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

    const mergeNode = new MergeNode({
      prevNodes: [tableNode1, tableNode2],
      leftQueryAlias: 't1',
      rightQueryAlias: 't2',
      conditionType: 'freeform',
      leftColumn: '',
      rightColumn: '',
      sqlExpression: 't1.id = t2.parent_id',
    });
    tableNode1.nextNodes.push(mergeNode);
    tableNode2.nextNodes.push(mergeNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode1, tableNode2],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(2);
    const deserializedTableNode1 = deserializedState.rootNodes[0];
    expect(deserializedTableNode1.nextNodes.length).toBe(1);
    const deserializedMergeNode = deserializedTableNode1
      .nextNodes[0] as MergeNode;
    expect(deserializedMergeNode.state.leftQueryAlias).toBe('t1');
    expect(deserializedMergeNode.state.rightQueryAlias).toBe('t2');
    expect(deserializedMergeNode.state.conditionType).toBe('freeform');
    expect(deserializedMergeNode.state.sqlExpression).toBe(
      't1.id = t2.parent_id',
    );
  });

  test('serializes and deserializes union node', () => {
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

    const tableNode3 = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const sliceTable = sqlModules.getTable('slice')!;
    const unionNode = new UnionNode({
      prevNodes: [tableNode1, tableNode2, tableNode3],
      selectedColumns: [
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: sliceTable.columns[0],
        },
        {
          name: 'ts',
          type: 'TIMESTAMP_NS',
          checked: true,
          column: sliceTable.columns[1],
        },
      ],
    });
    tableNode1.nextNodes.push(unionNode);
    tableNode2.nextNodes.push(unionNode);
    tableNode3.nextNodes.push(unionNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode1, tableNode2, tableNode3],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(3);
    const deserializedTableNode1 = deserializedState.rootNodes[0];
    const deserializedTableNode2 = deserializedState.rootNodes[1];
    const deserializedTableNode3 = deserializedState.rootNodes[2];
    expect(deserializedTableNode1.nextNodes.length).toBe(1);
    const deserializedUnionNode = deserializedTableNode1
      .nextNodes[0] as UnionNode;
    expect(deserializedUnionNode.prevNodes).toBeDefined();
    expect(deserializedUnionNode.prevNodes?.length).toBe(3);
    expect(deserializedUnionNode.prevNodes?.[0].nodeId).toBe(
      deserializedTableNode1.nodeId,
    );
    expect(deserializedUnionNode.prevNodes?.[1].nodeId).toBe(
      deserializedTableNode2.nodeId,
    );
    expect(deserializedUnionNode.prevNodes?.[2].nodeId).toBe(
      deserializedTableNode3.nodeId,
    );
    expect(deserializedUnionNode.state.selectedColumns.length).toBe(2);
    expect(deserializedUnionNode.state.selectedColumns[0].name).toBe('name');
    expect(deserializedUnionNode.state.selectedColumns[0].checked).toBe(true);
    expect(deserializedUnionNode.state.selectedColumns[1].name).toBe('ts');
    expect(deserializedUnionNode.state.selectedColumns[1].checked).toBe(true);
  });

  test('serializes and deserializes merge node with filters and comment', () => {
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

    const mergeNode = new MergeNode({
      prevNodes: [tableNode1, tableNode2],
      leftQueryAlias: 'left',
      rightQueryAlias: 'right',
      conditionType: 'equality',
      leftColumn: 'name',
      rightColumn: 'name',
      sqlExpression: '',
      comment: 'Join on matching names with duration filter',
    });
    tableNode1.nextNodes.push(mergeNode);
    tableNode2.nextNodes.push(mergeNode);

    const filterNode = new FilterNode({
      prevNode: mergeNode,
      filters: [
        {
          column: 'dur',
          op: '>',
          value: '1000',
        },
      ],
    });
    mergeNode.nextNodes.push(filterNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode1, tableNode2],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(2);
    const deserializedTableNode1 = deserializedState.rootNodes[0];
    expect(deserializedTableNode1.nextNodes.length).toBe(1);
    const deserializedMergeNode = deserializedTableNode1
      .nextNodes[0] as MergeNode;
    expect(deserializedMergeNode.state.comment).toBe(
      'Join on matching names with duration filter',
    );
    expect(deserializedMergeNode.nextNodes.length).toBe(1);
    const deserializedFilterNode = deserializedMergeNode
      .nextNodes[0] as FilterNode;
    expect(deserializedFilterNode.state.filters).toBeDefined();
    expect(deserializedFilterNode.state.filters?.length).toBe(1);
    expect(deserializedFilterNode.state.filters?.[0].column).toBe('dur');
    expect(deserializedFilterNode.state.filters?.[0].op).toBe('>');
  });

  test('serializes and deserializes union node with filters and comment', () => {
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

    const sliceTable = sqlModules.getTable('slice')!;
    const unionNode = new UnionNode({
      prevNodes: [tableNode1, tableNode2],
      selectedColumns: [
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: sliceTable.columns[0],
        },
        {
          name: 'ts',
          type: 'TIMESTAMP_NS',
          checked: false,
          column: sliceTable.columns[1],
        },
      ],
    });
    unionNode.comment = 'Union of slice sources excluding idle';
    tableNode1.nextNodes.push(unionNode);
    tableNode2.nextNodes.push(unionNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode1, tableNode2],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(2);
    const deserializedTableNode1 = deserializedState.rootNodes[0];
    expect(deserializedTableNode1.nextNodes.length).toBe(1);
    const deserializedUnionNode = deserializedTableNode1
      .nextNodes[0] as UnionNode;
    expect(deserializedUnionNode.comment).toBe(
      'Union of slice sources excluding idle',
    );
    // Verify unchecked column is preserved
    expect(deserializedUnionNode.state.selectedColumns[1].checked).toBe(false);
  });

  test('merge node filters duplicate columns and includes equality columns', () => {
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

    // Both tables have the same columns: name, ts, dur
    // When joining on 'name', the final columns should:
    // - Include 'name' once (the equality column)
    // - Exclude 'ts' and 'dur' (duplicated across both inputs)
    const mergeNode = new MergeNode({
      prevNodes: [tableNode1, tableNode2],
      leftQueryAlias: 'left',
      rightQueryAlias: 'right',
      conditionType: 'equality',
      leftColumn: 'name',
      rightColumn: 'name',
      sqlExpression: '',
    });
    tableNode1.nextNodes.push(mergeNode);
    tableNode2.nextNodes.push(mergeNode);

    // Verify finalCols behavior
    const finalCols = mergeNode.finalCols;
    const colNames = finalCols.map((c) => c.name);

    // Should include the equality column once
    expect(colNames).toContain('name');
    expect(colNames.filter((n) => n === 'name').length).toBe(1);

    // Should NOT include duplicated columns (ts, dur appear in both tables)
    expect(colNames).not.toContain('ts');
    expect(colNames).not.toContain('dur');

    // Verify the structured query includes select_columns
    const sq = mergeNode.getStructuredQuery();
    expect(sq).toBeDefined();
    expect(sq?.selectColumns).toBeDefined();
    expect(sq?.selectColumns?.length).toBe(finalCols.length);
    expect(sq?.selectColumns?.map((c) => c.columnNameOrExpression)).toEqual(
      colNames,
    );
  });

  test('serializes and deserializes modify columns node with selected columns', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const sliceTable = sqlModules.getTable('slice')!;
    const modifyColumnsNode = new ModifyColumnsNode({
      prevNode: tableNode,
      selectedColumns: [
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: sliceTable.columns[0],
        },
        {
          name: 'ts',
          type: 'TIMESTAMP_NS',
          checked: true,
          column: sliceTable.columns[1],
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
    expect(deserializedNode.state.selectedColumns.length).toBe(2);
    expect(deserializedNode.state.selectedColumns[0].name).toBe('name');
    expect(deserializedNode.state.selectedColumns[1].name).toBe('ts');
  });

  test('serializes modify columns node without prevNode', () => {
    // Create a modify columns node without a prevNode (edge case)
    const modifyColumnsNode = new ModifyColumnsNode({
      prevNode: undefined as unknown as QueryNode,
      selectedColumns: [],
    });

    const initialState: ExplorePageState = {
      rootNodes: [modifyColumnsNode],
      nodeLayouts: new Map(),
    };

    // Should be able to serialize without throwing
    const json = serializeState(initialState);
    const serialized = JSON.parse(json);

    // Verify prevNodeId is undefined in serialized state
    const serializedNode = serialized.nodes.find(
      (n: SerializedNode) => n.nodeId === modifyColumnsNode.nodeId,
    );
    expect(serializedNode).toBeDefined();
    expect(serializedNode.state.prevNodeId).toBeUndefined();
  });

  test('serializes and deserializes aggregation node with multiple aggregations', () => {
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
        {
          name: 'ts',
          type: 'TIMESTAMP_NS',
          checked: true,
          column: sliceTable.columns[1],
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
        {
          column: {
            name: 'dur',
            type: 'TIMESTAMP_NS',
            checked: true,
            column: sliceTable.columns[2],
          },
          aggregationOp: 'AVG',
          newColumnName: 'avg_dur',
        },
        {
          column: {
            name: 'dur',
            type: 'TIMESTAMP_NS',
            checked: true,
            column: sliceTable.columns[2],
          },
          aggregationOp: 'COUNT',
          newColumnName: 'count',
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
    expect(deserializedAggregationNode.state.groupByColumns.length).toBe(2);
    expect(deserializedAggregationNode.state.groupByColumns[1].name).toBe('ts');
    expect(deserializedAggregationNode.state.aggregations.length).toBe(3);
    expect(
      deserializedAggregationNode.state.aggregations[0].aggregationOp,
    ).toBe('SUM');
    expect(
      deserializedAggregationNode.state.aggregations[0].newColumnName,
    ).toBe('total_dur');
    expect(
      deserializedAggregationNode.state.aggregations[1].aggregationOp,
    ).toBe('AVG');
    expect(
      deserializedAggregationNode.state.aggregations[1].newColumnName,
    ).toBe('avg_dur');
    expect(
      deserializedAggregationNode.state.aggregations[2].aggregationOp,
    ).toBe('COUNT');
  });

  test('serializes and deserializes complex multi-node chain', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const filterNode = new FilterNode({
      prevNode: tableNode,
      filters: [
        {
          column: 'name',
          op: '!=',
          value: 'idle',
        },
      ],
    });
    tableNode.nextNodes.push(filterNode);

    const sliceTable = sqlModules.getTable('slice')!;
    const modifyNode = new ModifyColumnsNode({
      prevNode: filterNode,
      selectedColumns: [
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: sliceTable.columns[0],
        },
      ],
    });
    filterNode.nextNodes.push(modifyNode);

    const aggregationNode = new AggregationNode({
      prevNode: modifyNode,
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
            name: 'dur_ms',
            type: 'TIMESTAMP_NS',
            checked: true,
            column: sliceTable.columns[2],
          },
          aggregationOp: 'SUM',
          newColumnName: 'total_dur_ms',
        },
      ],
    });
    modifyNode.nextNodes.push(aggregationNode);

    const sortNode = new SortNode({
      prevNode: aggregationNode,
      sortColNames: ['total_dur_ms'],
    });
    aggregationNode.nextNodes.push(sortNode);

    const limitNode = new LimitAndOffsetNode({
      prevNode: sortNode,
      limit: 10,
      offset: 0,
    });
    sortNode.nextNodes.push(limitNode);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map([
        [tableNode.nodeId, {x: 0, y: 0}],
        [filterNode.nodeId, {x: 0, y: 50}],
        [modifyNode.nodeId, {x: 0, y: 100}],
        [aggregationNode.nodeId, {x: 0, y: 200}],
        [sortNode.nodeId, {x: 0, y: 300}],
        [limitNode.nodeId, {x: 0, y: 400}],
      ]),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    // Verify chain structure
    expect(deserializedState.rootNodes.length).toBe(1);
    const node1 = deserializedState.rootNodes[0] as TableSourceNode;
    expect(node1.nextNodes.length).toBe(1);

    const node2 = node1.nextNodes[0] as FilterNode;
    expect(node2.prevNode?.nodeId).toBe(node1.nodeId);
    expect(node2.state.filters?.length).toBe(1);
    expect(node2.nextNodes.length).toBe(1);

    const node3 = node2.nextNodes[0] as ModifyColumnsNode;
    expect(node3.prevNode?.nodeId).toBe(node2.nodeId);
    expect(node3.nextNodes.length).toBe(1);

    const node4 = node3.nextNodes[0] as AggregationNode;
    expect(node4.prevNode?.nodeId).toBe(node3.nodeId);
    expect(node4.nextNodes.length).toBe(1);

    const node5 = node4.nextNodes[0] as SortNode;
    expect(node5.prevNode?.nodeId).toBe(node4.nodeId);
    expect(node5.nextNodes.length).toBe(1);

    const node6 = node5.nextNodes[0] as LimitAndOffsetNode;
    expect(node6.prevNode?.nodeId).toBe(node5.nodeId);
    expect(node6.state.limit).toBe(10);

    // Verify all layouts preserved
    expect(deserializedState.nodeLayouts.size).toBe(6);
  });

  test('serializes and deserializes branching graph', () => {
    const tableNode = new TableSourceNode({
      sqlTable: sqlModules.getTable('slice'),
      trace,
      sqlModules,
    });

    const sliceTable = sqlModules.getTable('slice')!;
    const modifyNode1 = new ModifyColumnsNode({
      prevNode: tableNode,
      selectedColumns: [
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: sliceTable.columns[0],
        },
      ],
    });

    const modifyNode2 = new ModifyColumnsNode({
      prevNode: tableNode,
      selectedColumns: [
        {
          name: 'ts',
          type: 'TIMESTAMP_NS',
          checked: true,
          column: sliceTable.columns[1],
        },
      ],
    });

    tableNode.nextNodes.push(modifyNode1, modifyNode2);

    const initialState: ExplorePageState = {
      rootNodes: [tableNode],
      nodeLayouts: new Map(),
    };

    const json = serializeState(initialState);
    const deserializedState = deserializeState(json, trace, sqlModules);

    expect(deserializedState.rootNodes.length).toBe(1);
    const deserializedTableNode = deserializedState.rootNodes[0];
    expect(deserializedTableNode.nextNodes.length).toBe(2);

    const branch1 = deserializedTableNode.nextNodes[0] as ModifyColumnsNode;
    const branch2 = deserializedTableNode.nextNodes[1] as ModifyColumnsNode;

    expect(branch1.prevNode?.nodeId).toBe(deserializedTableNode.nodeId);
    expect(branch2.prevNode?.nodeId).toBe(deserializedTableNode.nodeId);
  });

  test('deserializes graph without nodeLayouts field (auto-layout)', () => {
    // Create a real node and serialize it
    const sliceNode = new SlicesSourceNode({});
    const initialState: ExplorePageState = {
      rootNodes: [sliceNode],
      nodeLayouts: new Map(),
    };
    const serialized = serializeState(initialState);

    // Parse the JSON and remove the nodeLayouts field
    const parsed = JSON.parse(serialized);
    delete parsed.nodeLayouts;
    const jsonWithoutLayouts = JSON.stringify(parsed);

    const deserializedState = deserializeState(
      jsonWithoutLayouts,
      trace,
      sqlModules,
    );

    expect(deserializedState.rootNodes.length).toBe(1);
    expect(deserializedState.nodeLayouts.size).toBe(0);
    const deserializedNode = deserializedState.rootNodes[0] as SlicesSourceNode;
    expect(deserializedNode).toBeInstanceOf(SlicesSourceNode);
  });
});

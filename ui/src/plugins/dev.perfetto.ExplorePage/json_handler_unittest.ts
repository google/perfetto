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
} from '../../plugins/dev.perfetto.SqlModules/sql_modules';
import {AddColumnsNode} from './query_builder/nodes/add_columns_node';
import {LimitAndOffsetNode} from './query_builder/nodes/limit_and_offset_node';
import {SortNode} from './query_builder/nodes/sort_node';
import {MergeNode} from './query_builder/nodes/merge_node';
import {UnionNode} from './query_builder/nodes/union_node';
import {PerfettoSqlType} from '../../trace_processor/perfetto_sql_type';

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
      filters: [
        {
          column: 'dur',
          op: '>',
          value: '1000',
        },
      ],
      comment: 'Join on matching names with duration filter',
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
    expect(deserializedMergeNode.state.filters).toBeDefined();
    expect(deserializedMergeNode.state.filters?.length).toBe(1);
    expect(deserializedMergeNode.state.filters?.[0].column).toBe('dur');
    expect(deserializedMergeNode.state.filters?.[0].op).toBe('>');
    expect(deserializedMergeNode.state.comment).toBe(
      'Join on matching names with duration filter',
    );
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
    unionNode.filters = [
      {
        column: 'name',
        op: '!=',
        value: 'idle',
      },
    ];
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
    expect(deserializedUnionNode.filters).toBeDefined();
    expect(deserializedUnionNode.filters?.length).toBe(1);
    expect(deserializedUnionNode.filters?.[0].column).toBe('name');
    expect(deserializedUnionNode.filters?.[0].op).toBe('!=');
    expect(deserializedUnionNode.comment).toBe(
      'Union of slice sources excluding idle',
    );
    // Verify unchecked column is preserved
    expect(deserializedUnionNode.state.selectedColumns[1].checked).toBe(false);
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
      newColumns: [{expression: 'dur / 1000', name: 'dur_ms'}],
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
    expect(deserializedNode.state.newColumns.length).toBe(1);
    expect(deserializedNode.state.newColumns[0].expression).toBe('dur / 1000');
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
      filters: [
        {
          column: 'name',
          op: '!=',
          value: 'idle',
        },
      ],
    });

    const sliceTable = sqlModules.getTable('slice')!;
    const modifyNode = new ModifyColumnsNode({
      prevNode: tableNode,
      newColumns: [{expression: 'dur / 1000', name: 'dur_ms'}],
      selectedColumns: [
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: sliceTable.columns[0],
        },
      ],
    });
    tableNode.nextNodes.push(modifyNode);

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
    expect(node1.state.filters?.length).toBe(1);

    const node2 = node1.nextNodes[0] as ModifyColumnsNode;
    expect(node2.prevNode?.nodeId).toBe(node1.nodeId);
    expect(node2.nextNodes.length).toBe(1);

    const node3 = node2.nextNodes[0] as AggregationNode;
    expect(node3.prevNode?.nodeId).toBe(node2.nodeId);
    expect(node3.nextNodes.length).toBe(1);

    const node4 = node3.nextNodes[0] as SortNode;
    expect(node4.prevNode?.nodeId).toBe(node3.nodeId);
    expect(node4.nextNodes.length).toBe(1);

    const node5 = node4.nextNodes[0] as LimitAndOffsetNode;
    expect(node5.prevNode?.nodeId).toBe(node4.nodeId);
    expect(node5.state.limit).toBe(10);

    // Verify all layouts preserved
    expect(deserializedState.nodeLayouts.size).toBe(5);
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
      newColumns: [{expression: 'dur / 1000', name: 'dur_ms'}],
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
      newColumns: [{expression: 'ts / 1000000', name: 'ts_ms'}],
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
    expect(branch1.state.newColumns[0].name).toBe('dur_ms');
    expect(branch2.state.newColumns[0].name).toBe('ts_ms');
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

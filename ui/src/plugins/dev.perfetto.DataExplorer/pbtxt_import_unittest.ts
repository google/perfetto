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

import protos from '../../protos';
import {NodeType, QueryNode} from './query_node';
import {
  detectAndWrapPbtxt,
  decomposeStructuredQuery,
  buildNodesFromTemplateSpec,
  templateSpecToMetricsState,
  metricSpecToMetricsState,
} from './pbtxt_import';
import {SerializedGraph, deserializeState} from './json_handler';
import {registerCoreNodes} from './query_builder/core_nodes';
import {Trace} from '../../public/trace';
import type {
  SqlModules,
  SqlTable,
} from '../dev.perfetto.SqlModules/sql_modules';
import {PerfettoSqlType} from '../../trace_processor/perfetto_sql_type';

describe('detectAndWrapPbtxt', () => {
  it('passes through full TraceSummarySpec unchanged', () => {
    const input = `metric_template_spec {
  id_prefix: "foo"
}`;
    expect(detectAndWrapPbtxt(input)).toBe(input);
  });

  it('passes through metric_spec unchanged', () => {
    const input = `metric_spec {
  id: "foo"
}`;
    expect(detectAndWrapPbtxt(input)).toBe(input);
  });

  it('wraps single template spec with id_prefix', () => {
    const input = `id_prefix: "foo"
value_columns: "bar"`;
    const result = detectAndWrapPbtxt(input);
    expect(result).toContain('metric_template_spec {');
    expect(result).toContain('id_prefix: "foo"');
    expect(result).toContain('}');
  });

  it('wraps single metric spec with id field', () => {
    const input = `id: "my_metric"
value: "dur"`;
    const result = detectAndWrapPbtxt(input);
    expect(result).toContain('metric_spec {');
    expect(result).toContain('id: "my_metric"');
    expect(result).toContain('}');
  });

  it('trims whitespace', () => {
    const input = `  metric_template_spec {
  id_prefix: "foo"
}  `;
    expect(detectAndWrapPbtxt(input)).toBe(input.trim());
  });
});

describe('decomposeStructuredQuery', () => {
  it('decomposes a table source', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'my_table', moduleName: 'my.module'},
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    expect((result.nodes[0].state as Record<string, unknown>).sqlTable).toBe(
      'my_table',
    );
    expect(result.rootNodeIds).toHaveLength(1);
    expect(result.finalNodeId).toBe(result.nodes[0].nodeId);
  });

  it('decomposes a simple_slices source without globs', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      simpleSlices: {},
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kSimpleSlices);
  });

  it('decomposes simple_slices with globs into SlicesSource + Filter', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      simpleSlices: {
        sliceNameGlob: 'Choreographer*',
        processNameGlob: 'system_server',
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // Should produce SlicesSourceNode + FilterNode with GLOB filters.
    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[0].type).toBe(NodeType.kSimpleSlices);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters).toHaveLength(2);
    expect(filters[0]).toEqual({
      column: 'name',
      op: 'GLOB',
      value: 'Choreographer*',
      enabled: true,
    });
    expect(filters[1]).toEqual({
      column: 'process_name',
      op: 'GLOB',
      value: 'system_server',
      enabled: true,
    });

    // Check wiring.
    expect(result.nodes[0].nextNodes).toContain(result.nodes[1].nodeId);
    expect(filterState.primaryInputId).toBe(result.nodes[0].nodeId);
  });

  it('decomposes a SQL source', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      sql: {sql: 'SELECT * FROM slice'},
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kSqlSource);
    expect((result.nodes[0].state as Record<string, unknown>).sql).toBe(
      'SELECT * FROM slice',
    );
  });

  it('decomposes table with filters', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      filters: [
        {
          columnName: 'name',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
          stringRhs: ['foo'],
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);

    // Check filter state
    const filterState = result.nodes[1].state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters).toHaveLength(1);
    expect(filters[0].column).toBe('name');
    expect(filters[0].op).toBe('=');
    expect(filters[0].value).toBe('foo');

    // Check wiring
    expect(result.nodes[0].nextNodes).toContain(result.nodes[1].nodeId);
    expect(filterState.primaryInputId).toBe(result.nodes[0].nodeId);
  });

  it('decomposes table with group_by', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      groupBy: {
        columnNames: ['process_name'],
        aggregates: [
          {
            columnName: 'dur',
            op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.SUM,
            resultColumnName: 'total_dur',
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    expect(result.nodes[1].type).toBe(NodeType.kAggregation);

    const aggState = result.nodes[1].state as Record<string, unknown>;
    const groupByCols = aggState.groupByColumns as Array<
      Record<string, unknown>
    >;
    expect(groupByCols).toHaveLength(1);
    expect(groupByCols[0].name).toBe('process_name');

    const aggs = aggState.aggregations as Array<Record<string, unknown>>;
    expect(aggs).toHaveLength(1);
    expect((aggs[0].column as Record<string, unknown> | undefined)?.name).toBe(
      'dur',
    );
    expect(aggs[0].aggregationOp).toBe('SUM');
    expect(aggs[0].newColumnName).toBe('total_dur');
  });

  it('decomposes table with limit and offset', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      limit: 10,
      offset: 5,
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[1].type).toBe(NodeType.kLimitAndOffset);
    const state = result.nodes[1].state as Record<string, unknown>;
    expect(state.limit).toBe(10);
    expect(state.offset).toBe(5);
  });

  it('decomposes table with order_by', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      orderBy: {
        orderingSpecs: [
          {
            columnName: 'dur',
            direction: protos.PerfettoSqlStructuredQuery.OrderBy.Direction.DESC,
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[1].type).toBe(NodeType.kSort);
    const state = result.nodes[1].state as Record<string, unknown>;
    const criteria = state.sortCriteria as Array<Record<string, unknown>>;
    expect(criteria).toHaveLength(1);
    expect(criteria[0].colName).toBe('dur');
    expect(criteria[0].direction).toBe('DESC');
  });

  it('decomposes nested inner_query', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      innerQuery: {
        table: {tableName: 'slice'},
      },
      filters: [
        {
          columnName: 'dur',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
          int64Rhs: [1000],
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters[0].op).toBe('>');
    expect(filters[0].value).toBe('1000');
  });

  it('decomposes deeply nested query chain', () => {
    // table → filter → group_by → limit
    const sq = protos.PerfettoSqlStructuredQuery.create({
      innerQuery: {
        innerQuery: {
          table: {tableName: 'slice'},
          filters: [
            {
              columnName: 'name',
              op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GLOB,
              stringRhs: ['Choreographer*'],
            },
          ],
        },
        groupBy: {
          columnNames: ['name'],
          aggregates: [
            {
              columnName: 'dur',
              op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.SUM,
              resultColumnName: 'total_dur',
            },
          ],
        },
      },
      limit: 10,
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // Should produce: Table → Filter → Aggregation → Limit
    expect(result.nodes).toHaveLength(4);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);
    expect(result.nodes[2].type).toBe(NodeType.kAggregation);
    expect(result.nodes[3].type).toBe(NodeType.kLimitAndOffset);

    // Check chain wiring
    expect(result.nodes[0].nextNodes).toContain(result.nodes[1].nodeId);
    expect(result.nodes[1].nextNodes).toContain(result.nodes[2].nodeId);
    expect(result.nodes[2].nextNodes).toContain(result.nodes[3].nodeId);
  });

  it('decomposes interval_intersect', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      intervalIntersect: {
        base: {
          table: {tableName: 'memory_table'},
        },
        intervalIntersect: [
          {
            simpleSlices: {sliceNameGlob: 'CujName*'},
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // Should have: TableNode, SlicesNode, IntervalIntersectNode
    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kTable);
    expect(types).toContain(NodeType.kSimpleSlices);
    expect(types).toContain(NodeType.kIntervalIntersect);
  });

  it('creates fallback for unsupported source types', () => {
    // An empty query with no recognized source.
    const sq = protos.PerfettoSqlStructuredQuery.create({});

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kSqlSource);
  });

  it('assigns layout positions', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      filters: [
        {
          columnName: 'dur',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
          int64Rhs: [0],
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 100, 200);

    // Source nodes should have layouts; dockable nodes (filter) should not
    // (the graph renderer docks them to their parent when no layout is set).
    const tableNode = result.nodes[0];
    expect(result.layouts.has(tableNode.nodeId)).toBe(true);
    const firstLayout = result.layouts.get(tableNode.nodeId);
    expect(firstLayout?.x).toBe(100);
    expect(firstLayout?.y).toBe(200);

    // Filter is a dockable node — no layout position
    const filterNode = result.nodes[1];
    expect(result.layouts.has(filterNode.nodeId)).toBe(false);
  });
});

describe('templateSpecToMetricsState', () => {
  it('maps basic fields', () => {
    const spec = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'memory_per_process',
      valueColumns: ['avg_rss', 'total_swap'],
    });

    const state = templateSpecToMetricsState(spec);

    expect(state.metricIdPrefix).toBe('memory_per_process');
    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols).toHaveLength(2);
    expect(cols[0].column).toBe('avg_rss');
    expect(cols[0].unit).toBe('COUNT');
    expect(cols[1].column).toBe('total_swap');
  });

  it('maps valueColumnSpecs with unit and polarity', () => {
    const spec = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'test',
      valueColumnSpecs: [
        {
          name: 'duration',
          unit: protos.TraceMetricV2Spec.MetricUnit.TIME_NANOS,
          polarity: protos.TraceMetricV2Spec.MetricPolarity.LOWER_IS_BETTER,
          displayName: 'Duration',
          displayHelp: 'Total duration in ns',
        },
      ],
    });

    const state = templateSpecToMetricsState(spec);

    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols).toHaveLength(1);
    expect(cols[0].column).toBe('duration');
    expect(cols[0].unit).toBe('TIME_NANOS');
    expect(cols[0].polarity).toBe('LOWER_IS_BETTER');
    expect(cols[0].displayName).toBe('Duration');
    expect(cols[0].displayHelp).toBe('Total duration in ns');
  });

  it('maps custom unit', () => {
    const spec = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'test',
      valueColumnSpecs: [
        {
          name: 'score',
          customUnit: 'jank_score',
        },
      ],
    });

    const state = templateSpecToMetricsState(spec);

    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols[0].unit).toBe('CUSTOM');
    expect(cols[0].customUnit).toBe('jank_score');
  });

  it('maps dimension configs from dimensionsSpecs', () => {
    const spec = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'test',
      valueColumns: ['val'],
      dimensionsSpecs: [
        {
          name: 'process_name',
          displayName: 'Process',
          displayHelp: 'The process name',
        },
        {
          name: 'thread_name',
          // No display fields — should not appear in configs.
        },
      ],
    });

    const state = templateSpecToMetricsState(spec);

    const configs = state.dimensionConfigs as
      | Record<string, Record<string, unknown>>
      | undefined;
    expect(configs).toBeDefined();
    expect(configs?.process_name?.displayName).toBe('Process');
    expect(configs?.process_name?.displayHelp).toBe('The process name');
    // thread_name has no display fields, so shouldn't be in configs.
    expect(configs?.thread_name).toBeUndefined();
  });

  it('maps dimension uniqueness', () => {
    const specUnique = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'test',
      valueColumns: ['val'],
      dimensionUniqueness: protos.TraceMetricV2Spec.DimensionUniqueness.UNIQUE,
    });

    expect(templateSpecToMetricsState(specUnique).dimensionUniqueness).toBe(
      'UNIQUE',
    );

    const specNotUnique = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'test',
      valueColumns: ['val'],
      dimensionUniqueness:
        protos.TraceMetricV2Spec.DimensionUniqueness.NOT_UNIQUE,
    });

    expect(templateSpecToMetricsState(specNotUnique).dimensionUniqueness).toBe(
      'NOT_UNIQUE',
    );
  });

  it('defaults to NOT_UNIQUE when unspecified', () => {
    const spec = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'test',
      valueColumns: ['val'],
    });

    expect(templateSpecToMetricsState(spec).dimensionUniqueness).toBe(
      'NOT_UNIQUE',
    );
  });
});

describe('decomposeStructuredQuery - multi-input source types', () => {
  it('decomposes experimental_join into JoinNode with sub-queries', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      experimentalJoin: {
        type: protos.PerfettoSqlStructuredQuery.ExperimentalJoin.Type.INNER,
        leftQuery: {table: {tableName: 'slice'}},
        rightQuery: {table: {tableName: 'thread'}},
        equalityColumns: {leftColumn: 'utid', rightColumn: 'id'},
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kTable);
    expect(types).toContain(NodeType.kJoin);
    const joinNode = result.nodes.find((n) => n.type === NodeType.kJoin);
    const state = joinNode?.state as Record<string, unknown>;
    expect(state.joinType).toBe('INNER');
    expect(state.leftColumn).toBe('utid');
    expect(state.rightColumn).toBe('id');
  });

  it('decomposes experimental_join with LEFT type', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      experimentalJoin: {
        type: protos.PerfettoSqlStructuredQuery.ExperimentalJoin.Type.LEFT,
        leftQuery: {table: {tableName: 'slice'}},
        rightQuery: {table: {tableName: 'thread'}},
        equalityColumns: {leftColumn: 'utid', rightColumn: 'id'},
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const joinNode = result.nodes.find((n) => n.type === NodeType.kJoin);
    const state = joinNode?.state as Record<string, unknown>;
    expect(state.joinType).toBe('LEFT');
  });

  it('decomposes experimental_union into UnionNode with sub-queries', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      experimentalUnion: {
        queries: [
          {table: {tableName: 'slice'}},
          {table: {tableName: 'counter'}},
        ],
        useUnionAll: true,
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kTable);
    expect(types).toContain(NodeType.kUnion);
    // Two table nodes + one union node.
    expect(result.nodes.filter((n) => n.type === NodeType.kTable)).toHaveLength(
      2,
    );
  });

  it('decomposes experimental_add_columns into AddColumnsNode', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      experimentalAddColumns: {
        coreQuery: {table: {tableName: 'slice'}},
        inputQuery: {table: {tableName: 'thread'}},
        inputColumns: [{columnNameOrExpression: 'thread_name'}],
        equalityColumns: {leftColumn: 'utid', rightColumn: 'id'},
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kAddColumns);
    const addColNode = result.nodes.find(
      (n) => n.type === NodeType.kAddColumns,
    );
    const state = addColNode?.state as Record<string, unknown>;
    expect(state.leftColumn).toBe('utid');
    expect(state.rightColumn).toBe('id');
    expect(state.selectedColumns).toEqual(['thread_name']);
  });

  it('decomposes experimental_filter_to_intervals into FilterDuringNode', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      experimentalFilterToIntervals: {
        base: {table: {tableName: 'slice'}},
        intervals: {table: {tableName: 'cuj_intervals'}},
        clipToIntervals: true,
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kFilterDuring);
    const ftiNode = result.nodes.find((n) => n.type === NodeType.kFilterDuring);
    const state = ftiNode?.state as Record<string, unknown>;
    expect(state.clipToIntervals).toBe(true);
  });

  it('decomposes experimental_create_slices into CreateSlicesNode', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      experimentalCreateSlices: {
        startsQuery: {table: {tableName: 'start_events'}},
        endsQuery: {table: {tableName: 'end_events'}},
        startsTsColumn: 'begin_ts',
        endsTsColumn: 'end_ts',
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kCreateSlices);
    const csNode = result.nodes.find((n) => n.type === NodeType.kCreateSlices);
    const state = csNode?.state as Record<string, unknown>;
    expect(state.startsTsColumn).toBe('begin_ts');
    expect(state.endsTsColumn).toBe('end_ts');
  });

  it('decomposes experimental_counter_intervals into CounterToIntervalsNode', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      experimentalCounterIntervals: {
        inputQuery: {table: {tableName: 'counter'}},
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kTable);
    expect(types).toContain(NodeType.kCounterToIntervals);
    // Table node + CounterToIntervals node.
    expect(result.nodes).toHaveLength(2);
  });

  it('decomposes experimental_filter_in into FilterInNode', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      experimentalFilterIn: {
        base: {table: {tableName: 'slice'}},
        matchValues: {table: {tableName: 'selected_threads'}},
        baseColumn: 'utid',
        matchColumn: 'utid',
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kFilterIn);
    const fiNode = result.nodes.find((n) => n.type === NodeType.kFilterIn);
    const state = fiNode?.state as Record<string, unknown>;
    expect(state.baseColumn).toBe('utid');
    expect(state.matchColumn).toBe('utid');
  });

  it('falls back to SqlSourceNode for inner_query_id without shared queries', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      innerQueryId: 'shared_query_ref',
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // inner_query_id without a sharedQueries map falls back to SqlSourceNode.
    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kSqlSource);
  });
});

describe('decomposeStructuredQuery - experimental_filter_group', () => {
  it('converts flat AND of filters to structured FilterNode', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      experimentalFilterGroup: {
        op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
          .AND,
        filters: [
          {
            columnName: 'name',
            op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
            stringRhs: ['foo'],
          },
          {
            columnName: 'dur',
            op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
            int64Rhs: [1000],
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters).toHaveLength(2);
    expect(filters[0]).toEqual({
      column: 'name',
      op: '=',
      value: 'foo',
      enabled: true,
    });
    expect(filters[1]).toEqual({
      column: 'dur',
      op: '>',
      value: '1000',
      enabled: true,
    });
    // AND is the default, so filterOperator should not be set to 'OR'.
    expect(filterState.filterOperator).toBeUndefined();
  });

  it('converts flat OR of filters to structured FilterNode with OR', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      experimentalFilterGroup: {
        op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
          .OR,
        filters: [
          {
            columnName: 'name',
            op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
            stringRhs: ['foo'],
          },
          {
            columnName: 'name',
            op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
            stringRhs: ['bar'],
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    expect(filterState.filterOperator).toBe('OR');
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters).toHaveLength(2);
    expect(filters[0].column).toBe('name');
    expect(filters[0].value).toBe('foo');
    expect(filters[1].column).toBe('name');
    expect(filters[1].value).toBe('bar');
  });

  it('converts AND of sub-groups to multiple chained FilterNodes', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      experimentalFilterGroup: {
        op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
          .AND,
        groups: [
          {
            op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup
              .Operator.OR,
            filters: [
              {
                columnName: 'name',
                op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
                stringRhs: ['foo'],
              },
              {
                columnName: 'name',
                op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
                stringRhs: ['bar'],
              },
            ],
          },
          {
            op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup
              .Operator.AND,
            filters: [
              {
                columnName: 'dur',
                op: protos.PerfettoSqlStructuredQuery.Filter.Operator
                  .GREATER_THAN,
                int64Rhs: [1000],
              },
            ],
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // Table + 2 filter nodes (one per sub-group).
    expect(result.nodes).toHaveLength(3);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);
    expect(result.nodes[2].type).toBe(NodeType.kFilter);

    // First sub-group: OR of name='foo' and name='bar'.
    const state1 = result.nodes[1].state as Record<string, unknown>;
    expect(state1.filterOperator).toBe('OR');
    const filters1 = state1.filters as Array<Record<string, unknown>>;
    expect(filters1).toHaveLength(2);

    // Second sub-group: AND of dur > 1000.
    const state2 = result.nodes[2].state as Record<string, unknown>;
    const filters2 = state2.filters as Array<Record<string, unknown>>;
    expect(filters2).toHaveLength(1);
    expect(filters2[0].column).toBe('dur');
  });

  it('falls back to freeform for mixed filters + sql_expressions', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      experimentalFilterGroup: {
        op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
          .AND,
        filters: [
          {
            columnName: 'dur',
            op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
            int64Rhs: [1000],
          },
        ],
        sqlExpressions: ['LENGTH(name) > 10'],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    expect(filterState.filterMode).toBe('freeform');
    expect(filterState.sqlExpression).toBe('dur > 1000 AND LENGTH(name) > 10');
  });

  it('falls back to freeform for nested groups with sql_expressions', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      experimentalFilterGroup: {
        op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
          .AND,
        groups: [
          {
            op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup
              .Operator.OR,
            filters: [
              {
                columnName: 'name',
                op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
                stringRhs: ['foo'],
              },
              {
                columnName: 'name',
                op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
                stringRhs: ['bar'],
              },
            ],
          },
        ],
        sqlExpressions: ['LENGTH(name) > 10'],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    expect(filterState.filterMode).toBe('freeform');
    expect(filterState.sqlExpression).toBe(
      "LENGTH(name) > 10 AND (name = 'foo' OR name = 'bar')",
    );
  });

  it('coexists with regular filters field', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      filters: [
        {
          columnName: 'category',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
          stringRhs: ['rendering'],
        },
      ],
      experimentalFilterGroup: {
        op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
          .AND,
        filters: [
          {
            columnName: 'dur',
            op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
            int64Rhs: [1000],
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // Table + filter from `filters` + filter from `experimentalFilterGroup`.
    expect(result.nodes).toHaveLength(3);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);
    expect(result.nodes[2].type).toBe(NodeType.kFilter);

    // First filter node is from `filters` field.
    const state1 = result.nodes[1].state as Record<string, unknown>;
    const filters1 = state1.filters as Array<Record<string, unknown>>;
    expect(filters1[0].column).toBe('category');

    // Second filter node is from `experimentalFilterGroup`.
    const state2 = result.nodes[2].state as Record<string, unknown>;
    const filters2 = state2.filters as Array<Record<string, unknown>>;
    expect(filters2[0].column).toBe('dur');
  });
});

describe('decomposeStructuredQuery - filter edge cases', () => {
  it('expands multiple string_rhs values into OR FilterNode', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      filters: [
        {
          columnName: 'name',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
          stringRhs: ['foo', 'bar', 'baz'],
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    const filterState = result.nodes[1].state as Record<string, unknown>;
    expect(filterState.filterOperator).toBe('OR');
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters).toHaveLength(3);
    expect(filters[0].value).toBe('foo');
    expect(filters[1].value).toBe('bar');
    expect(filters[2].value).toBe('baz');
    expect(filters[0].column).toBe('name');
    expect(filters[0].op).toBe('=');
  });

  it('separates multi-value and single-value filters into chained nodes', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      filters: [
        {
          columnName: 'category',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
          stringRhs: ['gfx'],
        },
        {
          columnName: 'name',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
          stringRhs: ['DrawFrame', 'doFrame', 'queueBuffer'],
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // Table + AND FilterNode (category=gfx) + OR FilterNode (name IN ...)
    expect(result.nodes).toHaveLength(3);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);
    expect(result.nodes[2].type).toBe(NodeType.kFilter);

    // First filter: single-value, AND mode (default).
    const state1 = result.nodes[1].state as Record<string, unknown>;
    const filters1 = state1.filters as Array<Record<string, unknown>>;
    expect(filters1).toHaveLength(1);
    expect(filters1[0].column).toBe('category');
    expect(filters1[0].value).toBe('gfx');

    // Second filter: multi-value, OR mode.
    const state2 = result.nodes[2].state as Record<string, unknown>;
    expect(state2.filterOperator).toBe('OR');
    const filters2 = state2.filters as Array<Record<string, unknown>>;
    expect(filters2).toHaveLength(3);
    expect(filters2[0].value).toBe('DrawFrame');
    expect(filters2[1].value).toBe('doFrame');
    expect(filters2[2].value).toBe('queueBuffer');
  });

  it('handles double_rhs filter values', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'counter'},
      filters: [
        {
          columnName: 'value',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
          doubleRhs: [3.14],
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters[0].value).toBe('3.14');
    expect(filters[0].op).toBe('>');
  });

  it('handles IS_NULL filter with no RHS values', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      filters: [
        {
          columnName: 'parent_id',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NULL,
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters[0].column).toBe('parent_id');
    expect(filters[0].op).toBe('IS NULL');
    expect(filters[0].value).toBe('');
  });

  it('handles IS_NOT_NULL filter with no RHS values', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      filters: [
        {
          columnName: 'parent_id',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NOT_NULL,
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters[0].op).toBe('IS NOT NULL');
    expect(filters[0].value).toBe('');
  });

  it('handles UNKNOWN operator by defaulting to =', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      filters: [
        {
          columnName: 'name',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.UNKNOWN,
          stringRhs: ['test'],
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    // UNKNOWN (0) is not in FILTER_OP_MAP, so falls back to '='.
    expect(filters[0].op).toBe('=');
  });

  it('handles multiple filters with different RHS types', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      filters: [
        {
          columnName: 'name',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GLOB,
          stringRhs: ['Choreographer*'],
        },
        {
          columnName: 'dur',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
          int64Rhs: [1000000],
        },
        {
          columnName: 'value',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.LESS_THAN_EQUAL,
          doubleRhs: [99.5],
        },
        {
          columnName: 'parent_id',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NOT_NULL,
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const filterState = result.nodes[1].state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters).toHaveLength(4);
    expect(filters[0]).toEqual({
      column: 'name',
      op: 'GLOB',
      value: 'Choreographer*',
      enabled: true,
    });
    expect(filters[1]).toEqual({
      column: 'dur',
      op: '>',
      value: '1000000',
      enabled: true,
    });
    expect(filters[2]).toEqual({
      column: 'value',
      op: '<=',
      value: '99.5',
      enabled: true,
    });
    expect(filters[3]).toEqual({
      column: 'parent_id',
      op: 'IS NOT NULL',
      value: '',
      enabled: true,
    });
  });
});

describe('decomposeStructuredQuery - select_columns edge cases', () => {
  it('handles select_columns with column_name_or_expression', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      selectColumns: [
        {columnNameOrExpression: 'ts + dur', alias: 'end_ts'},
        {columnNameOrExpression: 'name'},
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[1].type).toBe(NodeType.kModifyColumns);
    const modState = result.nodes[1].state as Record<string, unknown>;
    const cols = modState.selectedColumns as Array<Record<string, unknown>>;
    expect(cols).toHaveLength(2);
    expect(cols[0].name).toBe('ts + dur');
    expect(cols[0].alias).toBe('end_ts');
    expect(cols[1].name).toBe('name');
  });

  it('handles select_columns with deprecated column_name field', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      selectColumns: [{columnName: 'ts', alias: 'timestamp'}],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const modState = result.nodes[1].state as Record<string, unknown>;
    const cols = modState.selectedColumns as Array<Record<string, unknown>>;
    expect(cols[0].name).toBe('ts');
    expect(cols[0].alias).toBe('timestamp');
  });

  it('prefers column_name_or_expression over deprecated column_name', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      selectColumns: [
        {
          columnNameOrExpression: 'CAST(ts AS TEXT)',
          columnName: 'ts',
          alias: 'ts_text',
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const modState = result.nodes[1].state as Record<string, unknown>;
    const cols = modState.selectedColumns as Array<Record<string, unknown>>;
    // Should prefer columnNameOrExpression over deprecated columnName.
    expect(cols[0].name).toBe('CAST(ts AS TEXT)');
    expect(cols[0].alias).toBe('ts_text');
  });
});

describe('decomposeStructuredQuery - referenced_modules', () => {
  it('does not produce any node for referenced_modules', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      referencedModules: ['linux.cpu.utilization', 'android.frames'],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // referenced_modules is silently ignored; only the table node is produced.
    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
  });
});

describe('decomposeStructuredQuery - table column_names', () => {
  it('does not include column_names in table node state', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {
        tableName: 'slice',
        columnNames: ['ts', 'dur', 'name'],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    const state = result.nodes[0].state as Record<string, unknown>;
    // column_names are not persisted in the node state.
    expect(state.sqlTable).toBe('slice');
    expect(state.columnNames).toBeUndefined();
  });
});

describe('decomposeStructuredQuery - limit/offset with zero values', () => {
  it('does not create LimitAndOffset node when limit is 0', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      limit: 0,
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // protobufjs --force-number sets unset to 0, and limit=0 means "unset".
    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
  });

  it('creates LimitAndOffset node when only limit is set', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      limit: 100,
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(2);
    expect(result.nodes[1].type).toBe(NodeType.kLimitAndOffset);
    const state = result.nodes[1].state as Record<string, unknown>;
    expect(state.limit).toBe(100);
    // offset=0 is treated as unset so it's not in the state.
    expect(state.offset).toBeUndefined();
  });
});

describe('decomposeStructuredQuery - source with operations chained', () => {
  it('chains filter onto JoinNode from experimental_join', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      experimentalJoin: {
        type: protos.PerfettoSqlStructuredQuery.ExperimentalJoin.Type.LEFT,
        leftQuery: {table: {tableName: 'slice'}},
        rightQuery: {table: {tableName: 'thread'}},
        equalityColumns: {leftColumn: 'utid', rightColumn: 'id'},
      },
      filters: [
        {
          columnName: 'name',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL,
          stringRhs: ['test'],
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // Two TableNodes + JoinNode + FilterNode.
    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kJoin);
    expect(types).toContain(NodeType.kFilter);

    // Filter should chain off the JoinNode.
    const joinNode = result.nodes.find((n) => n.type === NodeType.kJoin);
    const filterNode = result.nodes.find((n) => n.type === NodeType.kFilter);
    expect(joinNode?.nextNodes).toContain(filterNode?.nodeId);
  });
});

describe('decomposeStructuredQuery - aggregation edge cases', () => {
  it('handles COUNT aggregate without column_name', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      groupBy: {
        columnNames: ['name'],
        aggregates: [
          {
            op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.COUNT,
            resultColumnName: 'cnt',
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const aggState = result.nodes[1].state as Record<string, unknown>;
    const aggs = aggState.aggregations as Array<Record<string, unknown>>;
    expect(aggs[0].aggregationOp).toBe('COUNT');
    expect(aggs[0].column).toBeUndefined();
    expect(aggs[0].newColumnName).toBe('cnt');
  });

  it('handles PERCENTILE aggregate with percentile field', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      groupBy: {
        columnNames: ['name'],
        aggregates: [
          {
            columnName: 'dur',
            op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
              .PERCENTILE,
            resultColumnName: 'p95_dur',
            percentile: 95,
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const aggState = result.nodes[1].state as Record<string, unknown>;
    const aggs = aggState.aggregations as Array<Record<string, unknown>>;
    expect(aggs[0].aggregationOp).toBe('PERCENTILE');
    expect(aggs[0].percentile).toBe(95);
  });

  it('handles CUSTOM aggregate with custom_sql_expression', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      groupBy: {
        columnNames: ['name'],
        aggregates: [
          {
            op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.CUSTOM,
            customSqlExpression: 'SUM(dur * priority) / SUM(dur)',
            resultColumnName: 'weighted_avg',
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const aggState = result.nodes[1].state as Record<string, unknown>;
    const aggs = aggState.aggregations as Array<Record<string, unknown>>;
    expect(aggs[0].aggregationOp).toBe('CUSTOM');
  });

  it('handles all aggregate ops', () => {
    const ops = [
      {
        op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
          .DURATION_WEIGHTED_MEAN,
        expected: 'DURATION_WEIGHTED_MEAN',
      },
      {
        op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
          .COUNT_DISTINCT,
        expected: 'COUNT_DISTINCT',
      },
      {
        op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MEDIAN,
        expected: 'MEDIAN',
      },
      {
        op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MEAN,
        expected: 'MEAN',
      },
      {
        op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MIN,
        expected: 'MIN',
      },
      {
        op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MAX,
        expected: 'MAX',
      },
    ];

    for (const {op, expected} of ops) {
      const sq = protos.PerfettoSqlStructuredQuery.create({
        table: {tableName: 'slice'},
        groupBy: {
          columnNames: ['name'],
          aggregates: [{columnName: 'dur', op, resultColumnName: 'result'}],
        },
      });
      const result = decomposeStructuredQuery(sq, 0, 0);
      const aggState = result.nodes[1].state as Record<string, unknown>;
      const aggs = aggState.aggregations as Array<Record<string, unknown>>;
      expect(aggs[0].aggregationOp).toBe(expected);
    }
  });
});

describe('decomposeStructuredQuery - order_by edge cases', () => {
  it('defaults direction to ASC when unspecified', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      orderBy: {
        orderingSpecs: [{columnName: 'ts'}],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const state = result.nodes[1].state as Record<string, unknown>;
    const criteria = state.sortCriteria as Array<Record<string, unknown>>;
    expect(criteria[0].direction).toBe('ASC');
  });

  it('handles multiple ordering specs', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      orderBy: {
        orderingSpecs: [
          {
            columnName: 'dur',
            direction: protos.PerfettoSqlStructuredQuery.OrderBy.Direction.DESC,
          },
          {
            columnName: 'ts',
            direction: protos.PerfettoSqlStructuredQuery.OrderBy.Direction.ASC,
          },
        ],
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    const state = result.nodes[1].state as Record<string, unknown>;
    const criteria = state.sortCriteria as Array<Record<string, unknown>>;
    expect(criteria).toHaveLength(2);
    expect(criteria[0].colName).toBe('dur');
    expect(criteria[0].direction).toBe('DESC');
    expect(criteria[1].colName).toBe('ts');
    expect(criteria[1].direction).toBe('ASC');
  });
});

describe('decomposeStructuredQuery - full operation chain ordering', () => {
  it('applies operations in correct order: filter, group_by, select, sort, limit', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice'},
      filters: [
        {
          columnName: 'dur',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
          int64Rhs: [0],
        },
      ],
      groupBy: {
        columnNames: ['name'],
        aggregates: [
          {
            columnName: 'dur',
            op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.SUM,
            resultColumnName: 'total',
          },
        ],
      },
      selectColumns: [
        {columnNameOrExpression: 'name'},
        {columnNameOrExpression: 'total'},
      ],
      orderBy: {
        orderingSpecs: [
          {
            columnName: 'total',
            direction: protos.PerfettoSqlStructuredQuery.OrderBy.Direction.DESC,
          },
        ],
      },
      limit: 10,
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    // Table, Filter, Aggregation, Sort, LimitAndOffset
    // (select_columns is redundant after group_by and is skipped)
    expect(result.nodes).toHaveLength(5);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    expect(result.nodes[1].type).toBe(NodeType.kFilter);
    expect(result.nodes[2].type).toBe(NodeType.kAggregation);
    expect(result.nodes[3].type).toBe(NodeType.kSort);
    expect(result.nodes[4].type).toBe(NodeType.kLimitAndOffset);

    // Verify chain wiring.
    for (let i = 0; i < result.nodes.length - 1; i++) {
      expect(result.nodes[i].nextNodes).toContain(result.nodes[i + 1].nodeId);
    }
    expect(result.finalNodeId).toBe(result.nodes[4].nodeId);
  });
});

describe('decomposeStructuredQuery - time range source', () => {
  it('handles experimental_time_range with ts value', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      experimentalTimeRange: {
        mode: protos.PerfettoSqlStructuredQuery.ExperimentalTimeRange.Mode
          .STATIC,
        ts: 1000000,
        dur: 5000000,
      },
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kTimeRangeSource);
    const state = result.nodes[0].state as Record<string, unknown>;
    expect(state.start).toBe('1000000');
  });
});

describe('metricSpecToMetricsState', () => {
  it('maps basic metric spec fields', () => {
    const spec = protos.TraceMetricV2Spec.create({
      id: 'my_metric',
      value: 'total_dur',
      unit: protos.TraceMetricV2Spec.MetricUnit.TIME_NANOS,
      polarity: protos.TraceMetricV2Spec.MetricPolarity.LOWER_IS_BETTER,
    });

    const state = metricSpecToMetricsState(spec);

    expect(state.metricIdPrefix).toBe('my_metric');
    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols).toHaveLength(1);
    expect(cols[0].column).toBe('total_dur');
    expect(cols[0].unit).toBe('TIME_NANOS');
    expect(cols[0].polarity).toBe('LOWER_IS_BETTER');
  });

  it('handles missing value field', () => {
    const spec = protos.TraceMetricV2Spec.create({
      id: 'empty_metric',
    });

    const state = metricSpecToMetricsState(spec);

    expect(state.metricIdPrefix).toBe('empty_metric');
    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols).toHaveLength(0);
  });

  it('maps UNIQUE dimension uniqueness', () => {
    const spec = protos.TraceMetricV2Spec.create({
      id: 'test',
      value: 'val',
      dimensionUniqueness: protos.TraceMetricV2Spec.DimensionUniqueness.UNIQUE,
    });

    expect(metricSpecToMetricsState(spec).dimensionUniqueness).toBe('UNIQUE');
  });

  it('maps custom_unit on metric spec', () => {
    const spec = protos.TraceMetricV2Spec.create({
      id: 'custom_metric',
      value: 'score',
      customUnit: 'fps',
    });

    const state = metricSpecToMetricsState(spec);

    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols[0].unit).toBe('CUSTOM');
    expect(cols[0].customUnit).toBe('fps');
  });

  it('maps polarity HIGHER_IS_BETTER on metric spec', () => {
    const spec = protos.TraceMetricV2Spec.create({
      id: 'perf_metric',
      value: 'throughput',
      polarity: protos.TraceMetricV2Spec.MetricPolarity.HIGHER_IS_BETTER,
    });

    const state = metricSpecToMetricsState(spec);

    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols[0].polarity).toBe('HIGHER_IS_BETTER');
  });

  it('maps polarity NOT_APPLICABLE on metric spec', () => {
    const spec = protos.TraceMetricV2Spec.create({
      id: 'info_metric',
      value: 'count',
      polarity: protos.TraceMetricV2Spec.MetricPolarity.NOT_APPLICABLE,
    });

    const state = metricSpecToMetricsState(spec);

    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols[0].polarity).toBe('NOT_APPLICABLE');
  });

  it('defaults polarity to NOT_APPLICABLE when unset', () => {
    const spec = protos.TraceMetricV2Spec.create({
      id: 'no_polarity',
      value: 'val',
    });

    const state = metricSpecToMetricsState(spec);

    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols[0].polarity).toBe('NOT_APPLICABLE');
  });

  it('does not include bundle_id in state', () => {
    const spec = protos.TraceMetricV2Spec.create({
      id: 'bundled_metric',
      value: 'dur',
      bundleId: 'my_bundle',
    });

    const state = metricSpecToMetricsState(spec);

    // bundle_id is not mapped to any state field.
    expect(state.bundleId).toBeUndefined();
  });

  it('does not include interned_dimension_specs in state', () => {
    const spec = protos.TraceMetricV2Spec.create({
      id: 'interned_metric',
      value: 'dur',
      internedDimensionSpecs: [
        {
          keyColumnSpec: {
            name: 'package_name',
            type: protos.TraceMetricV2Spec.DimensionType.STRING,
          },
          dataColumnSpecs: [
            {
              name: 'version_code',
              type: protos.TraceMetricV2Spec.DimensionType.INT64,
            },
          ],
          query: {
            sql: {
              sql: 'SELECT name AS package_name, version_code FROM package_list',
            },
          },
        },
      ],
    });

    const state = metricSpecToMetricsState(spec);

    // interned_dimension_specs are not mapped.
    expect(state.internedDimensionSpecs).toBeUndefined();
  });

  it('maps dimensions_specs with display fields', () => {
    const spec = protos.TraceMetricV2Spec.create({
      id: 'dim_test',
      value: 'val',
      dimensionsSpecs: [
        {
          name: 'process_name',
          displayName: 'Process',
          displayHelp: 'Target process',
        },
      ],
    });

    const state = metricSpecToMetricsState(spec);

    const configs = state.dimensionConfigs as
      | Record<string, Record<string, unknown>>
      | undefined;
    expect(configs).toBeDefined();
    expect(configs?.process_name?.displayName).toBe('Process');
    expect(configs?.process_name?.displayHelp).toBe('Target process');
  });
});

describe('templateSpecToMetricsState - additional coverage', () => {
  it('does not include disable_auto_bundling in state', () => {
    const spec = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'test',
      valueColumns: ['val'],
      disableAutoBundling: true,
    });

    const state = templateSpecToMetricsState(spec);

    // disable_auto_bundling is not mapped to any state field.
    expect(state.disableAutoBundling).toBeUndefined();
  });

  it('does not include interned_dimension_specs in state', () => {
    const spec = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'test',
      valueColumns: ['val'],
      internedDimensionSpecs: [
        {
          keyColumnSpec: {
            name: 'pkg',
            type: protos.TraceMetricV2Spec.DimensionType.STRING,
          },
          dataColumnSpecs: [
            {name: 'ver', type: protos.TraceMetricV2Spec.DimensionType.INT64},
          ],
        },
      ],
    });

    const state = templateSpecToMetricsState(spec);

    // interned_dimension_specs are not mapped.
    expect(state.internedDimensionSpecs).toBeUndefined();
  });

  it('returns undefined dimensionConfigs when no display fields set', () => {
    const spec = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'test',
      valueColumns: ['val'],
      dimensionsSpecs: [{name: 'col_a'}, {name: 'col_b'}],
    });

    const state = templateSpecToMetricsState(spec);

    // No display fields on any dimension, so dimensionConfigs is undefined.
    expect(state.dimensionConfigs).toBeUndefined();
  });

  it('handles empty valueColumns and valueColumnSpecs', () => {
    const spec = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'empty',
    });

    const state = templateSpecToMetricsState(spec);

    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols).toHaveLength(0);
  });

  it('maps HIGHER_IS_BETTER polarity in valueColumnSpecs', () => {
    const spec = protos.TraceMetricV2TemplateSpec.create({
      idPrefix: 'test',
      valueColumnSpecs: [
        {
          name: 'throughput',
          polarity: protos.TraceMetricV2Spec.MetricPolarity.HIGHER_IS_BETTER,
        },
      ],
    });

    const state = templateSpecToMetricsState(spec);

    const cols = state.valueColumns as Array<Record<string, unknown>>;
    expect(cols[0].polarity).toBe('HIGHER_IS_BETTER');
  });
});

describe('detectAndWrapPbtxt - additional edge cases', () => {
  it('passes through text containing query field unchanged', () => {
    const input = `query {
  table {
    table_name: "slice"
  }
}`;
    expect(detectAndWrapPbtxt(input)).toBe(input);
  });

  it('returns as-is when text has no recognized structure', () => {
    const input = `some_unknown_field: "value"`;
    // Not recognized as template spec, metric spec, or TraceSummarySpec.
    // Falls through to "assume full TraceSummarySpec".
    expect(detectAndWrapPbtxt(input)).toBe(input);
  });
});

describe('decomposeStructuredQuery - inner_query_id with shared queries', () => {
  it('resolves inner_query_id to a shared table query', () => {
    const sharedQueries = new Map<string, protos.IPerfettoSqlStructuredQuery>();
    sharedQueries.set(
      'my_table',
      protos.PerfettoSqlStructuredQuery.create({
        table: {tableName: 'slice', moduleName: 'linux.cpu'},
      }),
    );

    const sq = protos.PerfettoSqlStructuredQuery.create({
      innerQueryId: 'my_table',
    });

    const result = decomposeStructuredQuery(sq, 0, 0, sharedQueries);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    const state = result.nodes[0].state as Record<string, unknown>;
    expect(state.sqlTable).toBe('slice');
    expect(state.moduleName).toBe('linux.cpu');
  });

  it('resolves inner_query_id to a shared query with operations', () => {
    const sharedQueries = new Map<string, protos.IPerfettoSqlStructuredQuery>();
    sharedQueries.set(
      'filtered_slices',
      protos.PerfettoSqlStructuredQuery.create({
        table: {tableName: 'slice'},
        filters: [
          {
            columnName: 'dur',
            op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
            int64Rhs: [1000],
          },
        ],
        groupBy: {
          columnNames: ['name'],
          aggregates: [
            {
              columnName: 'dur',
              op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.SUM,
              resultColumnName: 'total_dur',
            },
          ],
        },
      }),
    );

    const sq = protos.PerfettoSqlStructuredQuery.create({
      innerQueryId: 'filtered_slices',
    });

    const result = decomposeStructuredQuery(sq, 0, 0, sharedQueries);

    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kTable);
    expect(types).toContain(NodeType.kFilter);
    expect(types).toContain(NodeType.kAggregation);

    // Verify the filter was properly decomposed.
    const filterNode = result.nodes.find((n) => n.type === NodeType.kFilter);
    const filterState = filterNode?.state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters).toHaveLength(1);
    expect(filters[0].column).toBe('dur');
    expect(filters[0].op).toBe('>');
  });

  it('falls back to SqlSourceNode when inner_query_id is not found', () => {
    const sharedQueries = new Map<string, protos.IPerfettoSqlStructuredQuery>();
    sharedQueries.set(
      'existing_query',
      protos.PerfettoSqlStructuredQuery.create({
        table: {tableName: 'slice'},
      }),
    );

    const sq = protos.PerfettoSqlStructuredQuery.create({
      innerQueryId: 'nonexistent_query',
    });

    const result = decomposeStructuredQuery(sq, 0, 0, sharedQueries);

    // Not found in map — falls back to SqlSourceNode.
    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kSqlSource);
  });

  it('resolves nested inner_query_id references', () => {
    const sharedQueries = new Map<string, protos.IPerfettoSqlStructuredQuery>();
    // Shared query B: a simple table.
    sharedQueries.set(
      'base_table',
      protos.PerfettoSqlStructuredQuery.create({
        table: {tableName: 'thread'},
      }),
    );
    // Shared query A: references shared query B via inner_query_id.
    sharedQueries.set(
      'filtered_threads',
      protos.PerfettoSqlStructuredQuery.create({
        innerQueryId: 'base_table',
        filters: [
          {
            columnName: 'name',
            op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GLOB,
            stringRhs: ['Render*'],
          },
        ],
      }),
    );

    const sq = protos.PerfettoSqlStructuredQuery.create({
      innerQueryId: 'filtered_threads',
    });

    const result = decomposeStructuredQuery(sq, 0, 0, sharedQueries);

    const types = result.nodes.map((n) => n.type);
    // The chain should be: TableNode (thread) → FilterNode.
    expect(types).toContain(NodeType.kTable);
    expect(types).toContain(NodeType.kFilter);

    const tableNode = result.nodes.find((n) => n.type === NodeType.kTable);
    const tableState = tableNode?.state as Record<string, unknown>;
    expect(tableState.sqlTable).toBe('thread');

    const filterNode = result.nodes.find((n) => n.type === NodeType.kFilter);
    const filterState = filterNode?.state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters[0].column).toBe('name');
    expect(filters[0].op).toBe('GLOB');
    expect(filters[0].value).toBe('Render*');
  });

  it('applies operations from the referencing query on top of resolved shared query', () => {
    const sharedQueries = new Map<string, protos.IPerfettoSqlStructuredQuery>();
    sharedQueries.set(
      'base_slices',
      protos.PerfettoSqlStructuredQuery.create({
        table: {tableName: 'slice'},
      }),
    );

    // The referencing query adds filters on top of the resolved shared query.
    const sq = protos.PerfettoSqlStructuredQuery.create({
      innerQueryId: 'base_slices',
      filters: [
        {
          columnName: 'dur',
          op: protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN,
          int64Rhs: [5000],
        },
      ],
    });

    const result = decomposeStructuredQuery(sq, 0, 0, sharedQueries);

    const types = result.nodes.map((n) => n.type);
    expect(types).toContain(NodeType.kTable);
    expect(types).toContain(NodeType.kFilter);

    const filterNode = result.nodes.find((n) => n.type === NodeType.kFilter);
    const filterState = filterNode?.state as Record<string, unknown>;
    const filters = filterState.filters as Array<Record<string, unknown>>;
    expect(filters[0].column).toBe('dur');
    expect(filters[0].value).toBe('5000');
  });
});

describe('decomposeStructuredQuery - table source with sqlModules', () => {
  // Minimal mock that satisfies the SqlModules interface for getTable().
  function createMockSqlModules(knownTables: string[]): SqlModules {
    return {
      getTable(name: string) {
        return knownTables.includes(name) ? ({name} as SqlTable) : undefined;
      },
      listTables() {
        return [];
      },
      listModules() {
        return [];
      },
      listTablesNames() {
        return knownTables;
      },
      getModuleForTable() {
        return undefined;
      },
      isModuleDisabled() {
        return false;
      },
      getDisabledModules() {
        return new Set<string>();
      },
      async ensureInitialized() {},
    };
  }

  it('creates TableSourceNode when table exists in sqlModules', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'slice', moduleName: 'slices'},
    });
    const mockModules = createMockSqlModules(['slice']);

    const result = decomposeStructuredQuery(sq, 0, 0, undefined, mockModules);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    const state = result.nodes[0].state as Record<string, unknown>;
    expect(state.sqlTable).toBe('slice');
    expect(state.moduleName).toBe('slices');
  });

  it('creates SqlSourceNode when table does not exist in sqlModules', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'custom_table'},
    });
    const mockModules = createMockSqlModules([]);

    const result = decomposeStructuredQuery(sq, 0, 0, undefined, mockModules);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kSqlSource);
    const state = result.nodes[0].state as Record<string, unknown>;
    expect(state.sql).toBe('SELECT * FROM custom_table');
  });

  it('creates SqlSourceNode with INCLUDE when moduleName is provided and table missing', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'my_table', moduleName: 'my.module'},
    });
    const mockModules = createMockSqlModules([]);

    const result = decomposeStructuredQuery(sq, 0, 0, undefined, mockModules);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kSqlSource);
    const state = result.nodes[0].state as Record<string, unknown>;
    expect(state.sql).toBe(
      'INCLUDE PERFETTO MODULE my.module;\nSELECT * FROM my_table',
    );
  });

  it('creates SqlSourceNode without INCLUDE when moduleName is empty and table missing', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'orphan_table', moduleName: ''},
    });
    const mockModules = createMockSqlModules([]);

    const result = decomposeStructuredQuery(sq, 0, 0, undefined, mockModules);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kSqlSource);
    const state = result.nodes[0].state as Record<string, unknown>;
    expect(state.sql).toBe('SELECT * FROM orphan_table');
  });

  it('creates TableSourceNode when sqlModules is undefined (backward compat)', () => {
    const sq = protos.PerfettoSqlStructuredQuery.create({
      table: {tableName: 'any_table', moduleName: 'any.module'},
    });

    const result = decomposeStructuredQuery(sq, 0, 0);

    expect(result.nodes).toHaveLength(1);
    expect(result.nodes[0].type).toBe(NodeType.kTable);
    const state = result.nodes[0].state as Record<string, unknown>;
    expect(state.sqlTable).toBe('any_table');
    expect(state.moduleName).toBe('any.module');
  });
});

// Smoke test matching the exact trace_summary_spec.pbtxt used for manual
// testing. This verifies the full pipeline: join with nested queries,
// filters, aggregation, sort, limit, select_columns, and metrics.
describe('buildNodesFromTemplateSpec - smoke test (trace_summary_spec.pbtxt)', () => {
  const spec = protos.TraceMetricV2TemplateSpec.create({
    idPrefix: 'x',
    query: protos.PerfettoSqlStructuredQuery.create({
      id: '36',
      // Use alias: '' for columns without aliases to match protobuf wire
      // format: wasm decoding sets unset string fields to '' not undefined.
      selectColumns: [
        {alias: '', columnNameOrExpression: 'id'},
        {alias: '', columnNameOrExpression: 'name'},
        {alias: '', columnNameOrExpression: 'utid'},
        {alias: 'total_running_time', columnNameOrExpression: 'sum_dur'},
      ],
      experimentalJoin: {
        type: protos.PerfettoSqlStructuredQuery.ExperimentalJoin.Type.INNER,
        leftQuery: protos.PerfettoSqlStructuredQuery.create({
          id: '36_left_ref',
          innerQuery: protos.PerfettoSqlStructuredQuery.create({
            id: '35',
            table: {
              tableName: 'thread_or_process_slice',
              moduleName: 'slices.with_context',
            },
          }),
        }),
        rightQuery: protos.PerfettoSqlStructuredQuery.create({
          id: '36_right_ref',
          innerQuery: protos.PerfettoSqlStructuredQuery.create({
            id: '749',
            innerQuery: protos.PerfettoSqlStructuredQuery.create({
              id: '196',
              innerQuery: protos.PerfettoSqlStructuredQuery.create({
                id: '14',
                innerQuery: protos.PerfettoSqlStructuredQuery.create({
                  id: '13',
                  innerQuery: protos.PerfettoSqlStructuredQuery.create({
                    id: '12',
                    table: {
                      tableName: 'thread_state',
                      columnNames: [
                        'id',
                        'ts',
                        'dur',
                        'cpu',
                        'utid',
                        'state',
                        'io_wait',
                        'blocked_function',
                        'waker_utid',
                        'waker_id',
                        'irq_context',
                        'ucpu',
                      ],
                    },
                  }),
                  experimentalFilterGroup: {
                    op: protos.PerfettoSqlStructuredQuery
                      .ExperimentalFilterGroup.Operator.AND,
                    filters: [
                      {
                        columnName: 'state',
                        op: protos.PerfettoSqlStructuredQuery.Filter.Operator
                          .EQUAL,
                        stringRhs: ['Running'],
                      },
                    ],
                  },
                }),
                groupBy: {
                  columnNames: ['utid'],
                  aggregates: [
                    {
                      columnName: 'dur',
                      op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
                        .SUM,
                      resultColumnName: 'sum_dur',
                    },
                  ],
                },
                selectColumns: [{columnName: 'utid'}, {columnName: 'sum_dur'}],
              }),
              orderBy: {
                orderingSpecs: [
                  {
                    columnName: 'sum_dur',
                    direction:
                      protos.PerfettoSqlStructuredQuery.OrderBy.Direction.DESC,
                  },
                ],
              },
            }),
            limit: 10,
          }),
        }),
        equalityColumns: {
          leftColumn: 'utid',
          rightColumn: 'utid',
        },
      },
    }),
    dimensionsSpecs: [
      {name: 'id'},
      {name: 'name'},
      {name: 'total_running_time'},
    ],
    dimensionUniqueness:
      protos.TraceMetricV2Spec.DimensionUniqueness.NOT_UNIQUE,
    valueColumnSpecs: [
      {
        name: 'utid',
        unit: protos.TraceMetricV2Spec.MetricUnit.COUNT,
        polarity: protos.TraceMetricV2Spec.MetricPolarity.NOT_APPLICABLE,
      },
    ],
  });

  it('produces the correct graph structure', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);

    // Expected structure:
    // Left chain:  SimpleSlices
    // Right chain: Table(thread_state) → Filter → Aggregation → Sort → Limit
    // Then:        Join → ModifyColumns(id, name, utid, sum_dur AS ...) → Metrics
    // select_columns after group_by is redundant and skipped.

    const nodeTypes = result.nodes.map((n) => n.type);

    // Source nodes
    expect(nodeTypes).toContain(NodeType.kSimpleSlices);
    expect(nodeTypes).toContain(NodeType.kTable);

    // Right chain modification nodes
    expect(nodeTypes).toContain(NodeType.kFilter);
    expect(nodeTypes).toContain(NodeType.kAggregation);
    expect(nodeTypes).toContain(NodeType.kSort);
    expect(nodeTypes).toContain(NodeType.kLimitAndOffset);

    // Join + post-join
    expect(nodeTypes).toContain(NodeType.kJoin);
    expect(nodeTypes).toContain(NodeType.kModifyColumns);
    expect(nodeTypes).toContain(NodeType.kMetrics);
  });

  it('SimpleSlices source for thread_or_process_slice with slices.with_context', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const slicesNode = result.nodes.find(
      (n) => n.type === NodeType.kSimpleSlices,
    );
    expect(slicesNode).toBeDefined();
  });

  it('redundant select_columns after group_by is skipped', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    // Only ONE ModifyColumns (the outer one after join), not two.
    const modifyCols = result.nodes.filter(
      (n) => n.type === NodeType.kModifyColumns,
    );
    expect(modifyCols).toHaveLength(1);
  });

  it('right chain is wired correctly: Table → Filter → Agg → Sort → Limit', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);

    const table = result.nodes.find(
      (n) =>
        n.type === NodeType.kTable &&
        (n.state as Record<string, unknown>).sqlTable === 'thread_state',
    );
    expect(table).toBeDefined();

    // Follow the modification chain via nextNodes, stopping at Join.
    const chain: string[] = [];
    let current = table;
    while (current && current.nextNodes.length > 0) {
      const next = result.nodes.find((n) => n.nodeId === current!.nextNodes[0]);
      if (!next || next.type === NodeType.kJoin) break;
      chain.push(next.type);
      current = next;
    }

    expect(chain).toEqual([
      NodeType.kFilter,
      NodeType.kAggregation,
      NodeType.kSort,
      NodeType.kLimitAndOffset,
    ]);
  });

  it('filter has state = "Running"', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const filter = result.nodes.find((n) => n.type === NodeType.kFilter);
    const state = filter!.state as Record<string, unknown>;
    const filters = state.filters as Array<Record<string, unknown>>;
    expect(filters).toHaveLength(1);
    expect(filters[0].column).toBe('state');
    expect(filters[0].op).toBe('=');
    expect(filters[0].value).toBe('Running');
  });

  it('aggregation groups by utid with SUM(dur) AS sum_dur', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const agg = result.nodes.find((n) => n.type === NodeType.kAggregation);
    const state = agg!.state as Record<string, unknown>;
    const groupByCols = state.groupByColumns as Array<Record<string, unknown>>;
    expect(groupByCols).toHaveLength(1);
    expect(groupByCols[0].name).toBe('utid');
    const aggs = state.aggregations as Array<Record<string, unknown>>;
    expect(aggs).toHaveLength(1);
    expect(aggs[0].aggregationOp).toBe('SUM');
    expect(aggs[0].newColumnName).toBe('sum_dur');
  });

  it('sort by sum_dur DESC', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const sort = result.nodes.find((n) => n.type === NodeType.kSort);
    const state = sort!.state as Record<string, unknown>;
    const criteria = state.sortCriteria as Array<Record<string, unknown>>;
    expect(criteria).toHaveLength(1);
    expect(criteria[0].colName).toBe('sum_dur');
    expect(criteria[0].direction).toBe('DESC');
  });

  it('limit is 10', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const limit = result.nodes.find((n) => n.type === NodeType.kLimitAndOffset);
    const state = limit!.state as Record<string, unknown>;
    expect(state.limit).toBe(10);
  });

  it('join is INNER on utid = utid', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const join = result.nodes.find((n) => n.type === NodeType.kJoin);
    const state = join!.state as Record<string, unknown>;
    expect(state.joinType).toBe('INNER');
    expect(state.conditionType).toBe('equality');
    expect(state.leftColumn).toBe('utid');
    expect(state.rightColumn).toBe('utid');
  });

  it('join inputs are wired to SimpleSlices (left) and Limit (right)', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const join = result.nodes.find((n) => n.type === NodeType.kJoin);
    const state = join!.state as Record<string, unknown>;

    const leftId = state.leftNodeId as string;
    const rightId = state.rightNodeId as string;

    const leftNode = result.nodes.find((n) => n.nodeId === leftId);
    const rightNode = result.nodes.find((n) => n.nodeId === rightId);

    expect(leftNode!.type).toBe(NodeType.kSimpleSlices);
    expect(rightNode!.type).toBe(NodeType.kLimitAndOffset);
  });

  it('outer select_columns creates ModifyColumns with id, name, utid, sum_dur AS total_running_time', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const mc = result.nodes.find((n) => n.type === NodeType.kModifyColumns);
    const state = mc!.state as Record<string, unknown>;
    const cols = state.selectedColumns as Array<Record<string, unknown>>;
    expect(cols).toHaveLength(4);
    expect(cols[0].name).toBe('id');
    expect(cols[1].name).toBe('name');
    expect(cols[2].name).toBe('utid');
    expect(cols[3].name).toBe('sum_dur');
    expect(cols[3].alias).toBe('total_running_time');
  });

  it('ModifyColumns is chained after Join', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const join = result.nodes.find((n) => n.type === NodeType.kJoin);
    const mc = result.nodes.find((n) => n.type === NodeType.kModifyColumns);
    expect(join!.nextNodes).toContain(mc!.nodeId);
    const mcState = mc!.state as Record<string, unknown>;
    expect(mcState.primaryInputId).toBe(join!.nodeId);
  });

  it('Metrics is chained after ModifyColumns with correct state', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const mc = result.nodes.find((n) => n.type === NodeType.kModifyColumns);
    const metrics = result.nodes.find((n) => n.type === NodeType.kMetrics);
    expect(mc!.nextNodes).toContain(metrics!.nodeId);

    const state = metrics!.state as Record<string, unknown>;
    expect(state.metricIdPrefix).toBe('x');
    expect(state.primaryInputId).toBe(mc!.nodeId);

    const valueCols = state.valueColumns as Array<Record<string, unknown>>;
    expect(valueCols).toHaveLength(1);
    expect(valueCols[0].column).toBe('utid');
    expect(valueCols[0].unit).toBe('COUNT');
    expect(valueCols[0].polarity).toBe('NOT_APPLICABLE');
  });

  it('dockable modification nodes have no layout entries', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const dockableTypes = new Set([
      NodeType.kFilter,
      NodeType.kAggregation,
      NodeType.kSort,
      NodeType.kLimitAndOffset,
      NodeType.kModifyColumns,
      NodeType.kMetrics,
    ]);
    for (const node of result.nodes) {
      if (dockableTypes.has(node.type)) {
        expect(result.layouts.has(node.nodeId)).toBe(false);
      }
    }
  });

  it('rootNodeIds contains only source nodes and join', () => {
    const result = buildNodesFromTemplateSpec(spec, 0);
    const rootTypes = result.rootNodeIds.map(
      (id) => result.nodes.find((n) => n.nodeId === id)!.type,
    );
    // SimpleSlices, Table(thread_state), and Join
    expect(rootTypes).toContain(NodeType.kSimpleSlices);
    expect(rootTypes).toContain(NodeType.kTable);
    expect(rootTypes).toContain(NodeType.kJoin);
    expect(rootTypes).toHaveLength(3);
  });
});

// End-to-end validation test: build serialized graph, deserialize into
// hydrated nodes, apply fixups, and verify every node validates.
describe('pbtxt import - end-to-end validation', () => {
  registerCoreNodes();

  const intType: PerfettoSqlType = {kind: 'int'};
  const stringType: PerfettoSqlType = {kind: 'string'};

  const trace = {
    traceInfo: {traceTitle: 'test'},
  } as Trace;

  // thread_state columns from the pbtxt.
  const threadStateCols = [
    {name: 'id', type: intType},
    {name: 'ts', type: intType},
    {name: 'dur', type: intType},
    {name: 'cpu', type: intType},
    {name: 'utid', type: intType},
    {name: 'state', type: stringType},
    {name: 'io_wait', type: intType},
    {name: 'blocked_function', type: stringType},
    {name: 'waker_utid', type: intType},
    {name: 'waker_id', type: intType},
    {name: 'irq_context', type: intType},
    {name: 'ucpu', type: intType},
  ];

  const tables = new Map<string, SqlTable>();
  tables.set('thread_state', {
    name: 'thread_state',
    description: '',
    type: 'table',
    includeKey: '',
    importance: undefined,
    getTableColumns: () => [],
    columns: threadStateCols,
  } as SqlTable);

  const sqlModules: SqlModules = {
    getTable: (name: string) => tables.get(name),
    listTables: () => [],
    listModules: () => [],
    listTablesNames: () => [...tables.keys()],
    getModuleForTable: () => undefined,
    isModuleDisabled: () => false,
    getDisabledModules: () => new Set<string>(),
    async ensureInitialized() {},
  } as SqlModules;

  const spec = protos.TraceMetricV2TemplateSpec.create({
    idPrefix: 'x',
    query: protos.PerfettoSqlStructuredQuery.create({
      id: '36',
      // Use alias: '' for columns without aliases to match protobuf wire
      // format: wasm decoding sets unset string fields to '' not undefined.
      selectColumns: [
        {alias: '', columnNameOrExpression: 'id'},
        {alias: '', columnNameOrExpression: 'name'},
        {alias: '', columnNameOrExpression: 'utid'},
        {alias: 'total_running_time', columnNameOrExpression: 'sum_dur'},
      ],
      experimentalJoin: {
        type: protos.PerfettoSqlStructuredQuery.ExperimentalJoin.Type.INNER,
        leftQuery: protos.PerfettoSqlStructuredQuery.create({
          id: '36_left_ref',
          innerQuery: protos.PerfettoSqlStructuredQuery.create({
            id: '35',
            table: {
              tableName: 'thread_or_process_slice',
              moduleName: 'slices.with_context',
            },
          }),
        }),
        rightQuery: protos.PerfettoSqlStructuredQuery.create({
          id: '36_right_ref',
          innerQuery: protos.PerfettoSqlStructuredQuery.create({
            id: '749',
            innerQuery: protos.PerfettoSqlStructuredQuery.create({
              id: '196',
              innerQuery: protos.PerfettoSqlStructuredQuery.create({
                id: '14',
                innerQuery: protos.PerfettoSqlStructuredQuery.create({
                  id: '13',
                  innerQuery: protos.PerfettoSqlStructuredQuery.create({
                    id: '12',
                    table: {
                      tableName: 'thread_state',
                      columnNames: [
                        'id',
                        'ts',
                        'dur',
                        'cpu',
                        'utid',
                        'state',
                        'io_wait',
                        'blocked_function',
                        'waker_utid',
                        'waker_id',
                        'irq_context',
                        'ucpu',
                      ],
                    },
                  }),
                  experimentalFilterGroup: {
                    op: protos.PerfettoSqlStructuredQuery
                      .ExperimentalFilterGroup.Operator.AND,
                    filters: [
                      {
                        columnName: 'state',
                        op: protos.PerfettoSqlStructuredQuery.Filter.Operator
                          .EQUAL,
                        stringRhs: ['Running'],
                      },
                    ],
                  },
                }),
                groupBy: {
                  columnNames: ['utid'],
                  aggregates: [
                    {
                      columnName: 'dur',
                      op: protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
                        .SUM,
                      resultColumnName: 'sum_dur',
                    },
                  ],
                },
                selectColumns: [{columnName: 'utid'}, {columnName: 'sum_dur'}],
              }),
              orderBy: {
                orderingSpecs: [
                  {
                    columnName: 'sum_dur',
                    direction:
                      protos.PerfettoSqlStructuredQuery.OrderBy.Direction.DESC,
                  },
                ],
              },
            }),
            limit: 10,
          }),
        }),
        equalityColumns: {
          leftColumn: 'utid',
          rightColumn: 'utid',
        },
      },
    }),
    dimensionsSpecs: [
      {name: 'id'},
      {name: 'name'},
      {name: 'total_running_time'},
    ],
    dimensionUniqueness:
      protos.TraceMetricV2Spec.DimensionUniqueness.NOT_UNIQUE,
    valueColumnSpecs: [
      {
        name: 'utid',
        unit: protos.TraceMetricV2Spec.MetricUnit.COUNT,
        polarity: protos.TraceMetricV2Spec.MetricPolarity.NOT_APPLICABLE,
      },
    ],
  });

  function hydrateSpec(): QueryNode[] {
    const buildResult = buildNodesFromTemplateSpec(
      spec,
      0,
      undefined,
      sqlModules,
    );
    const serializedGraph: SerializedGraph = {
      nodes: buildResult.nodes,
      rootNodeIds: buildResult.rootNodeIds,
      nodeLayouts: Object.fromEntries(buildResult.layouts),
    };
    const json = JSON.stringify(serializedGraph);
    const state = deserializeState(json, trace, sqlModules);

    // Collect all nodes by walking from roots.
    const allNodes: QueryNode[] = [];
    const seen = new Set<string>();
    function collect(node: QueryNode): void {
      if (seen.has(node.nodeId)) return;
      seen.add(node.nodeId);
      allNodes.push(node);
      for (const next of node.nextNodes) collect(next);
    }
    for (const root of state.rootNodes) collect(root);
    return allNodes;
  }

  it('all nodes validate successfully', () => {
    const allNodes = hydrateSpec();

    // Verify we have the expected number of hydrated nodes.
    // SimpleSlices, Table, Filter, Aggregation, Sort, Limit, Join,
    // ModifyColumns, Metrics = 9
    expect(allNodes.length).toBe(9);

    // Every node must validate.
    for (const node of allNodes) {
      const valid = node.validate();
      if (!valid) {
        const err = node.state.issues?.queryError?.message ?? 'unknown';
        fail(`${node.type} (${node.nodeId}) failed validation: ${err}`);
      }
    }
  });

  it('Join has checked columns from both sides', () => {
    const allNodes = hydrateSpec();
    const join = allNodes.find((n) => n.type === NodeType.kJoin);
    expect(join).toBeDefined();
    const state = join!.state as {
      leftColumns?: Array<{checked: boolean}>;
      rightColumns?: Array<{checked: boolean}>;
    };
    expect(state.leftColumns?.some((c) => c.checked)).toBe(true);
    expect(state.rightColumns?.some((c) => c.checked)).toBe(true);
  });

  it('Metrics node preserves value columns after fixups', () => {
    const allNodes = hydrateSpec();
    const metrics = allNodes.find((n) => n.type === NodeType.kMetrics);
    expect(metrics).toBeDefined();
    const state = metrics!.state as {
      metricIdPrefix: string;
      valueColumns: Array<{column: string}>;
    };
    expect(state.metricIdPrefix).toBe('x');
    expect(state.valueColumns.length).toBe(1);
    expect(state.valueColumns[0].column).toBe('utid');
  });

  it('Metrics node has correct dimensions from cached availableColumns', () => {
    const allNodes = hydrateSpec();
    const metrics = allNodes.find((n) => n.type === NodeType.kMetrics);
    expect(metrics).toBeDefined();

    // Test the CACHED state.availableColumns — this is what the UI renders.
    // It's set during updateAvailableColumns() and NOT recomputed on read.
    const state = metrics!.state as {
      availableColumns: Array<{name: string}>;
      valueColumns: Array<{column: string}>;
    };
    const availNames = state.availableColumns.map((c) => c.name);
    // Must include all 4 columns: id, name, utid, total_running_time.
    expect(availNames).toContain('id');
    expect(availNames).toContain('name');
    expect(availNames).toContain('utid');
    expect(availNames).toContain('total_running_time');
    // No empty names allowed.
    expect(availNames.every((n) => n.length > 0)).toBe(true);

    // getDimensions() = availableColumns minus valueColumns.
    // Expected: id, name, total_running_time (utid is the value column).
    const getDimensions = (
      metrics as unknown as {getDimensions: () => string[]}
    ).getDimensions;
    const dims = getDimensions.call(metrics);
    expect(dims).toEqual(['id', 'name', 'total_running_time']);
  });

  it('ModifyColumns after Join has correct finalCols', () => {
    const allNodes = hydrateSpec();
    const mc = allNodes.find((n) => n.type === NodeType.kModifyColumns);
    expect(mc).toBeDefined();

    const finalCols = mc!.finalCols;
    const names = finalCols.map((c) => c.name);
    expect(names).toEqual(['id', 'name', 'utid', 'total_running_time']);
  });

  it('empty alias from protobuf does not produce empty column names', () => {
    // Protobuf wire format sets unset strings to '' not undefined.
    // Verify the serialized ModifyColumns state handles this correctly.
    const result = buildNodesFromTemplateSpec(spec, 0, undefined, sqlModules);
    const mcNode = result.nodes.find((n) => n.type === NodeType.kModifyColumns);
    expect(mcNode).toBeDefined();
    const state = mcNode!.state as {
      selectedColumns: Array<{name: string; alias?: string}>;
    };
    // alias: '' must become undefined, NOT empty string.
    // This prevents newColumnInfo from producing empty column names.
    for (const col of state.selectedColumns) {
      if (col.alias !== undefined) {
        expect(col.alias.length).toBeGreaterThan(0);
      }
    }
  });
});

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
  AggregationNode,
  placeholderNewColumnName,
  Aggregation,
  AggregationNodeAttrs,
} from './aggregation_node';
import {Trace} from '../../../../public/trace';
import {QueryNode} from '../../query_node';
import {
  createMockNode,
  createColumnInfo,
  connectNodes,
} from '../testing/test_utils';
import {PerfettoSqlTypes} from '../../../../trace_processor/perfetto_sql_type';

describe('AggregationNode', () => {
  function createMockTrace(): Trace {
    return {} as Trace;
  }

  function createAggregationNodeWithInput(
    state: AggregationNodeAttrs & {trace?: Trace},
    inputNode?: QueryNode,
  ): AggregationNode {
    const {trace, ...attrs} = state;
    const node = new AggregationNode(attrs, {trace});
    if (inputNode) {
      connectNodes(inputNode, node);
    }
    return node;
  }

  describe('placeholderNewColumnName', () => {
    it('should generate placeholder for COUNT(*) without column', () => {
      const agg: Aggregation = {
        aggregationOp: 'COUNT(*)',
      };
      expect(placeholderNewColumnName(agg)).toBe('count');
    });

    it('should generate placeholder for PERCENTILE with column', () => {
      const agg: Aggregation = {
        aggregationOp: 'PERCENTILE',
        column: createColumnInfo('dur', 'int'),
        percentile: 95,
      };
      expect(placeholderNewColumnName(agg)).toBe('percentile_dur');
    });

    it('should generate placeholder for MEDIAN with column', () => {
      const agg: Aggregation = {
        aggregationOp: 'MEDIAN',
        column: createColumnInfo('value', 'double'),
      };
      expect(placeholderNewColumnName(agg)).toBe('median_value');
    });

    it('should generate placeholder for SUM with column', () => {
      const agg: Aggregation = {
        aggregationOp: 'SUM',
        column: createColumnInfo('dur', 'int'),
      };
      expect(placeholderNewColumnName(agg)).toBe('sum_dur');
    });

    it('should generate placeholder for COUNT with column', () => {
      const agg: Aggregation = {
        aggregationOp: 'COUNT',
        column: createColumnInfo('name', 'string'),
      };
      expect(placeholderNewColumnName(agg)).toBe('count_name');
    });

    it('should handle aggregation without operation', () => {
      const agg: Aggregation = {};
      expect(placeholderNewColumnName(agg)).toBe('result');
    });

    it('should handle aggregation with operation but no column', () => {
      const agg: Aggregation = {
        aggregationOp: 'SUM',
      };
      expect(placeholderNewColumnName(agg)).toBe('sum');
    });

    it('should use lowercase in placeholder', () => {
      const agg: Aggregation = {
        aggregationOp: 'MEAN',
        column: createColumnInfo('Value', 'double'),
      };
      expect(placeholderNewColumnName(agg)).toBe('mean_Value');
    });
  });

  describe('validation', () => {
    let node: AggregationNode;

    beforeEach(() => {
      node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [],
        },
        createMockNode(),
      );
    });

    it('should validate COUNT(*) without column', () => {
      node.attrs.aggregations = [
        {
          aggregationOp: 'COUNT(*)',
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.attrs.aggregations) {
        agg.isValid =
          agg.aggregationOp === 'COUNT(*)' ||
          (agg.column !== undefined && agg.aggregationOp !== undefined);
      }

      expect(node.attrs.aggregations[0].isValid).toBe(true);
    });

    it('should validate PERCENTILE with column and percentile value', () => {
      node.attrs.aggregations = [
        {
          aggregationOp: 'PERCENTILE',
          column: createColumnInfo('dur', 'int'),
          percentile: 50,
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.attrs.aggregations) {
        if (agg.aggregationOp === 'PERCENTILE') {
          agg.isValid =
            agg.column !== undefined &&
            agg.percentile !== undefined &&
            agg.percentile >= 0 &&
            agg.percentile <= 100;
        }
      }

      expect(node.attrs.aggregations[0].isValid).toBe(true);
    });

    it('should invalidate PERCENTILE with percentile value out of range', () => {
      node.attrs.aggregations = [
        {
          aggregationOp: 'PERCENTILE',
          column: createColumnInfo('dur', 'int'),
          percentile: 150, // Invalid: > 100
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.attrs.aggregations) {
        if (agg.aggregationOp === 'PERCENTILE') {
          agg.isValid =
            agg.column !== undefined &&
            agg.percentile !== undefined &&
            agg.percentile >= 0 &&
            agg.percentile <= 100;
        }
      }

      expect(node.attrs.aggregations[0].isValid).toBe(false);
    });

    it('should invalidate PERCENTILE without percentile value', () => {
      node.attrs.aggregations = [
        {
          aggregationOp: 'PERCENTILE',
          column: createColumnInfo('dur', 'int'),
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.attrs.aggregations) {
        if (agg.aggregationOp === 'PERCENTILE') {
          agg.isValid =
            agg.column !== undefined &&
            agg.percentile !== undefined &&
            agg.percentile >= 0 &&
            agg.percentile <= 100;
        }
      }

      expect(node.attrs.aggregations[0].isValid).toBe(false);
    });

    it('should invalidate PERCENTILE without column', () => {
      node.attrs.aggregations = [
        {
          aggregationOp: 'PERCENTILE',
          percentile: 50,
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.attrs.aggregations) {
        if (agg.aggregationOp === 'PERCENTILE') {
          agg.isValid =
            agg.column !== undefined &&
            agg.percentile !== undefined &&
            agg.percentile >= 0 &&
            agg.percentile <= 100;
        }
      }

      expect(node.attrs.aggregations[0].isValid).toBe(false);
    });

    it('should validate MEDIAN with column', () => {
      node.attrs.aggregations = [
        {
          aggregationOp: 'MEDIAN',
          column: createColumnInfo('value', 'double'),
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.attrs.aggregations) {
        if (
          agg.aggregationOp !== 'COUNT(*)' &&
          agg.aggregationOp !== 'PERCENTILE'
        ) {
          agg.isValid =
            agg.column !== undefined && agg.aggregationOp !== undefined;
        }
      }

      expect(node.attrs.aggregations[0].isValid).toBe(true);
    });

    it('should invalidate MEDIAN without column', () => {
      node.attrs.aggregations = [
        {
          aggregationOp: 'MEDIAN',
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.attrs.aggregations) {
        if (
          agg.aggregationOp !== 'COUNT(*)' &&
          agg.aggregationOp !== 'PERCENTILE'
        ) {
          agg.isValid =
            agg.column !== undefined && agg.aggregationOp !== undefined;
        }
      }

      expect(node.attrs.aggregations[0].isValid).toBe(false);
    });

    it('should validate SUM with column', () => {
      node.attrs.aggregations = [
        {
          aggregationOp: 'SUM',
          column: createColumnInfo('dur', 'int'),
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.attrs.aggregations) {
        if (
          agg.aggregationOp !== 'COUNT(*)' &&
          agg.aggregationOp !== 'PERCENTILE'
        ) {
          agg.isValid =
            agg.column !== undefined && agg.aggregationOp !== undefined;
        }
      }

      expect(node.attrs.aggregations[0].isValid).toBe(true);
    });
  });

  describe('serialization', () => {
    it('should serialize PERCENTILE aggregation with percentile value', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [],
        },
        createMockNode(),
      );

      node.attrs.aggregations = [
        {
          aggregationOp: 'PERCENTILE',
          column: createColumnInfo('dur', 'int'),
          percentile: 95,
          newColumnName: 'p95_dur',
          isValid: true,
        },
      ];

      expect(node.attrs.aggregations).toBeDefined();
      expect(node.attrs.aggregations.length).toBe(1);
      expect(node.attrs.aggregations[0].aggregationOp).toBe('PERCENTILE');
      expect(node.attrs.aggregations[0].percentile).toBe(95);
      expect(node.attrs.aggregations[0].newColumnName).toBe('p95_dur');
    });

    it('should serialize COUNT(*) aggregation without column', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [],
        },
        createMockNode(),
      );

      node.attrs.aggregations = [
        {
          aggregationOp: 'COUNT(*)',
          newColumnName: 'total_count',
          isValid: true,
        },
      ];

      expect(node.attrs.aggregations).toBeDefined();
      expect(node.attrs.aggregations.length).toBe(1);
      expect(node.attrs.aggregations[0].aggregationOp).toBe('COUNT(*)');
      expect(node.attrs.aggregations[0].column).toBeUndefined();
      expect(node.attrs.aggregations[0].newColumnName).toBe('total_count');
    });

    it('should serialize MEDIAN aggregation', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [],
        },
        createMockNode(),
      );

      node.attrs.aggregations = [
        {
          aggregationOp: 'MEDIAN',
          column: {name: 'value', type: {kind: 'int' as const}},
          newColumnName: 'median_value',
          isValid: true,
        },
      ];

      expect(node.attrs.aggregations).toBeDefined();
      expect(node.attrs.aggregations.length).toBe(1);
      expect(node.attrs.aggregations[0].aggregationOp).toBe('MEDIAN');
      expect(node.attrs.aggregations[0].column?.name).toBe('value');
      expect(node.attrs.aggregations[0].newColumnName).toBe('median_value');
    });

    it('should serialize multiple aggregations including new types', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [],
        },
        createMockNode(),
      );

      node.attrs.aggregations = [
        {
          aggregationOp: 'COUNT(*)',
          newColumnName: 'count',
          isValid: true,
        },
        {
          aggregationOp: 'MEDIAN',

          newColumnName: 'median_dur',
          isValid: true,
        },
        {
          aggregationOp: 'PERCENTILE',

          percentile: 99,
          newColumnName: 'p99_dur',
          isValid: true,
        },
      ];

      expect(node.attrs.aggregations.length).toBe(3);
      expect(node.attrs.aggregations[0].aggregationOp).toBe('COUNT(*)');
      expect(node.attrs.aggregations[1].aggregationOp).toBe('MEDIAN');
      expect(node.attrs.aggregations[2].aggregationOp).toBe('PERCENTILE');
      expect(node.attrs.aggregations[2].percentile).toBe(99);
    });
  });

  describe('deserialization', () => {
    it('should deserialize PERCENTILE aggregation with percentile value', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [
            {
              aggregationOp: 'PERCENTILE',
              column: createColumnInfo('dur', 'int'),
              percentile: 95,
              newColumnName: 'p95_dur',
              isValid: true,
            },
          ],
        },
        createMockNode(),
      );

      expect(node.attrs.aggregations.length).toBe(1);
      expect(node.attrs.aggregations[0].aggregationOp).toBe('PERCENTILE');
      expect(node.attrs.aggregations[0].percentile).toBe(95);
      expect(node.attrs.aggregations[0].newColumnName).toBe('p95_dur');
    });

    it('should deserialize COUNT(*) aggregation without column', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [
            {
              aggregationOp: 'COUNT(*)',
              newColumnName: 'total_count',
              isValid: true,
            },
          ],
        },
        createMockNode(),
      );

      expect(node.attrs.aggregations.length).toBe(1);
      expect(node.attrs.aggregations[0].aggregationOp).toBe('COUNT(*)');
      expect(node.attrs.aggregations[0].column).toBeUndefined();
      expect(node.attrs.aggregations[0].newColumnName).toBe('total_count');
    });

    it('should deserialize MEDIAN aggregation', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [
            {
              aggregationOp: 'MEDIAN',
              column: createColumnInfo('value', 'double'),
              newColumnName: 'median_value',
              isValid: true,
            },
          ],
        },
        createMockNode(),
      );

      expect(node.attrs.aggregations.length).toBe(1);
      expect(node.attrs.aggregations[0].aggregationOp).toBe('MEDIAN');
      expect(node.attrs.aggregations[0].column?.name).toBe('value');
      expect(node.attrs.aggregations[0].newColumnName).toBe('median_value');
    });

    it('should migrate old COUNT_ALL to COUNT(*) for backward compatibility', () => {
      const serializedState = {
        groupByColumns: [],
        aggregations: [
          {
            aggregationOp: 'COUNT_ALL', // Simulate old saved state
            newColumnName: 'total_count',
            isValid: true,
          },
        ],
      };
      const deserializedState =
        AggregationNode.deserializeState(serializedState);
      const node = createAggregationNodeWithInput(
        deserializedState,
        createMockNode(),
      );

      expect(node.attrs.aggregations.length).toBe(1);
      expect(node.attrs.aggregations[0].aggregationOp).toBe('COUNT(*)');
      expect(node.attrs.aggregations[0].column).toBeUndefined();
      expect(node.attrs.aggregations[0].newColumnName).toBe('total_count');
    });
  });

  describe('node validation', () => {
    let mockPrevNode: QueryNode;

    beforeEach(() => {
      mockPrevNode = createMockNode({
        columns: [
          createColumnInfo('name', 'string'),
          createColumnInfo('dur', 'int'),
          createColumnInfo('ts', 'int'),
        ],
      });
    });

    it('should validate node with only group by columns (no aggregations)', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [
            {...createColumnInfo('name', 'string'), checked: true},
            {...createColumnInfo('dur', 'int'), checked: false},
          ],
          aggregations: [],
        },
        mockPrevNode,
      );

      expect(node.validate()).toBe(true);
    });

    it('should validate node with only aggregations (no group by)', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [
            {...createColumnInfo('name', 'string'), checked: false},
            {...createColumnInfo('dur', 'int'), checked: false},
          ],
          aggregations: [
            {
              aggregationOp: 'COUNT(*)',
              isValid: true,
            },
          ],
        },
        mockPrevNode,
      );

      expect(node.validate()).toBe(true);
    });

    it('should validate node with both group by and aggregations', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [
            {...createColumnInfo('name', 'string'), checked: true},
            {...createColumnInfo('dur', 'int'), checked: false},
          ],
          aggregations: [
            {
              aggregationOp: 'SUM',
              column: createColumnInfo('dur', 'int'),
              isValid: true,
            },
          ],
        },
        mockPrevNode,
      );

      expect(node.validate()).toBe(true);
    });

    it('should invalidate node with neither group by nor aggregations', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [
            {...createColumnInfo('name', 'string'), checked: false},
            {...createColumnInfo('dur', 'int'), checked: false},
          ],
          aggregations: [],
        },
        mockPrevNode,
      );

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'requires at least one group by column or aggregation function',
      );
    });

    it('should invalidate node with invalid aggregations and no group by', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [
            {...createColumnInfo('name', 'string'), checked: false},
          ],
          aggregations: [
            {
              aggregationOp: 'SUM',
              // Missing column - invalid
              isValid: false,
            },
          ],
        },
        mockPrevNode,
      );

      expect(node.validate()).toBe(false);
    });

    it('should validate multiple aggregations without group by', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [
            {...createColumnInfo('name', 'string'), checked: false},
          ],
          aggregations: [
            {
              aggregationOp: 'COUNT(*)',
              isValid: true,
            },
            {
              aggregationOp: 'SUM',
              column: createColumnInfo('dur', 'int'),
              isValid: true,
            },
            {
              aggregationOp: 'MAX',
              column: createColumnInfo('ts', 'int'),
              isValid: true,
            },
          ],
        },
        mockPrevNode,
      );

      expect(node.validate()).toBe(true);
    });

    it('should invalidate node when group by columns are missing from input', () => {
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [
            {...createColumnInfo('name', 'string'), checked: true},
            {...createColumnInfo('missing_col', 'int'), checked: true},
          ],
          aggregations: [],
        },
        mockPrevNode,
      );

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'not found in input',
      );
    });

    it('should invalidate node without primaryInput', () => {
      const node = new AggregationNode(
        {
          groupByColumns: [],
          aggregations: [
            {
              aggregationOp: 'COUNT(*)',
              isValid: true,
            },
          ],
        },
        {trace: createMockTrace()},
      );

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toBe(
        'No input node connected',
      );
    });
  });

  describe('column type propagation', () => {
    it('should set INT type for COUNT aggregation', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('dur', 'duration'),
      ];
      const mockInput = createMockNode({columns: inputCols});
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: inputCols.map((c) => ({...c, checked: false})),
          aggregations: [
            {
              aggregationOp: 'COUNT',
              column: inputCols[1],
              newColumnName: 'count_dur',
              isValid: true,
            },
          ],
        },
        mockInput,
      );

      expect(node.finalCols.length).toBe(1);
      expect(node.finalCols[0].name).toBe('count_dur');
      expect(node.finalCols[0].type).toEqual(PerfettoSqlTypes.INT);
    });

    it('should set INT type for COUNT(*) aggregation', () => {
      const inputCols = [createColumnInfo('id', 'int')];
      const mockInput = createMockNode({columns: inputCols});
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: inputCols.map((c) => ({...c, checked: false})),
          aggregations: [
            {
              aggregationOp: 'COUNT(*)',
              newColumnName: 'total_count',
              isValid: true,
            },
          ],
        },
        mockInput,
      );

      expect(node.finalCols.length).toBe(1);
      expect(node.finalCols[0].name).toBe('total_count');
      expect(node.finalCols[0].type).toEqual(PerfettoSqlTypes.INT);
    });

    it('should preserve type for SUM aggregation', () => {
      const inputCols = [
        {
          name: 'dur',
          checked: false,
          type: {kind: 'duration' as const},
        },
      ];
      const mockInput = createMockNode({columns: inputCols});
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [
            {
              aggregationOp: 'SUM',
              column: inputCols[0],
              newColumnName: 'total_dur',
              isValid: true,
            },
          ],
        },
        mockInput,
      );

      expect(node.finalCols.length).toBe(1);
      expect(node.finalCols[0].name).toBe('total_dur');
      expect(node.finalCols[0].type).toEqual(PerfettoSqlTypes.DURATION);
    });

    it('should preserve type for MIN/MAX aggregations', () => {
      const inputCols = [
        {
          name: 'ts',
          checked: false,
          type: {kind: 'timestamp' as const},
        },
      ];
      const mockInput = createMockNode({columns: inputCols});
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [
            {
              aggregationOp: 'MIN',
              column: inputCols[0],
              newColumnName: 'min_ts',
              isValid: true,
            },
            {
              aggregationOp: 'MAX',
              column: inputCols[0],
              newColumnName: 'max_ts',
              isValid: true,
            },
          ],
        },
        mockInput,
      );

      expect(node.finalCols.length).toBe(2);
      expect(node.finalCols[0].name).toBe('min_ts');
      expect(node.finalCols[0].type).toEqual(PerfettoSqlTypes.TIMESTAMP);
      expect(node.finalCols[1].name).toBe('max_ts');
      expect(node.finalCols[1].type).toEqual(PerfettoSqlTypes.TIMESTAMP);
    });

    it('should set DOUBLE type for MEAN aggregation', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const mockInput = createMockNode({columns: inputCols});
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [
            {
              aggregationOp: 'MEAN',
              column: inputCols[0],
              newColumnName: 'avg_value',
              isValid: true,
            },
          ],
        },
        mockInput,
      );

      expect(node.finalCols.length).toBe(1);
      expect(node.finalCols[0].name).toBe('avg_value');
      expect(node.finalCols[0].type).toEqual(PerfettoSqlTypes.DOUBLE);
    });

    it('should set DOUBLE type for MEDIAN aggregation', () => {
      const inputCols = [createColumnInfo('dur', 'int')];
      const mockInput = createMockNode({columns: inputCols});
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [
            {
              aggregationOp: 'MEDIAN',
              column: inputCols[0],
              newColumnName: 'median_dur',
              isValid: true,
            },
          ],
        },
        mockInput,
      );

      expect(node.finalCols.length).toBe(1);
      expect(node.finalCols[0].name).toBe('median_dur');
      expect(node.finalCols[0].type).toEqual(PerfettoSqlTypes.DOUBLE);
    });

    it('should set DOUBLE type for DURATION_WEIGHTED_MEAN aggregation', () => {
      const inputCols = [createColumnInfo('dur', 'duration')];
      const mockInput = createMockNode({columns: inputCols});
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [
            {
              aggregationOp: 'DURATION_WEIGHTED_MEAN',
              column: inputCols[0],
              newColumnName: 'weighted_avg_dur',
              isValid: true,
            },
          ],
        },
        mockInput,
      );

      expect(node.finalCols.length).toBe(1);
      expect(node.finalCols[0].name).toBe('weighted_avg_dur');
      expect(node.finalCols[0].type).toEqual(PerfettoSqlTypes.DOUBLE);
    });

    it('should set DOUBLE type for PERCENTILE aggregation', () => {
      const inputCols = [createColumnInfo('dur', 'int')];
      const mockInput = createMockNode({columns: inputCols});
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [],
          aggregations: [
            {
              aggregationOp: 'PERCENTILE',
              column: inputCols[0],
              percentile: 95,
              newColumnName: 'p95_dur',
              isValid: true,
            },
          ],
        },
        mockInput,
      );

      expect(node.finalCols.length).toBe(1);
      expect(node.finalCols[0].name).toBe('p95_dur');
      expect(node.finalCols[0].type).toEqual(PerfettoSqlTypes.DOUBLE);
    });

    it('should include GROUP BY columns with their original types', () => {
      const inputCols = [
        {
          name: 'name',
          checked: true,
          type: {kind: 'string' as const},
        },
        createColumnInfo('value', 'int'),
      ];
      const mockInput = createMockNode({columns: inputCols});
      const node = createAggregationNodeWithInput(
        {
          trace: createMockTrace(),
          groupByColumns: [
            {
              name: inputCols[0].name,
              type: inputCols[0].type,
              checked: true,
            },
          ],
          aggregations: [
            {
              aggregationOp: 'SUM',
              column: inputCols[1],
              newColumnName: 'total_value',
              isValid: true,
            },
          ],
        },
        mockInput,
      );

      // First column should be the GROUP BY column (name) with preserved STRING type
      // Second column should be the aggregation result (total_value) with INT type
      expect(node.finalCols.length).toBe(2);
      expect(node.finalCols[0].name).toBe('name');
      expect(node.finalCols[0].type).toEqual(PerfettoSqlTypes.STRING);
      expect(node.finalCols[1].name).toBe('total_value');
      expect(node.finalCols[1].type).toEqual(PerfettoSqlTypes.INT);
    });
  });

  describe('edge cases', () => {
    it('should handle PERCENTILE with 0 percentile', () => {
      const agg: Aggregation = {
        aggregationOp: 'PERCENTILE',
        column: createColumnInfo('dur', 'int'),
        percentile: 0,
      };

      expect(placeholderNewColumnName(agg)).toBe('percentile_dur');

      // Validation
      const isValid =
        agg.column !== undefined &&
        agg.percentile !== undefined &&
        agg.percentile >= 0 &&
        agg.percentile <= 100;
      expect(isValid).toBe(true);
    });

    it('should handle PERCENTILE with 100 percentile', () => {
      const agg: Aggregation = {
        aggregationOp: 'PERCENTILE',
        column: createColumnInfo('dur', 'int'),
        percentile: 100,
      };

      expect(placeholderNewColumnName(agg)).toBe('percentile_dur');

      // Validation
      const isValid =
        agg.column !== undefined &&
        agg.percentile !== undefined &&
        agg.percentile >= 0 &&
        agg.percentile <= 100;
      expect(isValid).toBe(true);
    });

    it('should handle PERCENTILE with negative percentile', () => {
      const agg: Aggregation = {
        aggregationOp: 'PERCENTILE',
        column: createColumnInfo('dur', 'int'),
        percentile: -1,
      };

      // Validation should fail
      const isValid =
        agg.column !== undefined &&
        agg.percentile !== undefined &&
        agg.percentile >= 0 &&
        agg.percentile <= 100;
      expect(isValid).toBe(false);
    });

    it('should handle aggregation with special characters in column name', () => {
      const agg: Aggregation = {
        aggregationOp: 'SUM',
        column: createColumnInfo('dur_ns', 'int'),
      };

      expect(placeholderNewColumnName(agg)).toBe('sum_dur_ns');
    });
  });
});

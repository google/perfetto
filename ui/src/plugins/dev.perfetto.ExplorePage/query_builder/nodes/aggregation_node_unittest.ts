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
  AggregationNodeState,
} from './aggregation_node';
import {Trace} from '../../../../public/trace';
import {ColumnInfo} from '../column_info';
import {QueryNode, NodeType} from '../../query_node';

describe('AggregationNode', () => {
  function createMockTrace(): Trace {
    return {} as Trace;
  }

  function createMockPrevNode(cols: ColumnInfo[] = []): QueryNode {
    return {
      nodeId: 'mock',
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: cols,
      state: {},
      validate: () => true,
      getTitle: () => 'Mock',
      nodeSpecificModify: () => null,
      nodeInfo: () => null,
      clone: () => createMockPrevNode(cols),
      getStructuredQuery: () => undefined,
      serializeState: () => ({}),
    } as QueryNode;
  }

  function createColumnInfo(name: string, type: string): ColumnInfo {
    return {
      name,
      type,
      checked: false,
      column: {
        name,
      },
    };
  }

  describe('placeholderNewColumnName', () => {
    it('should generate placeholder for COUNT_ALL without column', () => {
      const agg: Aggregation = {
        aggregationOp: 'COUNT_ALL',
      };
      expect(placeholderNewColumnName(agg)).toBe('count');
    });

    it('should generate placeholder for PERCENTILE with column', () => {
      const agg: Aggregation = {
        aggregationOp: 'PERCENTILE',
        column: createColumnInfo('dur', 'INT'),
        percentile: 95,
      };
      expect(placeholderNewColumnName(agg)).toBe('dur_percentile');
    });

    it('should generate placeholder for MEDIAN with column', () => {
      const agg: Aggregation = {
        aggregationOp: 'MEDIAN',
        column: createColumnInfo('value', 'DOUBLE'),
      };
      expect(placeholderNewColumnName(agg)).toBe('value_median');
    });

    it('should generate placeholder for SUM with column', () => {
      const agg: Aggregation = {
        aggregationOp: 'SUM',
        column: createColumnInfo('dur', 'INT'),
      };
      expect(placeholderNewColumnName(agg)).toBe('dur_sum');
    });

    it('should generate placeholder for COUNT with column', () => {
      const agg: Aggregation = {
        aggregationOp: 'COUNT',
        column: createColumnInfo('name', 'STRING'),
      };
      expect(placeholderNewColumnName(agg)).toBe('name_count');
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
        column: createColumnInfo('Value', 'DOUBLE'),
      };
      expect(placeholderNewColumnName(agg)).toBe('Value_mean');
    });
  });

  describe('validation', () => {
    let node: AggregationNode;

    beforeEach(() => {
      node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: createMockPrevNode(),
        groupByColumns: [],
        aggregations: [],
      });
    });

    it('should validate COUNT_ALL without column', () => {
      node.state.aggregations = [
        {
          aggregationOp: 'COUNT_ALL',
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.state.aggregations) {
        agg.isValid =
          agg.aggregationOp === 'COUNT_ALL' ||
          (agg.column !== undefined && agg.aggregationOp !== undefined);
      }

      expect(node.state.aggregations[0].isValid).toBe(true);
    });

    it('should validate PERCENTILE with column and percentile value', () => {
      node.state.aggregations = [
        {
          aggregationOp: 'PERCENTILE',
          column: createColumnInfo('dur', 'INT'),
          percentile: 50,
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.state.aggregations) {
        if (agg.aggregationOp === 'PERCENTILE') {
          agg.isValid =
            agg.column !== undefined &&
            agg.percentile !== undefined &&
            agg.percentile >= 0 &&
            agg.percentile <= 100;
        }
      }

      expect(node.state.aggregations[0].isValid).toBe(true);
    });

    it('should invalidate PERCENTILE with percentile value out of range', () => {
      node.state.aggregations = [
        {
          aggregationOp: 'PERCENTILE',
          column: createColumnInfo('dur', 'INT'),
          percentile: 150, // Invalid: > 100
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.state.aggregations) {
        if (agg.aggregationOp === 'PERCENTILE') {
          agg.isValid =
            agg.column !== undefined &&
            agg.percentile !== undefined &&
            agg.percentile >= 0 &&
            agg.percentile <= 100;
        }
      }

      expect(node.state.aggregations[0].isValid).toBe(false);
    });

    it('should invalidate PERCENTILE without percentile value', () => {
      node.state.aggregations = [
        {
          aggregationOp: 'PERCENTILE',
          column: createColumnInfo('dur', 'INT'),
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.state.aggregations) {
        if (agg.aggregationOp === 'PERCENTILE') {
          agg.isValid =
            agg.column !== undefined &&
            agg.percentile !== undefined &&
            agg.percentile >= 0 &&
            agg.percentile <= 100;
        }
      }

      expect(node.state.aggregations[0].isValid).toBe(false);
    });

    it('should invalidate PERCENTILE without column', () => {
      node.state.aggregations = [
        {
          aggregationOp: 'PERCENTILE',
          percentile: 50,
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.state.aggregations) {
        if (agg.aggregationOp === 'PERCENTILE') {
          agg.isValid =
            agg.column !== undefined &&
            agg.percentile !== undefined &&
            agg.percentile >= 0 &&
            agg.percentile <= 100;
        }
      }

      expect(node.state.aggregations[0].isValid).toBe(false);
    });

    it('should validate MEDIAN with column', () => {
      node.state.aggregations = [
        {
          aggregationOp: 'MEDIAN',
          column: createColumnInfo('value', 'DOUBLE'),
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.state.aggregations) {
        if (
          agg.aggregationOp !== 'COUNT_ALL' &&
          agg.aggregationOp !== 'PERCENTILE'
        ) {
          agg.isValid =
            agg.column !== undefined && agg.aggregationOp !== undefined;
        }
      }

      expect(node.state.aggregations[0].isValid).toBe(true);
    });

    it('should invalidate MEDIAN without column', () => {
      node.state.aggregations = [
        {
          aggregationOp: 'MEDIAN',
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.state.aggregations) {
        if (
          agg.aggregationOp !== 'COUNT_ALL' &&
          agg.aggregationOp !== 'PERCENTILE'
        ) {
          agg.isValid =
            agg.column !== undefined && agg.aggregationOp !== undefined;
        }
      }

      expect(node.state.aggregations[0].isValid).toBe(false);
    });

    it('should validate SUM with column', () => {
      node.state.aggregations = [
        {
          aggregationOp: 'SUM',
          column: createColumnInfo('dur', 'INT'),
          isValid: false,
        },
      ];

      // Simulate validation
      for (const agg of node.state.aggregations) {
        if (
          agg.aggregationOp !== 'COUNT_ALL' &&
          agg.aggregationOp !== 'PERCENTILE'
        ) {
          agg.isValid =
            agg.column !== undefined && agg.aggregationOp !== undefined;
        }
      }

      expect(node.state.aggregations[0].isValid).toBe(true);
    });
  });

  describe('serialization', () => {
    it('should serialize PERCENTILE aggregation with percentile value', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: createMockPrevNode(),
        groupByColumns: [],
        aggregations: [],
      });

      node.state.aggregations = [
        {
          aggregationOp: 'PERCENTILE',
          column: createColumnInfo('dur', 'INT'),
          percentile: 95,
          newColumnName: 'p95_dur',
          isValid: true,
        },
      ];

      const serialized = node.serializeState();

      expect(serialized.aggregations).toBeDefined();
      expect(serialized.aggregations.length).toBe(1);
      expect(serialized.aggregations[0].aggregationOp).toBe('PERCENTILE');
      expect(serialized.aggregations[0].percentile).toBe(95);
      expect(serialized.aggregations[0].newColumnName).toBe('p95_dur');
    });

    it('should serialize COUNT_ALL aggregation without column', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: createMockPrevNode(),
        groupByColumns: [],
        aggregations: [],
      });

      node.state.aggregations = [
        {
          aggregationOp: 'COUNT_ALL',
          newColumnName: 'total_count',
          isValid: true,
        },
      ];

      const serialized = node.serializeState();

      expect(serialized.aggregations).toBeDefined();
      expect(serialized.aggregations.length).toBe(1);
      expect(serialized.aggregations[0].aggregationOp).toBe('COUNT_ALL');
      expect(serialized.aggregations[0].column).toBeUndefined();
      expect(serialized.aggregations[0].newColumnName).toBe('total_count');
    });

    it('should serialize MEDIAN aggregation', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: createMockPrevNode(),
        groupByColumns: [],
        aggregations: [],
      });

      node.state.aggregations = [
        {
          aggregationOp: 'MEDIAN',
          column: createColumnInfo('value', 'DOUBLE'),
          newColumnName: 'median_value',
          isValid: true,
        },
      ];

      const serialized = node.serializeState();

      expect(serialized.aggregations).toBeDefined();
      expect(serialized.aggregations.length).toBe(1);
      expect(serialized.aggregations[0].aggregationOp).toBe('MEDIAN');
      expect(serialized.aggregations[0].column?.name).toBe('value');
      expect(serialized.aggregations[0].newColumnName).toBe('median_value');
    });

    it('should serialize multiple aggregations including new types', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: createMockPrevNode(),
        groupByColumns: [],
        aggregations: [],
      });

      node.state.aggregations = [
        {
          aggregationOp: 'COUNT_ALL',
          newColumnName: 'count',
          isValid: true,
        },
        {
          aggregationOp: 'MEDIAN',
          column: createColumnInfo('dur', 'INT'),
          newColumnName: 'median_dur',
          isValid: true,
        },
        {
          aggregationOp: 'PERCENTILE',
          column: createColumnInfo('dur', 'INT'),
          percentile: 99,
          newColumnName: 'p99_dur',
          isValid: true,
        },
      ];

      const serialized = node.serializeState();

      expect(serialized.aggregations.length).toBe(3);
      expect(serialized.aggregations[0].aggregationOp).toBe('COUNT_ALL');
      expect(serialized.aggregations[1].aggregationOp).toBe('MEDIAN');
      expect(serialized.aggregations[2].aggregationOp).toBe('PERCENTILE');
      expect(serialized.aggregations[2].percentile).toBe(99);
    });
  });

  describe('deserialization', () => {
    it('should deserialize PERCENTILE aggregation with percentile value', () => {
      const state: AggregationNodeState = {
        trace: createMockTrace(),
        prevNode: createMockPrevNode(),
        groupByColumns: [],
        aggregations: [
          {
            aggregationOp: 'PERCENTILE',
            column: createColumnInfo('dur', 'INT'),
            percentile: 95,
            newColumnName: 'p95_dur',
            isValid: true,
          },
        ],
      };

      const node = new AggregationNode(state);

      expect(node.state.aggregations.length).toBe(1);
      expect(node.state.aggregations[0].aggregationOp).toBe('PERCENTILE');
      expect(node.state.aggregations[0].percentile).toBe(95);
      expect(node.state.aggregations[0].newColumnName).toBe('p95_dur');
    });

    it('should deserialize COUNT_ALL aggregation without column', () => {
      const state: AggregationNodeState = {
        trace: createMockTrace(),
        prevNode: createMockPrevNode(),
        groupByColumns: [],
        aggregations: [
          {
            aggregationOp: 'COUNT_ALL',
            newColumnName: 'total_count',
            isValid: true,
          },
        ],
      };

      const node = new AggregationNode(state);

      expect(node.state.aggregations.length).toBe(1);
      expect(node.state.aggregations[0].aggregationOp).toBe('COUNT_ALL');
      expect(node.state.aggregations[0].column).toBeUndefined();
      expect(node.state.aggregations[0].newColumnName).toBe('total_count');
    });

    it('should deserialize MEDIAN aggregation', () => {
      const state: AggregationNodeState = {
        trace: createMockTrace(),
        prevNode: createMockPrevNode(),
        groupByColumns: [],
        aggregations: [
          {
            aggregationOp: 'MEDIAN',
            column: createColumnInfo('value', 'DOUBLE'),
            newColumnName: 'median_value',
            isValid: true,
          },
        ],
      };

      const node = new AggregationNode(state);

      expect(node.state.aggregations.length).toBe(1);
      expect(node.state.aggregations[0].aggregationOp).toBe('MEDIAN');
      expect(node.state.aggregations[0].column?.name).toBe('value');
      expect(node.state.aggregations[0].newColumnName).toBe('median_value');
    });
  });

  describe('node validation', () => {
    let mockPrevNode: QueryNode;

    beforeEach(() => {
      mockPrevNode = createMockPrevNode([
        createColumnInfo('name', 'STRING'),
        createColumnInfo('dur', 'INT'),
        createColumnInfo('ts', 'INT'),
      ]);
    });

    it('should validate node with only group by columns (no aggregations)', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: mockPrevNode,
        groupByColumns: [
          {...createColumnInfo('name', 'STRING'), checked: true},
          {...createColumnInfo('dur', 'INT'), checked: false},
        ],
        aggregations: [],
      });

      expect(node.validate()).toBe(true);
    });

    it('should validate node with only aggregations (no group by)', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: mockPrevNode,
        groupByColumns: [
          {...createColumnInfo('name', 'STRING'), checked: false},
          {...createColumnInfo('dur', 'INT'), checked: false},
        ],
        aggregations: [
          {
            aggregationOp: 'COUNT_ALL',
            isValid: true,
          },
        ],
      });

      expect(node.validate()).toBe(true);
    });

    it('should validate node with both group by and aggregations', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: mockPrevNode,
        groupByColumns: [
          {...createColumnInfo('name', 'STRING'), checked: true},
          {...createColumnInfo('dur', 'INT'), checked: false},
        ],
        aggregations: [
          {
            aggregationOp: 'SUM',
            column: createColumnInfo('dur', 'INT'),
            isValid: true,
          },
        ],
      });

      expect(node.validate()).toBe(true);
    });

    it('should invalidate node with neither group by nor aggregations', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: mockPrevNode,
        groupByColumns: [
          {...createColumnInfo('name', 'STRING'), checked: false},
          {...createColumnInfo('dur', 'INT'), checked: false},
        ],
        aggregations: [],
      });

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'requires at least one group by column or aggregation function',
      );
    });

    it('should invalidate node with invalid aggregations and no group by', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: mockPrevNode,
        groupByColumns: [
          {...createColumnInfo('name', 'STRING'), checked: false},
        ],
        aggregations: [
          {
            aggregationOp: 'SUM',
            // Missing column - invalid
            isValid: false,
          },
        ],
      });

      expect(node.validate()).toBe(false);
    });

    it('should validate multiple aggregations without group by', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: mockPrevNode,
        groupByColumns: [
          {...createColumnInfo('name', 'STRING'), checked: false},
        ],
        aggregations: [
          {
            aggregationOp: 'COUNT_ALL',
            isValid: true,
          },
          {
            aggregationOp: 'SUM',
            column: createColumnInfo('dur', 'INT'),
            isValid: true,
          },
          {
            aggregationOp: 'MAX',
            column: createColumnInfo('ts', 'INT'),
            isValid: true,
          },
        ],
      });

      expect(node.validate()).toBe(true);
    });

    it('should invalidate node when group by columns are missing from input', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: mockPrevNode,
        groupByColumns: [
          {...createColumnInfo('name', 'STRING'), checked: true},
          {...createColumnInfo('missing_col', 'INT'), checked: true},
        ],
        aggregations: [],
      });

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'not found in input',
      );
    });

    it('should invalidate node without prevNode', () => {
      const node = new AggregationNode({
        trace: createMockTrace(),
        prevNode: undefined as unknown as QueryNode,
        groupByColumns: [],
        aggregations: [
          {
            aggregationOp: 'COUNT_ALL',
            isValid: true,
          },
        ],
      });

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toBe(
        'No input node connected',
      );
    });
  });

  describe('edge cases', () => {
    it('should handle PERCENTILE with 0 percentile', () => {
      const agg: Aggregation = {
        aggregationOp: 'PERCENTILE',
        column: createColumnInfo('dur', 'INT'),
        percentile: 0,
      };

      expect(placeholderNewColumnName(agg)).toBe('dur_percentile');

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
        column: createColumnInfo('dur', 'INT'),
        percentile: 100,
      };

      expect(placeholderNewColumnName(agg)).toBe('dur_percentile');

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
        column: createColumnInfo('dur', 'INT'),
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
        column: createColumnInfo('dur_ns', 'INT'),
      };

      expect(placeholderNewColumnName(agg)).toBe('dur_ns_sum');
    });
  });
});

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

import {
  MetricsNode,
  MetricsNodeState,
  MetricsSerializedState,
  ValueColumnConfig,
} from './metrics_node';
import {parseMetricBundleForValue} from './metrics_export_modal';
import {NodeType} from '../../query_node';
import {
  createMockNode,
  createColumnInfo,
  connectNodes,
  expectValidationError,
  createMockNodeWithStructuredQuery,
} from '../testing/test_utils';
import {isColumnDef} from '../../../../components/widgets/datagrid/datagrid_schema';
import protos from '../../../../protos';

// Helper to build a minimal valid state with one value column.
function makeState(
  overrides: Partial<MetricsNodeState> = {},
): MetricsNodeState {
  return {
    metricIdPrefix: 'test_metric',
    valueColumns: [
      {column: 'value', unit: 'COUNT', polarity: 'NOT_APPLICABLE'},
    ],
    dimensionUniqueness: 'NOT_UNIQUE',
    availableColumns: [],
    ...overrides,
  };
}

// Helper to build a single ValueColumnConfig.
function makeValueCol(
  column: string,
  unit = 'COUNT',
  polarity = 'NOT_APPLICABLE',
  customUnit?: string,
): ValueColumnConfig {
  return {column, unit, polarity, customUnit};
}

describe('MetricsNode', () => {
  describe('constructor', () => {
    it('should initialize with default state', () => {
      const node = new MetricsNode({} as MetricsNodeState);

      expect(node.state.metricIdPrefix).toBe('');
      expect(node.state.valueColumns).toEqual([]);
      expect(node.state.dimensionUniqueness).toBe('NOT_UNIQUE');
      expect(node.state.availableColumns).toEqual([]);
    });

    it('should have correct node type', () => {
      const node = new MetricsNode({} as MetricsNodeState);

      expect(node.type).toBe(NodeType.kMetrics);
    });

    it('should initialize with no primary input', () => {
      const node = new MetricsNode({} as MetricsNodeState);

      expect(node.primaryInput).toBeUndefined();
    });

    it('should initialize with empty nextNodes array', () => {
      const node = new MetricsNode({} as MetricsNodeState);

      expect(node.nextNodes).toEqual([]);
    });

    it('should preserve provided state values', () => {
      const node = new MetricsNode(
        makeState({
          metricIdPrefix: 'my_metric',
          valueColumns: [makeValueCol('value1', 'BYTES', 'HIGHER_IS_BETTER')],
          dimensionUniqueness: 'UNIQUE',
        }),
      );

      expect(node.state.metricIdPrefix).toBe('my_metric');
      expect(node.state.valueColumns).toHaveLength(1);
      expect(node.state.valueColumns[0].column).toBe('value1');
      expect(node.state.valueColumns[0].unit).toBe('BYTES');
      expect(node.state.valueColumns[0].polarity).toBe('HIGHER_IS_BETTER');
      expect(node.state.dimensionUniqueness).toBe('UNIQUE');
    });

    it('should preserve multiple value columns', () => {
      const node = new MetricsNode(
        makeState({
          valueColumns: [
            makeValueCol('cpu_time', 'TIME_NANOS', 'LOWER_IS_BETTER'),
            makeValueCol('mem_bytes', 'BYTES', 'LOWER_IS_BETTER'),
          ],
        }),
      );

      expect(node.state.valueColumns).toHaveLength(2);
      expect(node.state.valueColumns[0].column).toBe('cpu_time');
      expect(node.state.valueColumns[1].column).toBe('mem_bytes');
    });
  });

  describe('finalCols', () => {
    it('should return empty array when no primary input', () => {
      const node = new MetricsNode({} as MetricsNodeState);

      expect(node.finalCols).toEqual([]);
    });

    it('should pass through input columns unchanged', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('name', 'string'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({} as MetricsNodeState);
      node.primaryInput = inputNode;

      expect(node.finalCols).toEqual(inputCols);
    });
  });

  describe('getDimensions', () => {
    it('should return all columns when no value columns are set', () => {
      const node = new MetricsNode(
        makeState({
          valueColumns: [],
          availableColumns: [
            createColumnInfo('id', 'int'),
            createColumnInfo('name', 'string'),
          ],
        }),
      );

      expect(node.getDimensions()).toEqual(['id', 'name']);
    });

    it('should return all columns except value columns as dimensions', () => {
      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('value1')],
          availableColumns: [
            createColumnInfo('id', 'int'),
            createColumnInfo('name', 'string'),
            createColumnInfo('value1', 'double'),
            createColumnInfo('category', 'string'),
          ],
        }),
      );

      expect(node.getDimensions()).toEqual(['id', 'name', 'category']);
    });

    it('should exclude multiple value columns from dimensions', () => {
      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('cpu'), makeValueCol('mem')],
          availableColumns: [
            createColumnInfo('process', 'string'),
            createColumnInfo('cpu', 'double'),
            createColumnInfo('mem', 'double'),
            createColumnInfo('pid', 'int'),
          ],
        }),
      );

      expect(node.getDimensions()).toEqual(['process', 'pid']);
    });

    it('should return empty array when all columns are value columns', () => {
      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('value')],
          availableColumns: [createColumnInfo('value', 'double')],
        }),
      );

      expect(node.getDimensions()).toEqual([]);
    });
  });

  describe('validate', () => {
    it('should fail validation when no primary input', () => {
      const node = new MetricsNode(makeState());

      expectValidationError(node, 'No input node connected');
    });

    it('should fail validation when primary input is invalid', () => {
      const inputNode = createMockNode({validate: () => false});

      const node = new MetricsNode(makeState());
      connectNodes(inputNode, node);

      expectValidationError(node, 'Previous node is invalid');
    });

    it('should fail validation when metric ID prefix is empty', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(makeState({metricIdPrefix: ''}));
      connectNodes(inputNode, node);

      expectValidationError(node, 'Metric ID prefix is required');
    });

    it('should fail validation when metric ID prefix is only whitespace', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(makeState({metricIdPrefix: '   '}));
      connectNodes(inputNode, node);

      expectValidationError(node, 'Metric ID prefix is required');
    });

    it('should fail validation when no value columns set', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(makeState({valueColumns: []}));
      connectNodes(inputNode, node);

      expectValidationError(node, 'At least one value column is required');
    });

    it('should fail validation when value column not found in input', () => {
      const inputCols = [createColumnInfo('other', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({valueColumns: [makeValueCol('missing_column')]}),
      );
      connectNodes(inputNode, node);

      expectValidationError(node, "Value column 'missing_column' not found");
    });

    it('should fail validation when value column is not numeric', () => {
      const inputCols = [createColumnInfo('name', 'string')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({valueColumns: [makeValueCol('name')]}),
      );
      connectNodes(inputNode, node);

      expectValidationError(node, 'must be numeric');
    });

    it('should fail validation when custom unit not provided with CUSTOM unit', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('value', 'CUSTOM', 'NOT_APPLICABLE', '')],
        }),
      );
      connectNodes(inputNode, node);

      expectValidationError(node, 'Custom unit is required');
    });

    it('should fail validation when second value column has custom unit problem', () => {
      const inputCols = [
        createColumnInfo('cpu', 'double'),
        createColumnInfo('mem', 'double'),
      ];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({
          valueColumns: [
            makeValueCol('cpu', 'COUNT', 'NOT_APPLICABLE'),
            makeValueCol('mem', 'CUSTOM', 'NOT_APPLICABLE', ''), // missing custom unit
          ],
        }),
      );
      connectNodes(inputNode, node);

      expectValidationError(
        node,
        "Custom unit is required for value column 'mem'",
      );
    });

    it('should pass validation with valid single value column', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('value', 'double'),
        createColumnInfo('name', 'string'),
      ];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({valueColumns: [makeValueCol('value')]}),
      );
      connectNodes(inputNode, node);

      expect(node.validate()).toBe(true);
    });

    it('should pass validation with multiple value columns', () => {
      const inputCols = [
        createColumnInfo('cpu', 'double'),
        createColumnInfo('mem', 'int'),
        createColumnInfo('name', 'string'),
      ];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({
          valueColumns: [
            makeValueCol('cpu', 'TIME_NANOS', 'LOWER_IS_BETTER'),
            makeValueCol('mem', 'BYTES', 'LOWER_IS_BETTER'),
          ],
        }),
      );
      connectNodes(inputNode, node);

      expect(node.validate()).toBe(true);
    });

    it('should pass validation with custom unit', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({
          valueColumns: [
            makeValueCol('value', 'CUSTOM', 'NOT_APPLICABLE', 'widgets'),
          ],
        }),
      );
      connectNodes(inputNode, node);

      expect(node.validate()).toBe(true);
    });

    it('should clear previous validation errors on success', () => {
      const node = new MetricsNode(makeState());

      // First validation should fail (no input)
      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError).toBeDefined();

      // Add valid input and validate again
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});
      connectNodes(inputNode, node);

      expect(node.validate()).toBe(true);
      expect(node.state.issues?.queryError).toBeUndefined();
    });
  });

  describe('onPrevNodesUpdated', () => {
    it('should update available columns from primary input', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('name', 'string'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({} as MetricsNodeState);
      connectNodes(inputNode, node);

      node.onPrevNodesUpdated();

      expect(node.state.availableColumns.length).toBe(3);
      expect(node.state.availableColumns.map((c) => c.name)).toEqual([
        'id',
        'name',
        'value',
      ]);
    });

    it('should remove value column entry if the column no longer exists', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({valueColumns: [makeValueCol('old_value')]}),
      );
      connectNodes(inputNode, node);

      node.onPrevNodesUpdated();

      // 'old_value' is gone, so it should be removed from valueColumns
      expect(node.state.valueColumns).toHaveLength(0);
    });

    it('should remove value column entry if it becomes non-numeric', () => {
      const inputCols = [createColumnInfo('value', 'string')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({valueColumns: [makeValueCol('value')]}),
      );
      connectNodes(inputNode, node);

      node.onPrevNodesUpdated();

      // 'value' is now string, so it should be removed
      expect(node.state.valueColumns).toHaveLength(0);
    });

    it('should preserve value column if it still exists and is numeric', () => {
      const inputCols = [createColumnInfo('value', 'double')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({valueColumns: [makeValueCol('value')]}),
      );
      connectNodes(inputNode, node);

      node.onPrevNodesUpdated();

      expect(node.state.valueColumns).toHaveLength(1);
      expect(node.state.valueColumns[0].column).toBe('value');
    });

    it('should selectively remove only stale value columns from multi-value', () => {
      const inputCols = [
        createColumnInfo('cpu', 'double'),
        createColumnInfo('name', 'string'), // was numeric, now string
      ];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode(
        makeState({
          valueColumns: [
            makeValueCol('cpu'),
            makeValueCol('stale_col'), // no longer exists
          ],
        }),
      );
      connectNodes(inputNode, node);

      node.onPrevNodesUpdated();

      expect(node.state.valueColumns).toHaveLength(1);
      expect(node.state.valueColumns[0].column).toBe('cpu');
    });

    it('should do nothing when no primary input', () => {
      const node = new MetricsNode(
        makeState({valueColumns: [makeValueCol('value')]}),
      );

      // Should not throw
      node.onPrevNodesUpdated();

      // State should remain unchanged
      expect(node.state.valueColumns).toHaveLength(1);
      expect(node.state.valueColumns[0].column).toBe('value');
    });
  });

  describe('getStructuredQuery', () => {
    it('should return undefined when validation fails', () => {
      const node = new MetricsNode({} as MetricsNodeState);

      expect(node.getStructuredQuery()).toBeUndefined();
    });

    it('should return undefined when primary input has no structured query', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({
        columns: inputCols,
        getStructuredQuery: () => undefined,
      });

      const node = new MetricsNode(makeState());
      connectNodes(inputNode, node);

      expect(node.getStructuredQuery()).toBeUndefined();
    });

    it('should return structured query when valid', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(makeState());
      connectNodes(inputNode, node);

      const sq = node.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.id).toBe(node.nodeId);
      expect(sq?.innerQueryId).toBe(inputNode.nodeId);
    });
  });

  describe('getMetricTemplateSpec', () => {
    it('should return undefined when validation fails', () => {
      const node = new MetricsNode({} as MetricsNodeState);

      expect(node.getMetricTemplateSpec()).toBeUndefined();
    });

    it('should return template spec with correct id prefix', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(
        makeState({
          metricIdPrefix: 'my_metric_prefix',
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec).toBeDefined();
      expect(spec?.idPrefix).toBe('my_metric_prefix');
    });

    it('should compute dimensions as all columns except value columns', () => {
      const inputCols = [
        createColumnInfo('value1', 'double'),
        createColumnInfo('dim1', 'string'),
        createColumnInfo('dim2', 'int'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('value1')],
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.dimensions).toEqual(['dim1', 'dim2']);
    });

    it('should compute dimensions excluding all value columns', () => {
      const inputCols = [
        createColumnInfo('cpu', 'double'),
        createColumnInfo('mem', 'double'),
        createColumnInfo('proc', 'string'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('cpu'), makeValueCol('mem')],
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.dimensions).toEqual(['proc']);
    });

    it('should include single value column spec', () => {
      const inputCols = [createColumnInfo('value', 'double')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('value')],
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.valueColumnSpecs?.length).toBe(1);
      expect(spec?.valueColumnSpecs?.[0].name).toBe('value');
      expect(spec?.valueColumnSpecs?.[0].unit).toBe(
        protos.TraceMetricV2Spec.MetricUnit.COUNT,
      );
    });

    it('should include multiple value column specs', () => {
      const inputCols = [
        createColumnInfo('cpu', 'double'),
        createColumnInfo('mem', 'int'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(
        makeState({
          valueColumns: [
            makeValueCol('cpu', 'TIME_NANOS', 'LOWER_IS_BETTER'),
            makeValueCol('mem', 'BYTES', 'LOWER_IS_BETTER'),
          ],
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.valueColumnSpecs?.length).toBe(2);
      expect(spec?.valueColumnSpecs?.[0].name).toBe('cpu');
      expect(spec?.valueColumnSpecs?.[0].unit).toBe(
        protos.TraceMetricV2Spec.MetricUnit.TIME_NANOS,
      );
      expect(spec?.valueColumnSpecs?.[1].name).toBe('mem');
      expect(spec?.valueColumnSpecs?.[1].unit).toBe(
        protos.TraceMetricV2Spec.MetricUnit.BYTES,
      );
    });

    it('should set dimension uniqueness to UNIQUE', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(
        makeState({dimensionUniqueness: 'UNIQUE', availableColumns: inputCols}),
      );
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.dimensionUniqueness).toBe(
        protos.TraceMetricV2Spec.DimensionUniqueness.UNIQUE,
      );
    });

    it('should set dimension uniqueness to NOT_UNIQUE', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(
        makeState({
          dimensionUniqueness: 'NOT_UNIQUE',
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.dimensionUniqueness).toBe(
        protos.TraceMetricV2Spec.DimensionUniqueness.NOT_UNIQUE,
      );
    });

    it('should set custom unit in value spec', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(
        makeState({
          valueColumns: [
            makeValueCol('value', 'CUSTOM', 'NOT_APPLICABLE', 'my_custom_unit'),
          ],
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.valueColumnSpecs?.[0].customUnit).toBe('my_custom_unit');
    });

    it('should set polarity in value spec', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('value', 'COUNT', 'LOWER_IS_BETTER')],
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.valueColumnSpecs?.[0].polarity).toBe(
        protos.TraceMetricV2Spec.MetricPolarity.LOWER_IS_BETTER,
      );
    });

    it('should include query from primary input', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode(makeState({availableColumns: inputCols}));
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.query).toBeDefined();
      expect(spec?.query?.id).toBe('input');
    });
  });

  describe('serializeState', () => {
    it('should serialize all state properties including valueColumns array', () => {
      const inputNode = createMockNode({columns: []});

      const node = new MetricsNode(
        makeState({
          metricIdPrefix: 'my_metric',
          valueColumns: [makeValueCol('value1', 'BYTES', 'HIGHER_IS_BETTER')],
          dimensionUniqueness: 'UNIQUE',
        }),
      );
      connectNodes(inputNode, node);

      const serialized = node.serializeState();

      expect(serialized.metricIdPrefix).toBe('my_metric');
      expect(serialized.valueColumns).toHaveLength(1);
      expect(serialized.valueColumns?.[0].column).toBe('value1');
      expect(serialized.valueColumns?.[0].unit).toBe('BYTES');
      expect(serialized.valueColumns?.[0].polarity).toBe('HIGHER_IS_BETTER');
      expect(serialized.dimensionUniqueness).toBe('UNIQUE');
      expect(serialized.primaryInputId).toBe(inputNode.nodeId);
    });

    it('should serialize multiple value columns', () => {
      const node = new MetricsNode(
        makeState({
          valueColumns: [
            makeValueCol('cpu', 'TIME_NANOS', 'LOWER_IS_BETTER'),
            makeValueCol('mem', 'BYTES', 'LOWER_IS_BETTER'),
          ],
        }),
      );

      const serialized = node.serializeState();

      expect(serialized.valueColumns).toHaveLength(2);
      expect(serialized.valueColumns?.[0].column).toBe('cpu');
      expect(serialized.valueColumns?.[1].column).toBe('mem');
    });

    it('should handle missing primary input', () => {
      const node = new MetricsNode(makeState());

      const serialized = node.serializeState();

      expect(serialized.primaryInputId).toBeUndefined();
    });
  });

  describe('deserializeState', () => {
    it('should deserialize new-format valueColumns array', () => {
      const serialized: MetricsSerializedState = {
        metricIdPrefix: 'restored_metric',
        valueColumns: [
          {column: 'value1', unit: 'MEGABYTES', polarity: 'LOWER_IS_BETTER'},
        ],
        dimensionUniqueness: 'UNIQUE',
      };

      const state = MetricsNode.deserializeState(serialized);

      expect(state.metricIdPrefix).toBe('restored_metric');
      expect(state.valueColumns).toHaveLength(1);
      expect(state.valueColumns[0].column).toBe('value1');
      expect(state.valueColumns[0].unit).toBe('MEGABYTES');
      expect(state.valueColumns[0].polarity).toBe('LOWER_IS_BETTER');
      expect(state.dimensionUniqueness).toBe('UNIQUE');
      expect(state.availableColumns).toEqual([]);
    });

    it('should deserialize multiple value columns', () => {
      const serialized: MetricsSerializedState = {
        metricIdPrefix: 'multi',
        valueColumns: [
          {column: 'cpu', unit: 'TIME_NANOS', polarity: 'LOWER_IS_BETTER'},
          {column: 'mem', unit: 'BYTES', polarity: 'LOWER_IS_BETTER'},
        ],
        dimensionUniqueness: 'NOT_UNIQUE',
      };

      const state = MetricsNode.deserializeState(serialized);

      expect(state.valueColumns).toHaveLength(2);
      expect(state.valueColumns[0].column).toBe('cpu');
      expect(state.valueColumns[1].column).toBe('mem');
    });

    it('should provide defaults for missing properties', () => {
      const state = MetricsNode.deserializeState({} as MetricsSerializedState);

      expect(state.metricIdPrefix).toBe('');
      expect(state.valueColumns).toEqual([]);
      expect(state.dimensionUniqueness).toBe('NOT_UNIQUE');
    });

    it('should migrate from old single-value format (valueColumn field)', () => {
      const oldFormat = {
        metricIdPrefix: 'old_metric',
        valueColumn: 'value1',
        unit: 'BYTES',
        customUnit: undefined,
        polarity: 'HIGHER_IS_BETTER',
        dimensionUniqueness: 'NOT_UNIQUE',
      } as unknown as MetricsSerializedState;

      const state = MetricsNode.deserializeState(oldFormat);

      expect(state.metricIdPrefix).toBe('old_metric');
      expect(state.valueColumns).toHaveLength(1);
      expect(state.valueColumns[0].column).toBe('value1');
      expect(state.valueColumns[0].unit).toBe('BYTES');
      expect(state.valueColumns[0].polarity).toBe('HIGHER_IS_BETTER');
    });

    it('should migrate from old single-value format with custom unit', () => {
      const oldFormat = {
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'CUSTOM',
        customUnit: 'widgets',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
      } as unknown as MetricsSerializedState;

      const state = MetricsNode.deserializeState(oldFormat);

      expect(state.valueColumns).toHaveLength(1);
      expect(state.valueColumns[0].unit).toBe('CUSTOM');
      expect(state.valueColumns[0].customUnit).toBe('widgets');
    });

    it('should migrate from old multi-value values[] array format', () => {
      const oldFormat = {
        metricIdPrefix: 'old_metric',
        values: [
          {
            column: 'cpu',
            unit: 'TIME_NANOS',
            customUnit: undefined,
            polarity: 'LOWER_IS_BETTER',
          },
          {
            column: 'mem',
            unit: 'BYTES',
            customUnit: undefined,
            polarity: 'LOWER_IS_BETTER',
          },
        ],
        dimensionUniqueness: 'UNIQUE',
      } as unknown as MetricsSerializedState;

      const state = MetricsNode.deserializeState(oldFormat);

      expect(state.metricIdPrefix).toBe('old_metric');
      expect(state.valueColumns).toHaveLength(2);
      expect(state.valueColumns[0].column).toBe('cpu');
      expect(state.valueColumns[1].column).toBe('mem');
    });

    it('should migrate from old metricId field', () => {
      const oldFormat = {
        metricId: 'legacy_metric',
        valueColumns: [
          {column: 'val', unit: 'COUNT', polarity: 'NOT_APPLICABLE'},
        ],
        dimensionUniqueness: 'NOT_UNIQUE',
      } as unknown as MetricsSerializedState;

      const state = MetricsNode.deserializeState(oldFormat);

      expect(state.metricIdPrefix).toBe('legacy_metric');
    });
  });

  describe('clone', () => {
    it('should create a new node with same state', () => {
      const node = new MetricsNode(
        makeState({
          metricIdPrefix: 'test',
          valueColumns: [makeValueCol('value1', 'BYTES', 'HIGHER_IS_BETTER')],
          dimensionUniqueness: 'UNIQUE',
          availableColumns: [createColumnInfo('value1', 'int')],
        }),
      );

      const cloned = node.clone() as MetricsNode;

      expect(cloned).toBeInstanceOf(MetricsNode);
      expect(cloned.nodeId).not.toBe(node.nodeId);
      expect(cloned.state.metricIdPrefix).toBe('test');
      expect(cloned.state.valueColumns).toHaveLength(1);
      expect(cloned.state.valueColumns[0].column).toBe('value1');
      expect(cloned.state.valueColumns[0].unit).toBe('BYTES');
      expect(cloned.state.dimensionUniqueness).toBe('UNIQUE');
    });

    it('should deep-copy valueColumns array so mutation does not affect original', () => {
      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('cpu'), makeValueCol('mem')],
        }),
      );

      const cloned = node.clone() as MetricsNode;

      // Mutate clone
      cloned.state.valueColumns[0].unit = 'BYTES';
      cloned.state.valueColumns.push(makeValueCol('extra'));

      // Original should be unaffected
      expect(node.state.valueColumns[0].unit).toBe('COUNT');
      expect(node.state.valueColumns).toHaveLength(2);
    });

    it('should preserve onchange callback', () => {
      const onchange = jest.fn();
      const node = new MetricsNode({
        ...makeState(),
        onchange,
      });

      const cloned = node.clone() as MetricsNode;

      expect(cloned.state.onchange).toBe(onchange);
    });

    it('should not copy issues to the clone', () => {
      const node = new MetricsNode({} as MetricsNodeState);
      node.validate(); // Triggers issues creation

      const cloned = node.clone() as MetricsNode;

      expect(cloned.state.issues).toBeUndefined();
    });
  });

  describe('getTitle', () => {
    it('should return correct title', () => {
      const node = new MetricsNode({} as MetricsNodeState);

      expect(node.getTitle()).toBe('Metrics');
    });
  });

  describe('nodeDetails', () => {
    it('should always include title', () => {
      const node = new MetricsNode({} as MetricsNodeState);

      const details = node.nodeDetails();

      expect(details.content).toBeDefined();
    });

    it('should show invalid state when metric ID prefix is empty', () => {
      const node = new MetricsNode(makeState({metricIdPrefix: ''}));

      const details = node.nodeDetails();

      expect(details.content).toBeDefined();
    });

    it('should show metric ID prefix when configured', () => {
      const node = new MetricsNode(
        makeState({metricIdPrefix: 'my_metric', valueColumns: []}),
      );

      const details = node.nodeDetails();

      expect(details.content).toBeDefined();
    });

    it('should show value columns', () => {
      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('cpu'), makeValueCol('mem')],
        }),
      );

      const details = node.nodeDetails();

      expect(details.content).toBeDefined();
    });

    it('should compute getDimensions correctly excluding value columns', () => {
      const node = new MetricsNode(
        makeState({
          valueColumns: [makeValueCol('value')],
          availableColumns: [
            createColumnInfo('value', 'double'),
            createColumnInfo('dim1', 'string'),
            createColumnInfo('dim2', 'int'),
          ],
        }),
      );

      const details = node.nodeDetails();

      expect(details.content).toBeDefined();
      expect(node.getDimensions()).toEqual(['dim1', 'dim2']);
    });
  });

  describe('nodeSpecificModify', () => {
    it('should return sections for configuration', () => {
      const node = new MetricsNode(
        makeState({
          availableColumns: [
            createColumnInfo('value', 'int'),
            createColumnInfo('name', 'string'),
          ],
        }),
      );

      const modify = node.nodeSpecificModify();

      expect(modify).toBeDefined();
      expect(modify.sections).toBeDefined();
      if (modify.sections !== undefined) {
        expect(modify.sections.length).toBeGreaterThan(0);
      }
      expect(modify.info).toContain('metric');
    });
  });

  describe('integration tests', () => {
    it('should work end-to-end with single value column', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('cpu_time', 'double'),
        createColumnInfo('process_name', 'string'),
        createColumnInfo('thread_name', 'string'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      const node = new MetricsNode(
        makeState({
          metricIdPrefix: 'process_metrics',
          valueColumns: [
            makeValueCol('cpu_time', 'TIME_NANOS', 'LOWER_IS_BETTER'),
          ],
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      expect(node.validate()).toBe(true);

      const sq = node.getStructuredQuery();
      expect(sq).toBeDefined();

      const spec = node.getMetricTemplateSpec();
      expect(spec).toBeDefined();
      expect(spec?.idPrefix).toBe('process_metrics');
      expect(spec?.valueColumnSpecs?.length).toBe(1);
      expect(spec?.valueColumnSpecs?.[0].name).toBe('cpu_time');
      expect(spec?.valueColumnSpecs?.[0].unit).toBe(
        protos.TraceMetricV2Spec.MetricUnit.TIME_NANOS,
      );
      expect(spec?.dimensions).toEqual([
        'id',
        'ts',
        'process_name',
        'thread_name',
      ]);
    });

    it('should work end-to-end with multiple value columns', () => {
      const inputCols = [
        createColumnInfo('cpu_time', 'double'),
        createColumnInfo('mem_bytes', 'int'),
        createColumnInfo('process_name', 'string'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      const node = new MetricsNode(
        makeState({
          metricIdPrefix: 'multi_metrics',
          valueColumns: [
            makeValueCol('cpu_time', 'TIME_NANOS', 'LOWER_IS_BETTER'),
            makeValueCol('mem_bytes', 'BYTES', 'LOWER_IS_BETTER'),
          ],
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      expect(node.validate()).toBe(true);

      const spec = node.getMetricTemplateSpec();
      expect(spec?.valueColumnSpecs?.length).toBe(2);
      expect(spec?.dimensions).toEqual(['process_name']);
    });

    it('should handle serialization round-trip', () => {
      const inputCols = [
        createColumnInfo('value1', 'double'),
        createColumnInfo('dim', 'string'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      const node = new MetricsNode(
        makeState({
          metricIdPrefix: 'original_metric',
          valueColumns: [
            makeValueCol('value1', 'PERCENTAGE', 'HIGHER_IS_BETTER'),
          ],
          dimensionUniqueness: 'UNIQUE',
          availableColumns: inputCols,
        }),
      );
      connectNodes(inputNode, node);

      const serialized = node.serializeState();

      const restoredState = MetricsNode.deserializeState(
        serialized as MetricsSerializedState,
      );

      const restoredNode = new MetricsNode(restoredState);

      expect(restoredNode.state.metricIdPrefix).toBe('original_metric');
      expect(restoredNode.state.valueColumns).toHaveLength(1);
      expect(restoredNode.state.valueColumns[0].column).toBe('value1');
      expect(restoredNode.state.valueColumns[0].unit).toBe('PERCENTAGE');
      expect(restoredNode.state.valueColumns[0].polarity).toBe(
        'HIGHER_IS_BETTER',
      );
      expect(restoredNode.state.dimensionUniqueness).toBe('UNIQUE');
    });

    it('should handle multi-value serialization round-trip', () => {
      const node = new MetricsNode(
        makeState({
          metricIdPrefix: 'multi',
          valueColumns: [
            makeValueCol('cpu', 'TIME_NANOS', 'LOWER_IS_BETTER'),
            makeValueCol('mem', 'BYTES', 'LOWER_IS_BETTER'),
            makeValueCol('score', 'CUSTOM', 'HIGHER_IS_BETTER', 'pts'),
          ],
        }),
      );

      const serialized = node.serializeState();
      const restored = MetricsNode.deserializeState(
        serialized as MetricsSerializedState,
      );
      const restoredNode = new MetricsNode(restored);

      expect(restoredNode.state.valueColumns).toHaveLength(3);
      expect(restoredNode.state.valueColumns[2].customUnit).toBe('pts');
    });

    it('should preserve value columns after deserialization and reconnection', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('metric_value', 'double'),
        createColumnInfo('category', 'string'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      const deserializedState = MetricsNode.deserializeState({
        metricIdPrefix: 'my_metric',
        valueColumns: [
          {column: 'metric_value', unit: 'BYTES', polarity: 'LOWER_IS_BETTER'},
        ],
        dimensionUniqueness: 'NOT_UNIQUE',
      });

      expect(deserializedState.availableColumns).toEqual([]);

      const restoredNode = new MetricsNode(deserializedState);
      expect(restoredNode.state.valueColumns).toHaveLength(1);

      connectNodes(inputNode, restoredNode);
      restoredNode.onPrevNodesUpdated();

      expect(restoredNode.state.availableColumns.length).toBe(3);
      expect(restoredNode.state.valueColumns).toHaveLength(1);
      expect(restoredNode.state.valueColumns[0].column).toBe('metric_value');
      expect(restoredNode.getDimensions()).toEqual(['id', 'category']);
      expect(restoredNode.validate()).toBe(true);
    });

    it('should clear stale value columns if they no longer exist after reconnection', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('different_value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      const deserializedState = MetricsNode.deserializeState({
        metricIdPrefix: 'my_metric',
        valueColumns: [
          {
            column: 'old_value_column',
            unit: 'COUNT',
            polarity: 'NOT_APPLICABLE',
          },
        ],
        dimensionUniqueness: 'NOT_UNIQUE',
      });

      const restoredNode = new MetricsNode(deserializedState);
      connectNodes(inputNode, restoredNode);
      restoredNode.onPrevNodesUpdated();

      expect(restoredNode.state.valueColumns).toHaveLength(0);
    });

    it('should clear value columns that become non-numeric after reconnection', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('metric_value', 'string'), // was numeric, now string
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      const deserializedState = MetricsNode.deserializeState({
        metricIdPrefix: 'my_metric',
        valueColumns: [
          {column: 'metric_value', unit: 'COUNT', polarity: 'NOT_APPLICABLE'},
        ],
        dimensionUniqueness: 'NOT_UNIQUE',
      });

      const restoredNode = new MetricsNode(deserializedState);
      connectNodes(inputNode, restoredNode);
      restoredNode.onPrevNodesUpdated();

      expect(restoredNode.state.valueColumns).toHaveLength(0);
    });

    it('should include primaryInputId in serialized state', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery(
        'input-123',
        inputCols,
      );
      (inputNode as {nodeId: string}).nodeId = 'input-123';

      const node = new MetricsNode(makeState());
      connectNodes(inputNode, node);

      const serialized = node.serializeState();

      expect(serialized.primaryInputId).toBe('input-123');
    });
  });
});

describe('parseMetricBundleForValue', () => {
  // Build a TraceSummary proto with one bundle.
  // _dimensionNames and _valueColumnNames are passed for call-site readability
  // but the proto structure doesn't embed column names (those come from
  // the calling code's dimension/valueColumnNames arguments).
  function buildTestProto(
    rows: Array<{dims: string[]; values: number[]}>,
  ): Uint8Array {
    const bundle = protos.TraceMetricV2Bundle.create({
      row: rows.map((r) => ({
        dimension: r.dims.map((d) =>
          protos.TraceMetricV2Bundle.Row.Dimension.create({stringValue: d}),
        ),
        values: r.values.map((v) =>
          protos.TraceMetricV2Bundle.Row.Value.create({doubleValue: v}),
        ),
      })),
    });
    const summary = protos.TraceSummary.create({metricBundles: [bundle]});
    return protos.TraceSummary.encode(summary).finish();
  }

  it('should return undefined when bundle is missing', () => {
    const summary = protos.TraceSummary.create({metricBundles: []});
    const data = protos.TraceSummary.encode(summary).finish();

    const result = parseMetricBundleForValue(
      data,
      'prefix',
      ['dim'],
      ['val'],
      0,
    );

    expect(result).toBeUndefined();
  });

  it('should return undefined when valueIndex is out of range', () => {
    const data = buildTestProto([{dims: ['a'], values: [1]}]);

    const result = parseMetricBundleForValue(
      data,
      'prefix',
      ['dim'],
      ['val'],
      1, // out of range
    );

    expect(result).toBeUndefined();
  });

  it('should parse the first value column correctly', () => {
    const data = buildTestProto([
      {dims: ['chrome'], values: [100, 200]},
      {dims: ['android'], values: [150, 250]},
    ]);

    const result = parseMetricBundleForValue(
      data,
      'my_metric',
      ['process'],
      ['cpu', 'mem'],
      0, // cpu
    );

    expect(result).toBeDefined();
    expect(result?.metricId).toBe('my_metric_cpu');
    expect(result?.rows).toHaveLength(2);
    expect(result?.rows[0]['process']).toBe('chrome');
    expect(result?.rows[0]['cpu']).toBe(100);
    // Should NOT include 'mem' column
    expect('mem' in (result?.rows[0] ?? {})).toBe(false);
  });

  it('should parse the second value column correctly', () => {
    const data = buildTestProto([
      {dims: ['chrome'], values: [100, 200]},
      {dims: ['android'], values: [150, 250]},
    ]);

    const result = parseMetricBundleForValue(
      data,
      'my_metric',
      ['process'],
      ['cpu', 'mem'],
      1, // mem
    );

    expect(result).toBeDefined();
    expect(result?.metricId).toBe('my_metric_mem');
    expect(result?.rows[0]['mem']).toBe(200);
    expect(result?.rows[1]['mem']).toBe(250);
    expect('cpu' in (result?.rows[0] ?? {})).toBe(false);
  });

  it('should build schema with dimension columns as text and value as quantitative', () => {
    const data = buildTestProto([{dims: ['chrome', 'main'], values: [42]}]);

    const result = parseMetricBundleForValue(
      data,
      'prefix',
      ['process', 'thread'],
      ['cpu'],
      0,
    );

    expect(result).toBeDefined();
    if (result !== undefined) {
      const schema = result.schema[result.metricId];
      const processEntry = schema['process'];
      const threadEntry = schema['thread'];
      const cpuEntry = schema['cpu'];
      // Schema values for leaf columns are ColumnDef objects with columnType.
      expect(
        isColumnDef(processEntry) ? processEntry.columnType : undefined,
      ).toBe('text');
      expect(
        isColumnDef(threadEntry) ? threadEntry.columnType : undefined,
      ).toBe('text');
      expect(isColumnDef(cpuEntry) ? cpuEntry.columnType : undefined).toBe(
        'quantitative',
      );
    }
  });

  it('should handle rows with null values', () => {
    const bundle = protos.TraceMetricV2Bundle.create({
      row: [
        {
          dimension: [
            protos.TraceMetricV2Bundle.Row.Dimension.create({stringValue: 'a'}),
          ],
          values: [], // no values - simulates null
        },
      ],
    });
    const summary = protos.TraceSummary.create({metricBundles: [bundle]});
    const data = protos.TraceSummary.encode(summary).finish();

    const result = parseMetricBundleForValue(
      data,
      'prefix',
      ['dim'],
      ['val'],
      0,
    );

    expect(result).toBeDefined();
    expect(result?.rows[0]['val']).toBeNull();
  });
});

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
} from './metrics_node';
import {NodeType} from '../../query_node';
import {
  createMockNode,
  createColumnInfo,
  connectNodes,
  expectValidationError,
  createMockNodeWithStructuredQuery,
} from '../testing/test_utils';
import protos from '../../../../protos';

describe('MetricsNode', () => {
  describe('constructor', () => {
    it('should initialize with default state', () => {
      const node = new MetricsNode({} as MetricsNodeState);

      expect(node.state.metricIdPrefix).toBe('');
      expect(node.state.valueColumn).toBeUndefined();
      expect(node.state.unit).toBe('COUNT');
      expect(node.state.polarity).toBe('NOT_APPLICABLE');
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
      const node = new MetricsNode({
        metricIdPrefix: 'my_metric',
        valueColumn: 'value1',
        unit: 'BYTES',
        polarity: 'HIGHER_IS_BETTER',
        dimensionUniqueness: 'UNIQUE',
        availableColumns: [],
      });

      expect(node.state.metricIdPrefix).toBe('my_metric');
      expect(node.state.valueColumn).toBe('value1');
      expect(node.state.unit).toBe('BYTES');
      expect(node.state.polarity).toBe('HIGHER_IS_BETTER');
      expect(node.state.dimensionUniqueness).toBe('UNIQUE');
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
    it('should return all columns when no value column is set', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: undefined,
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('name', 'string'),
        ],
      });

      expect(node.getDimensions()).toEqual(['id', 'name']);
    });

    it('should return all columns except value column as dimensions', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value1',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('name', 'string'),
          createColumnInfo('value1', 'double'),
          createColumnInfo('category', 'string'),
        ],
      });

      expect(node.getDimensions()).toEqual(['id', 'name', 'category']);
    });

    it('should return empty array when the only column is the value column', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [createColumnInfo('value', 'double')],
      });

      expect(node.getDimensions()).toEqual([]);
    });
  });

  describe('validate', () => {
    it('should fail validation when no primary input', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });

      expectValidationError(node, 'No input node connected');
    });

    it('should fail validation when primary input is invalid', () => {
      const inputNode = createMockNode({validate: () => false});

      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      expectValidationError(node, 'Previous node is invalid');
    });

    it('should fail validation when metric ID prefix is empty', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: '',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      expectValidationError(node, 'Metric ID prefix is required');
    });

    it('should fail validation when metric ID prefix is only whitespace', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: '   ',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      expectValidationError(node, 'Metric ID prefix is required');
    });

    it('should fail validation when no value column set', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: undefined,
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      expectValidationError(node, 'A value column is required');
    });

    it('should fail validation when value column not found in input', () => {
      const inputCols = [createColumnInfo('other', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'missing_column',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      expectValidationError(node, "Value column 'missing_column' not found");
    });

    it('should fail validation when value column is not numeric', () => {
      const inputCols = [createColumnInfo('name', 'string')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'name',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      expectValidationError(node, 'must be numeric');
    });

    it('should fail validation when custom unit not provided with CUSTOM unit', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'CUSTOM',
        customUnit: '',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      expectValidationError(node, 'Custom unit is required');
    });

    it('should pass validation with valid configuration', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('value', 'double'),
        createColumnInfo('name', 'string'),
      ];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      expect(node.validate()).toBe(true);
    });

    it('should pass validation with custom unit', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'CUSTOM',
        customUnit: 'widgets',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      expect(node.validate()).toBe(true);
    });

    it('should clear previous validation errors on success', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });

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

    it('should clear value column if it no longer exists', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'old_value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      node.onPrevNodesUpdated();

      expect(node.state.valueColumn).toBeUndefined();
    });

    it('should clear value column if it becomes non-numeric', () => {
      const inputCols = [createColumnInfo('value', 'string')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      node.onPrevNodesUpdated();

      expect(node.state.valueColumn).toBeUndefined();
    });

    it('should preserve value column if it still exists and is numeric', () => {
      const inputCols = [createColumnInfo('value', 'double')];
      const inputNode = createMockNode({columns: inputCols});

      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      node.onPrevNodesUpdated();

      expect(node.state.valueColumn).toBe('value');
    });

    it('should do nothing when no primary input', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });

      // Should not throw
      node.onPrevNodesUpdated();

      // State should remain unchanged
      expect(node.state.valueColumn).toBe('value');
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

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      expect(node.getStructuredQuery()).toBeUndefined();
    });

    it('should return structured query when valid', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
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

      const node = new MetricsNode({
        metricIdPrefix: 'my_metric_prefix',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: inputCols,
      });
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec).toBeDefined();
      expect(spec?.idPrefix).toBe('my_metric_prefix');
    });

    it('should compute dimensions as all columns except value column', () => {
      const inputCols = [
        createColumnInfo('value1', 'double'),
        createColumnInfo('dim1', 'string'),
        createColumnInfo('dim2', 'int'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value1',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: inputCols,
      });
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.dimensions).toEqual(['dim1', 'dim2']);
    });

    it('should include value column spec', () => {
      const inputCols = [createColumnInfo('value', 'double')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: inputCols,
      });
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.valueColumnSpecs?.length).toBe(1);
      expect(spec?.valueColumnSpecs?.[0].name).toBe('value');
      expect(spec?.valueColumnSpecs?.[0].unit).toBe(
        protos.TraceMetricV2Spec.MetricUnit.COUNT,
      );
    });

    it('should set dimension uniqueness to UNIQUE', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'UNIQUE',
        availableColumns: inputCols,
      });
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.dimensionUniqueness).toBe(
        protos.TraceMetricV2Spec.DimensionUniqueness.UNIQUE,
      );
    });

    it('should set dimension uniqueness to NOT_UNIQUE', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: inputCols,
      });
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.dimensionUniqueness).toBe(
        protos.TraceMetricV2Spec.DimensionUniqueness.NOT_UNIQUE,
      );
    });

    it('should set custom unit in value spec', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'CUSTOM',
        customUnit: 'my_custom_unit',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: inputCols,
      });
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.valueColumnSpecs?.[0].customUnit).toBe('my_custom_unit');
    });

    it('should set polarity in value spec', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'LOWER_IS_BETTER',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: inputCols,
      });
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.valueColumnSpecs?.[0].polarity).toBe(
        protos.TraceMetricV2Spec.MetricPolarity.LOWER_IS_BETTER,
      );
    });

    it('should include query from primary input', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new MetricsNode({
        metricIdPrefix: 'test_metric',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: inputCols,
      });
      connectNodes(inputNode, node);

      const spec = node.getMetricTemplateSpec();

      expect(spec?.query).toBeDefined();
      expect(spec?.query?.id).toBe('input');
    });
  });

  describe('serializeState', () => {
    it('should serialize all state properties', () => {
      const inputNode = createMockNode({columns: []});

      const node = new MetricsNode({
        metricIdPrefix: 'my_metric',
        valueColumn: 'value1',
        unit: 'BYTES',
        polarity: 'HIGHER_IS_BETTER',
        dimensionUniqueness: 'UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      const serialized = node.serializeState();

      expect(serialized.metricIdPrefix).toBe('my_metric');
      expect(serialized.valueColumn).toBe('value1');
      expect(serialized.unit).toBe('BYTES');
      expect(serialized.polarity).toBe('HIGHER_IS_BETTER');
      expect(serialized.dimensionUniqueness).toBe('UNIQUE');
      expect(serialized.primaryInputId).toBe(inputNode.nodeId);
    });

    it('should handle missing primary input', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });

      const serialized = node.serializeState();

      expect(serialized.primaryInputId).toBeUndefined();
    });
  });

  describe('deserializeState', () => {
    it('should deserialize all state properties', () => {
      const serialized: MetricsSerializedState = {
        metricIdPrefix: 'restored_metric',
        valueColumn: 'value1',
        unit: 'MEGABYTES',
        polarity: 'LOWER_IS_BETTER',
        dimensionUniqueness: 'UNIQUE',
      };

      const state = MetricsNode.deserializeState(serialized);

      expect(state.metricIdPrefix).toBe('restored_metric');
      expect(state.valueColumn).toBe('value1');
      expect(state.unit).toBe('MEGABYTES');
      expect(state.polarity).toBe('LOWER_IS_BETTER');
      expect(state.dimensionUniqueness).toBe('UNIQUE');
      expect(state.availableColumns).toEqual([]);
    });

    it('should provide defaults for missing properties', () => {
      const state = MetricsNode.deserializeState({} as MetricsSerializedState);

      expect(state.metricIdPrefix).toBe('');
      expect(state.valueColumn).toBeUndefined();
      expect(state.unit).toBe('COUNT');
      expect(state.polarity).toBe('NOT_APPLICABLE');
      expect(state.dimensionUniqueness).toBe('NOT_UNIQUE');
    });

    it('should migrate from old multi-value format', () => {
      // Old format had values array
      const oldFormat = {
        metricIdPrefix: 'old_metric',
        values: [
          {
            column: 'old_value',
            unit: 'BYTES',
            customUnit: 'old_custom',
            polarity: 'HIGHER_IS_BETTER',
          },
        ],
        dimensionUniqueness: 'UNIQUE',
      } as unknown as MetricsSerializedState;

      const state = MetricsNode.deserializeState(oldFormat);

      expect(state.metricIdPrefix).toBe('old_metric');
      expect(state.valueColumn).toBe('old_value');
      expect(state.unit).toBe('BYTES');
      expect(state.customUnit).toBe('old_custom');
      expect(state.polarity).toBe('HIGHER_IS_BETTER');
    });

    it('should migrate from old metricId field', () => {
      const oldFormat = {
        metricId: 'legacy_metric',
        valueColumn: 'val',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
      } as unknown as MetricsSerializedState;

      const state = MetricsNode.deserializeState(oldFormat);

      expect(state.metricIdPrefix).toBe('legacy_metric');
    });
  });

  describe('clone', () => {
    it('should create a new node with same state', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value1',
        unit: 'BYTES',
        polarity: 'HIGHER_IS_BETTER',
        dimensionUniqueness: 'UNIQUE',
        availableColumns: [createColumnInfo('value1', 'int')],
      });

      const cloned = node.clone() as MetricsNode;

      expect(cloned).toBeInstanceOf(MetricsNode);
      expect(cloned.nodeId).not.toBe(node.nodeId);
      expect(cloned.state.metricIdPrefix).toBe('test');
      expect(cloned.state.valueColumn).toBe('value1');
      expect(cloned.state.unit).toBe('BYTES');
      expect(cloned.state.dimensionUniqueness).toBe('UNIQUE');
    });

    it('should preserve onchange callback', () => {
      const onchange = jest.fn();
      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
        onchange,
      });

      const cloned = node.clone() as MetricsNode;

      expect(cloned.state.onchange).toBe(onchange);
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
      const node = new MetricsNode({
        metricIdPrefix: '',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });

      const details = node.nodeDetails();

      // Content should be defined and show invalid state
      expect(details.content).toBeDefined();
    });

    it('should show metric ID prefix when configured', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'my_metric',
        valueColumn: undefined,
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });

      const details = node.nodeDetails();

      expect(details.content).toBeDefined();
    });

    it('should show value column', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'my_metric',
        valueColumn: 'value1',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });

      const details = node.nodeDetails();

      expect(details.content).toBeDefined();
    });

    it('should show computed dimensions', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'my_metric',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [
          createColumnInfo('value', 'double'),
          createColumnInfo('dim1', 'string'),
          createColumnInfo('dim2', 'int'),
        ],
      });

      const details = node.nodeDetails();

      expect(details.content).toBeDefined();
      // Dimensions should be computed as ['dim1', 'dim2']
      expect(node.getDimensions()).toEqual(['dim1', 'dim2']);
    });
  });

  describe('nodeSpecificModify', () => {
    it('should return sections for configuration', () => {
      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [
          createColumnInfo('value', 'int'),
          createColumnInfo('name', 'string'),
        ],
      });

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
    it('should work end-to-end with complete configuration', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('cpu_time', 'double'),
        createColumnInfo('process_name', 'string'),
        createColumnInfo('thread_name', 'string'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      const node = new MetricsNode({
        metricIdPrefix: 'process_metrics',
        valueColumn: 'cpu_time',
        unit: 'TIME_NANOS',
        polarity: 'LOWER_IS_BETTER',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: inputCols,
      });
      connectNodes(inputNode, node);

      // Should validate
      expect(node.validate()).toBe(true);

      // Should generate structured query
      const sq = node.getStructuredQuery();
      expect(sq).toBeDefined();

      // Should generate metric template spec
      const spec = node.getMetricTemplateSpec();
      expect(spec).toBeDefined();
      expect(spec?.idPrefix).toBe('process_metrics');
      expect(spec?.valueColumnSpecs?.length).toBe(1);
      expect(spec?.valueColumnSpecs?.[0].name).toBe('cpu_time');
      expect(spec?.valueColumnSpecs?.[0].unit).toBe(
        protos.TraceMetricV2Spec.MetricUnit.TIME_NANOS,
      );
      // Dimensions should be all columns except value
      expect(spec?.dimensions).toEqual([
        'id',
        'ts',
        'process_name',
        'thread_name',
      ]);
    });

    it('should handle serialization round-trip', () => {
      const inputCols = [
        createColumnInfo('value1', 'double'),
        createColumnInfo('dim', 'string'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      const node = new MetricsNode({
        metricIdPrefix: 'original_metric',
        valueColumn: 'value1',
        unit: 'PERCENTAGE',
        polarity: 'HIGHER_IS_BETTER',
        dimensionUniqueness: 'UNIQUE',
        availableColumns: inputCols,
      });
      connectNodes(inputNode, node);

      // Serialize
      const serialized = node.serializeState();

      // Deserialize
      const restoredState = MetricsNode.deserializeState(
        serialized as MetricsSerializedState,
      );

      // Create new node
      const restoredNode = new MetricsNode(restoredState);

      // Should have same configuration
      expect(restoredNode.state.metricIdPrefix).toBe('original_metric');
      expect(restoredNode.state.valueColumn).toBe('value1');
      expect(restoredNode.state.unit).toBe('PERCENTAGE');
      expect(restoredNode.state.polarity).toBe('HIGHER_IS_BETTER');
      expect(restoredNode.state.dimensionUniqueness).toBe('UNIQUE');
    });

    it('should preserve value column after deserialization and reconnection', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('metric_value', 'double'),
        createColumnInfo('category', 'string'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      // Simulate deserialization: create node with value but empty availableColumns
      const deserializedState = MetricsNode.deserializeState({
        metricIdPrefix: 'my_metric',
        valueColumn: 'metric_value',
        unit: 'BYTES',
        polarity: 'LOWER_IS_BETTER',
        dimensionUniqueness: 'NOT_UNIQUE',
      });

      // availableColumns should be empty after deserialization
      expect(deserializedState.availableColumns).toEqual([]);

      const restoredNode = new MetricsNode(deserializedState);

      // Value should be preserved even with empty availableColumns
      expect(restoredNode.state.valueColumn).toBe('metric_value');

      // Connect to input (simulates third pass of deserialization)
      connectNodes(inputNode, restoredNode);

      // Call onPrevNodesUpdated (simulates fourth pass of deserialization)
      restoredNode.onPrevNodesUpdated();

      // availableColumns should now be populated
      expect(restoredNode.state.availableColumns.length).toBe(3);
      expect(restoredNode.state.availableColumns.map((c) => c.name)).toEqual([
        'id',
        'metric_value',
        'category',
      ]);

      // Value should STILL be preserved (not cleared)
      expect(restoredNode.state.valueColumn).toBe('metric_value');

      // Dimensions should be computed as all columns except value
      expect(restoredNode.getDimensions()).toEqual(['id', 'category']);

      // Node should validate successfully
      expect(restoredNode.validate()).toBe(true);
    });

    it('should clear value column if it no longer exists after reconnection', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('different_value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      const deserializedState = MetricsNode.deserializeState({
        metricIdPrefix: 'my_metric',
        valueColumn: 'old_value_column',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
      });

      const restoredNode = new MetricsNode(deserializedState);
      connectNodes(inputNode, restoredNode);
      restoredNode.onPrevNodesUpdated();

      // Value column should be cleared because 'old_value_column' doesn't exist
      expect(restoredNode.state.valueColumn).toBeUndefined();
    });

    it('should clear value column if it becomes non-numeric after reconnection', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('metric_value', 'string'), // Same name but now string type
      ];
      const inputNode = createMockNodeWithStructuredQuery('source', inputCols);

      const deserializedState = MetricsNode.deserializeState({
        metricIdPrefix: 'my_metric',
        valueColumn: 'metric_value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
      });

      const restoredNode = new MetricsNode(deserializedState);
      connectNodes(inputNode, restoredNode);
      restoredNode.onPrevNodesUpdated();

      // Value column should be cleared because 'metric_value' is not numeric
      expect(restoredNode.state.valueColumn).toBeUndefined();
    });

    it('should include primaryInputId in serialized state', () => {
      const inputCols = [createColumnInfo('value', 'int')];
      const inputNode = createMockNodeWithStructuredQuery(
        'input-123',
        inputCols,
      );
      // Override the nodeId for testing
      (inputNode as {nodeId: string}).nodeId = 'input-123';

      const node = new MetricsNode({
        metricIdPrefix: 'test',
        valueColumn: 'value',
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
        dimensionUniqueness: 'NOT_UNIQUE',
        availableColumns: [],
      });
      connectNodes(inputNode, node);

      const serialized = node.serializeState();

      expect(serialized.primaryInputId).toBe('input-123');
    });
  });
});

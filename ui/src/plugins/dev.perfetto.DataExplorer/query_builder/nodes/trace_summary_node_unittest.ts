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

import {TraceSummaryNode} from './trace_summary_node';
import {MetricsNode, MetricsNodeAttrs} from './metrics_node';
import {NodeType} from '../../query_node';
import {
  createMockNode,
  createColumnInfo,
  connectNodes,
  connectSecondary,
  expectValidationError,
  expectValidationSuccess,
  createMockNodeWithStructuredQuery,
} from '../testing/test_utils';

function makeMetricsNode(
  overrides: Partial<MetricsNodeAttrs> = {},
): MetricsNode {
  const attrs: MetricsNodeAttrs = {
    metricIdPrefix: 'test_metric',
    valueColumns: [
      {column: 'value', unit: 'COUNT', polarity: 'NOT_APPLICABLE'},
    ],
    dimensionConfigs: {},
    dimensionUniqueness: 'NOT_UNIQUE',
    ...overrides,
  };
  const node = new MetricsNode(attrs, {});
  node.availableColumns = [
    createColumnInfo('name', 'string'),
    createColumnInfo('value', 'int'),
  ];
  return node;
}

function makeConnectedMetricsNode(prefix = 'test_metric'): {
  source: ReturnType<typeof createMockNodeWithStructuredQuery>;
  metrics: MetricsNode;
} {
  const source = createMockNodeWithStructuredQuery('source', [
    createColumnInfo('name', 'string'),
    createColumnInfo('value', 'int'),
  ]);
  const metrics = makeMetricsNode({metricIdPrefix: prefix});
  connectNodes(source, metrics);
  metrics.onPrevNodesUpdated();
  return {source, metrics};
}

describe('TraceSummaryNode', () => {
  describe('basic properties', () => {
    test('type is kTraceSummary', () => {
      const node = new TraceSummaryNode({}, {});
      expect(node.type).toBe(NodeType.kTraceSummary);
    });

    test('has unique nodeId', () => {
      const node1 = new TraceSummaryNode({}, {});
      const node2 = new TraceSummaryNode({}, {});
      expect(node1.nodeId).not.toBe(node2.nodeId);
    });

    test('finalCols is always empty', () => {
      const node = new TraceSummaryNode({}, {});
      expect(node.finalCols).toEqual([]);
    });

    test('getTitle returns Trace Summary', () => {
      const node = new TraceSummaryNode({}, {});
      expect(node.getTitle()).toBe('Trace Summary');
    });

    test('nextNodes is empty', () => {
      const node = new TraceSummaryNode({}, {});
      expect(node.nextNodes).toEqual([]);
    });

    test('getStructuredQuery returns undefined', () => {
      const node = new TraceSummaryNode({}, {});
      expect(node.getStructuredQuery()).toBeUndefined();
    });

    test('secondaryInputs requires min 1', () => {
      const node = new TraceSummaryNode({}, {});
      expect(node.secondaryInputs.min).toBe(1);
      expect(node.secondaryInputs.max).toBe('unbounded');
    });
  });

  describe('validation', () => {
    test('fails with no inputs', () => {
      const node = new TraceSummaryNode({}, {});
      expectValidationError(node, 'At least one Metrics node is required');
    });

    test('fails when input is not a Metrics node', () => {
      const nonMetrics = createMockNode({nodeId: 'non-metrics'});
      const node = new TraceSummaryNode({}, {});
      connectSecondary(nonMetrics, node, 0);
      expectValidationError(node, 'All inputs must be Metrics nodes');
    });

    test('fails when one of multiple inputs is not a Metrics node', () => {
      const {metrics} = makeConnectedMetricsNode();
      const nonMetrics = createMockNode({nodeId: 'non-metrics'});
      const node = new TraceSummaryNode({}, {});
      connectSecondary(metrics, node, 0);
      connectSecondary(nonMetrics, node, 1);
      expectValidationError(node, 'All inputs must be Metrics nodes');
    });

    test('fails when connected Metrics node is invalid', () => {
      const metrics = makeMetricsNode({
        metricIdPrefix: '',
        valueColumns: [],
      });
      const source = createMockNodeWithStructuredQuery('source', [
        createColumnInfo('name', 'string'),
        createColumnInfo('value', 'int'),
      ]);
      connectNodes(source, metrics);
      metrics.onPrevNodesUpdated();

      const node = new TraceSummaryNode({}, {});
      connectSecondary(metrics, node, 0);

      expect(node.validate()).toBe(false);
    });

    test('succeeds with one valid Metrics node', () => {
      const {metrics} = makeConnectedMetricsNode();
      const node = new TraceSummaryNode({}, {});
      connectSecondary(metrics, node, 0);
      expectValidationSuccess(node);
    });

    test('succeeds with multiple Metrics nodes', () => {
      const {metrics: metrics1} = makeConnectedMetricsNode('metric_a');
      const {metrics: metrics2} = makeConnectedMetricsNode('metric_b');
      const node = new TraceSummaryNode({}, {});
      connectSecondary(metrics1, node, 0);
      connectSecondary(metrics2, node, 1);
      expectValidationSuccess(node);
    });
  });

  describe('getTraceSummarySpec', () => {
    test('returns undefined when invalid', () => {
      const node = new TraceSummaryNode({}, {});
      expect(node.getTraceSummarySpec()).toBeUndefined();
    });

    test('bundles single Metrics node', () => {
      const {metrics} = makeConnectedMetricsNode('my_metric');
      const node = new TraceSummaryNode({}, {});
      connectSecondary(metrics, node, 0);

      const spec = node.getTraceSummarySpec();
      expect(spec).toBeDefined();
      expect(spec?.metricTemplateSpec).toHaveLength(1);
      expect(spec?.metricTemplateSpec?.[0].idPrefix).toBe('my_metric');
    });

    test('bundles multiple Metrics nodes', () => {
      const {metrics: metrics1} = makeConnectedMetricsNode('cpu_metric');
      const {metrics: metrics2} = makeConnectedMetricsNode('mem_metric');
      const node = new TraceSummaryNode({}, {});
      connectSecondary(metrics1, node, 0);
      connectSecondary(metrics2, node, 1);

      const spec = node.getTraceSummarySpec();
      expect(spec).toBeDefined();
      expect(spec?.metricTemplateSpec).toHaveLength(2);
      const prefixes = spec?.metricTemplateSpec?.map((s) => s.idPrefix);
      expect(prefixes).toContain('cpu_metric');
      expect(prefixes).toContain('mem_metric');
    });

    test('preserves port order in the spec', () => {
      const {metrics: metrics1} = makeConnectedMetricsNode('first_metric');
      const {metrics: metrics2} = makeConnectedMetricsNode('second_metric');
      const node = new TraceSummaryNode({}, {});
      connectSecondary(metrics1, node, 0);
      connectSecondary(metrics2, node, 1);

      const spec = node.getTraceSummarySpec();
      expect(spec?.metricTemplateSpec?.[0].idPrefix).toBe('first_metric');
      expect(spec?.metricTemplateSpec?.[1].idPrefix).toBe('second_metric');
    });
  });

  describe('serialization', () => {
    test('serializeState returns empty object', () => {
      const node = new TraceSummaryNode({}, {});
      const serialized = node.attrs;
      expect(serialized).toEqual({});
    });

    test('constructor accepts empty attrs', () => {
      const node = new TraceSummaryNode({}, {});
      expect(node.attrs).toEqual({});
    });
  });

  describe('clone', () => {
    test('creates a new node with different ID', () => {
      const node = new TraceSummaryNode({}, {});
      const cloned = node.clone();
      expect(cloned.nodeId).not.toBe(node.nodeId);
      expect(cloned.type).toBe(NodeType.kTraceSummary);
    });
  });

  describe('nodeDetails', () => {
    test('shows message when no metrics connected', () => {
      const node = new TraceSummaryNode({}, {});
      const details = node.nodeDetails();
      expect(details.content).toBeDefined();
    });

    test('shows connected metrics count', () => {
      const {metrics: metrics1} = makeConnectedMetricsNode('cpu');
      const {metrics: metrics2} = makeConnectedMetricsNode('mem');
      const node = new TraceSummaryNode({}, {});
      connectSecondary(metrics1, node, 0);
      connectSecondary(metrics2, node, 1);

      const details = node.nodeDetails();
      expect(details.content).toBeDefined();
    });
  });

  describe('nodeSpecificModify', () => {
    test('returns NodeModifyAttrs with info', () => {
      const node = new TraceSummaryNode({}, {});
      const result = node.nodeSpecificModify();
      expect(result).toHaveProperty('info');
      expect(result).toHaveProperty('sections');
    });
  });
});

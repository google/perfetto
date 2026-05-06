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

import {CounterToIntervalsNode} from './counter_to_intervals_node';
import {NodeType} from '../../query_node';
import {
  createMockNodeWithStructuredQuery,
  createColumnInfo,
  expectValidationSuccess,
} from '../testing/test_utils';

describe('CounterToIntervalsNode', () => {
  describe('constructor', () => {
    it('should initialize with empty state', () => {
      const node = new CounterToIntervalsNode({});

      expect(node.attrs).toBeDefined();
    });

    it('should have correct node type', () => {
      const node = new CounterToIntervalsNode({});

      expect(node.type).toBe(NodeType.kCounterToIntervals);
    });

    it('should initialize with no primary input', () => {
      const node = new CounterToIntervalsNode({});

      expect(node.primaryInput).toBeUndefined();
    });

    it('should initialize with empty nextNodes array', () => {
      const node = new CounterToIntervalsNode({});

      expect(node.nextNodes).toEqual([]);
    });
  });

  describe('finalCols', () => {
    it('should return empty array when no primary input', () => {
      const node = new CounterToIntervalsNode({});

      expect(node.finalCols).toEqual([]);
    });

    it('should include all input columns plus dur, next_value, delta_value', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      const finalCols = node.finalCols;
      expect(finalCols.length).toBe(7); // 4 input + 3 new columns

      // Check that all input columns are present
      expect(finalCols.map((c) => c.name)).toContain('id');
      expect(finalCols.map((c) => c.name)).toContain('ts');
      expect(finalCols.map((c) => c.name)).toContain('track_id');
      expect(finalCols.map((c) => c.name)).toContain('value');

      // Check that new columns are added
      expect(finalCols.map((c) => c.name)).toContain('dur');
      expect(finalCols.map((c) => c.name)).toContain('next_value');
      expect(finalCols.map((c) => c.name)).toContain('delta_value');
    });

    it('should set correct types for new columns', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      const finalCols = node.finalCols;
      const durCol = finalCols.find((c) => c.name === 'dur');
      const nextValueCol = finalCols.find((c) => c.name === 'next_value');
      const deltaValueCol = finalCols.find((c) => c.name === 'delta_value');

      expect(durCol?.type).toEqual({kind: 'duration'});
      expect(nextValueCol?.type).toEqual({kind: 'double'});
      expect(deltaValueCol?.type).toEqual({kind: 'double'});
    });

    it('should preserve input column order', () => {
      const inputCols = [
        createColumnInfo('value', 'double'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      const finalCols = node.finalCols;
      // First 4 columns should be input columns in original order
      expect(finalCols.slice(0, 4).map((c) => c.name)).toEqual([
        'value',
        'track_id',
        'id',
        'ts',
      ]);
    });
  });

  describe('validate', () => {
    it('should fail validation when no primary input', () => {
      const node = new CounterToIntervalsNode({});

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'No input node connected',
      );
    });

    it('should fail validation when primary input is invalid', () => {
      const inputNode = createMockNodeWithStructuredQuery('input', []);
      inputNode.validate = () => false;

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'Previous node is invalid',
      );
    });

    it('should fail validation when input missing id column', () => {
      const inputCols = [
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'Input must have id, ts, track_id, and value columns',
      );
    });

    it('should fail validation when input missing ts column', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'Input must have id, ts, track_id, and value columns',
      );
    });

    it('should fail validation when input missing track_id column', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'Input must have id, ts, track_id, and value columns',
      );
    });

    it('should fail validation when input missing value column', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'Input must have id, ts, track_id, and value columns',
      );
    });

    it('should fail validation when input already has dur column', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('dur', 'duration'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'Input already has dur column',
      );
    });

    it('should fail validation when input has no columns', () => {
      const inputNode = createMockNodeWithStructuredQuery('input', []);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'Input has no columns',
      );
    });

    it('should pass validation when all requirements met', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      expectValidationSuccess(node);
    });

    it('should pass validation with extra columns', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
        createColumnInfo('name', 'string'),
        createColumnInfo('cpu', 'int'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      expectValidationSuccess(node);
    });

    it('should clear previous validation errors on success', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      // First validation should fail
      expect(node.validate()).toBe(false);

      // Add input and validate again
      node.primaryInput = inputNode;
      expectValidationSuccess(node);

      // Issues should be cleared
      expect(node.context.issues?.queryError).toBeUndefined();
    });
  });

  describe('getStructuredQuery', () => {
    it('should return undefined when validation fails', () => {
      const node = new CounterToIntervalsNode({});

      expect(node.getStructuredQuery()).toBeUndefined();
    });

    it('should return structured query when valid', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      const sq = node.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.id).toBe(node.nodeId);
    });

    it('should create query using experimentalCounterIntervals', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      const sq = node.getStructuredQuery();

      expect(sq?.experimentalCounterIntervals).toBeDefined();
      expect(sq?.experimentalCounterIntervals?.inputQuery).toBeDefined();
    });

    it('should use input node query as inputQuery', () => {
      const inputCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const inputNode = createMockNodeWithStructuredQuery('input', inputCols);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      const sq = node.getStructuredQuery();
      const inputQuery = sq?.experimentalCounterIntervals?.inputQuery;

      // Should reference the input node's query via innerQueryId
      expect(inputQuery?.innerQueryId).toBe(inputNode.nodeId);
    });
  });

  describe('serializeState', () => {
    it('should serialize state correctly', () => {
      const inputNode = createMockNodeWithStructuredQuery('input', [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ]);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      const serialized = node.attrs;

      expect(serialized).toEqual({});
    });

    it('should handle missing input gracefully', () => {
      const node = new CounterToIntervalsNode({});

      const serialized = node.attrs;

      expect(serialized).toEqual({});
    });
  });

  describe('clone', () => {
    it('should create a new node with same state', () => {
      const node = new CounterToIntervalsNode({});

      const cloned = node.clone() as CounterToIntervalsNode;

      expect(cloned).toBeInstanceOf(CounterToIntervalsNode);
      expect(cloned.nodeId).not.toBe(node.nodeId); // Should have different ID
    });

    it('should preserve onchange callback', () => {
      const onchange = jest.fn();
      const node = new CounterToIntervalsNode({}, {onchange});

      const cloned = node.clone() as CounterToIntervalsNode;

      expect(cloned.context.onchange).toBe(onchange);
    });
  });

  describe('getTitle', () => {
    it('should return correct title', () => {
      const node = new CounterToIntervalsNode({});

      expect(node.getTitle()).toBe('Counter to Intervals');
    });
  });

  describe('onPrevNodesUpdated', () => {
    it('should trigger onchange callback when called', () => {
      const onchange = jest.fn();
      const node = new CounterToIntervalsNode({}, {onchange});

      node.onPrevNodesUpdated();

      expect(onchange).toHaveBeenCalled();
    });

    it('should not throw when onchange is not defined', () => {
      const node = new CounterToIntervalsNode({});

      expect(() => node.onPrevNodesUpdated()).not.toThrow();
    });
  });

  describe('nodeDetails', () => {
    it('should return content', () => {
      const node = new CounterToIntervalsNode({});

      const details = node.nodeDetails();

      expect(details.content).toBeDefined();
    });
  });

  describe('integration tests', () => {
    it('should work end-to-end with counter data', () => {
      const counterCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
        createColumnInfo('name', 'string'),
      ];
      const counterNode = createMockNodeWithStructuredQuery(
        'counter',
        counterCols,
      );

      const node = new CounterToIntervalsNode({});
      node.primaryInput = counterNode;

      // Should validate successfully
      expectValidationSuccess(node);

      // Should produce output columns with dur, next_value, delta_value
      const finalCols = node.finalCols;
      expect(finalCols.length).toBe(8); // 5 input + 3 new
      expect(finalCols.map((c) => c.name)).toContain('dur');
      expect(finalCols.map((c) => c.name)).toContain('next_value');
      expect(finalCols.map((c) => c.name)).toContain('delta_value');

      // Should generate valid structured query
      const sq = node.getStructuredQuery();
      expect(sq).toBeDefined();
      expect(sq?.experimentalCounterIntervals).toBeDefined();
    });

    it('should reject interval data (data with dur)', () => {
      const intervalCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('dur', 'duration'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ];
      const intervalNode = createMockNodeWithStructuredQuery(
        'interval',
        intervalCols,
      );

      const node = new CounterToIntervalsNode({});
      node.primaryInput = intervalNode;

      // Should fail validation because it already has dur
      expect(node.validate()).toBe(false);
      expect(node.context.issues?.queryError?.message).toContain(
        'already interval data',
      );
    });

    it('should handle serialization round-trip', () => {
      const inputNode = createMockNodeWithStructuredQuery('input', [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'timestamp'),
        createColumnInfo('track_id', 'int'),
        createColumnInfo('value', 'double'),
      ]);

      const node = new CounterToIntervalsNode({});
      node.primaryInput = inputNode;

      // Serialize
      const serialized = node.attrs;

      // Create new node with restored attrs
      const restoredNode = new CounterToIntervalsNode(serialized);

      // Should have same structure (but no input node since that's reconnected separately)
      expect(restoredNode.type).toBe(node.type);
      expect(restoredNode.getTitle()).toBe(node.getTitle());
    });
  });
});

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
  TimeRangeSourceNode,
  TimeRangeSourceState,
  TimeRangeSourceSerializedState,
} from './timerange_source';
import {Trace} from '../../../../../public/trace';
import {Time, TimeSpan} from '../../../../../base/time';

describe('TimeRangeSourceNode', () => {
  function createMockTrace(): Trace {
    return {
      traceInfo: {
        start: Time.fromRaw(0n),
        end: Time.fromRaw(1000000n),
      },
      selection: {
        getTimeSpanOfSelection: () => undefined,
      },
    } as unknown as Trace;
  }

  describe('constructor', () => {
    it('should create node with start and end', () => {
      const state: TimeRangeSourceState = {
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      };

      const node = new TimeRangeSourceNode(state);

      expect(node.state.start).toEqual(Time.fromRaw(100n));
      expect(node.state.end).toEqual(Time.fromRaw(500n));
      expect(node.state.isDynamic).toBe(false);
    });

    it('should create node with undefined start/end', () => {
      const state: TimeRangeSourceState = {
        trace: createMockTrace(),
        isDynamic: false,
      };

      const node = new TimeRangeSourceNode(state);

      expect(node.state.start).toBeUndefined();
      expect(node.state.end).toBeUndefined();
    });

    it('should default isDynamic to false', () => {
      const state: TimeRangeSourceState = {
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
      };

      const node = new TimeRangeSourceNode(state);

      expect(node.state.isDynamic).toBe(false);
    });

    it('should create node in dynamic mode and update from selection', () => {
      const state: TimeRangeSourceState = {
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: true,
      };

      const node = new TimeRangeSourceNode(state);

      expect(node.state.isDynamic).toBe(true);
      // Dynamic mode immediately updates from selection (falls back to full trace)
      expect(node.state.start).toEqual(Time.fromRaw(0n));
      expect(node.state.end).toEqual(Time.fromRaw(1000000n));
    });
  });

  describe('validation', () => {
    it('should validate when start and end are set', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      expect(node.validate()).toBe(true);
    });

    it('should invalidate when start is missing', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      expect(node.validate()).toBe(false);
    });

    it('should invalidate when end is missing', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        isDynamic: false,
      });

      expect(node.validate()).toBe(false);
    });

    it('should invalidate when both start and end are missing', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        isDynamic: false,
      });

      expect(node.validate()).toBe(false);
    });

    it('should invalidate when end is before start', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(500n),
        end: Time.fromRaw(100n),
        isDynamic: false,
      });

      expect(node.validate()).toBe(false);
    });

    it('should validate when end equals start (zero duration)', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(100n),
        isDynamic: false,
      });

      expect(node.validate()).toBe(true);
    });
  });

  describe('getTimeRange', () => {
    it('should return TimeSpan when valid', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      const timeRange = node.getTimeRange();

      expect(timeRange).toBeDefined();
      expect(timeRange?.start).toEqual(Time.fromRaw(100n));
      expect(timeRange?.end).toEqual(Time.fromRaw(500n));
    });

    it('should return undefined when invalid', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        isDynamic: false,
      });

      const timeRange = node.getTimeRange();

      expect(timeRange).toBeUndefined();
    });
  });

  describe('getStructuredQuery', () => {
    it('should generate SQL with single row for valid time range', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      const query = node.getStructuredQuery();

      expect(query).toBeDefined();
      // Query should use experimentalTimeRange with STATIC mode (0)
      expect(query?.experimentalTimeRange?.mode).toBe(0); // STATIC
      expect(query?.experimentalTimeRange?.ts).toBe(100);
      expect(query?.experimentalTimeRange?.dur).toBe(400);
    });

    it('should return query with unset ts/dur for node without start/end', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        isDynamic: false,
      });

      const query = node.getStructuredQuery();

      // Should return a valid query with DYNAMIC mode - backend will use trace_start()/trace_dur()
      expect(query).toBeDefined();
      expect(query?.experimentalTimeRange).toBeDefined();
      expect(query?.experimentalTimeRange?.mode).toBe(1); // DYNAMIC
    });

    it('should handle zero duration', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(100n),
        isDynamic: false,
      });

      const query = node.getStructuredQuery();

      expect(query).toBeDefined();
      expect(query?.experimentalTimeRange?.mode).toBe(0); // STATIC
      expect(query?.experimentalTimeRange?.ts).toBe(100);
      expect(query?.experimentalTimeRange?.dur).toBe(0);
    });

    it('should return undefined when only end is set without start', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      const query = node.getStructuredQuery();

      // Cannot generate a query without a start point
      expect(query).toBeUndefined();
    });
  });

  describe('serialization', () => {
    it('should serialize static node with start and end', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      const serialized = node.serializeState();

      expect(serialized.start).toBe('100');
      expect(serialized.end).toBe('500');
      expect(serialized.isDynamic).toBe(false);
    });

    it('should serialize dynamic node without start/end', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(200n),
        end: Time.fromRaw(800n),
        isDynamic: true,
      });

      const serialized = node.serializeState();

      // Dynamic nodes don't serialize start/end - they're populated from selection on load
      expect(serialized.start).toBeUndefined();
      expect(serialized.end).toBeUndefined();
      expect(serialized.isDynamic).toBe(true);
    });

    it('should serialize node with undefined start/end', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        isDynamic: false,
      });

      const serialized = node.serializeState();

      expect(serialized.start).toBeUndefined();
      expect(serialized.end).toBeUndefined();
      expect(serialized.isDynamic).toBe(false);
    });

    it('should serialize state', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      const serialized = node.serializeState();
      expect(serialized).toBeDefined();
    });
  });

  describe('deserialization', () => {
    it('should deserialize static node with start and end', () => {
      const serialized: TimeRangeSourceSerializedState = {
        start: '100',
        end: '500',
        isDynamic: false,
      };

      const state = TimeRangeSourceNode.deserializeState(
        createMockTrace(),
        serialized,
      );

      expect(state.start).toEqual(Time.fromRaw(100n));
      expect(state.end).toEqual(Time.fromRaw(500n));
      expect(state.isDynamic).toBe(false);
    });

    it('should deserialize dynamic node without start/end', () => {
      // Dynamic nodes are serialized without start/end
      const serialized: TimeRangeSourceSerializedState = {
        isDynamic: true,
      };

      const state = TimeRangeSourceNode.deserializeState(
        createMockTrace(),
        serialized,
      );

      // Deserialize returns state with undefined start/end - constructor will populate
      expect(state.start).toBeUndefined();
      expect(state.end).toBeUndefined();
      expect(state.isDynamic).toBe(true);
    });

    it('should deserialize with undefined start/end', () => {
      const serialized: TimeRangeSourceSerializedState = {
        isDynamic: false,
      };

      const state = TimeRangeSourceNode.deserializeState(
        createMockTrace(),
        serialized,
      );

      expect(state.start).toBeUndefined();
      expect(state.end).toBeUndefined();
      expect(state.isDynamic).toBe(false);
    });

    it('should default isDynamic to false when undefined', () => {
      const serialized: TimeRangeSourceSerializedState = {
        start: '100',
        end: '500',
      };

      const state = TimeRangeSourceNode.deserializeState(
        createMockTrace(),
        serialized,
      );

      expect(state.isDynamic).toBe(false);
    });

    it('should deserialize state', () => {
      const serialized: TimeRangeSourceSerializedState = {
        start: '100',
        end: '500',
        isDynamic: false,
      };

      const state = TimeRangeSourceNode.deserializeState(
        createMockTrace(),
        serialized,
      );
      expect(state).toBeDefined();
    });

    it('should preserve trace reference', () => {
      const mockTrace = createMockTrace();
      const serialized: TimeRangeSourceSerializedState = {
        start: '100',
        end: '500',
        isDynamic: false,
      };

      const state = TimeRangeSourceNode.deserializeState(mockTrace, serialized);

      expect(state.trace).toBe(mockTrace);
    });
  });

  describe('clone', () => {
    it('should clone node as static snapshot', () => {
      const originalNode = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: true, // Original is dynamic
      });

      const clonedNode = originalNode.clone() as TimeRangeSourceNode;

      // Dynamic node's start/end are updated to trace range (0/1000000) on construction
      expect(clonedNode.state.start).toEqual(originalNode.state.start);
      expect(clonedNode.state.end).toEqual(originalNode.state.end);
      expect(clonedNode.state.start).toEqual(Time.fromRaw(0n));
      expect(clonedNode.state.end).toEqual(Time.fromRaw(1000000n));
      expect(clonedNode.state.isDynamic).toBe(false); // Clone is always static
      expect(clonedNode.nodeId).not.toBe(originalNode.nodeId);
    });

    it('should clone successfully', () => {
      const originalNode = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      const clonedNode = originalNode.clone() as TimeRangeSourceNode;
      expect(clonedNode).toBeDefined();
    });
  });

  describe('getTitle', () => {
    it('should return "Time range" for static mode', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      expect(node.getTitle()).toBe('Time range');
    });

    it('should return "Current time range" for dynamic mode', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        isDynamic: true,
      });

      expect(node.getTitle()).toBe('Current time range');
    });
  });

  describe('finalCols', () => {
    it('should have id, ts, and dur columns', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      expect(node.finalCols.length).toBe(3);
      expect(node.finalCols[0].name).toBe('id');
      expect(node.finalCols[1].name).toBe('ts');
      expect(node.finalCols[2].name).toBe('dur');
    });

    it('should have correct column types', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      expect(node.finalCols.length).toBe(3);
      expect(node.finalCols[0].name).toBe('id');
      expect(node.finalCols[0].type).toBe('INT');
      expect(node.finalCols[1].name).toBe('ts');
      expect(node.finalCols[1].type).toBe('TIMESTAMP');
      expect(node.finalCols[2].name).toBe('dur');
      expect(node.finalCols[2].type).toBe('DURATION');
    });
  });

  describe('edge cases', () => {
    it('should handle very large timestamps', () => {
      const largeStart = Time.fromRaw(9223372036854775000n);
      const largeEnd = Time.fromRaw(9223372036854775807n); // Near max int64

      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: largeStart,
        end: largeEnd,
        isDynamic: false,
      });

      expect(node.validate()).toBe(true);
      const serialized = node.serializeState();
      expect(serialized.start).toBe('9223372036854775000');
      expect(serialized.end).toBe('9223372036854775807');
    });

    it('should handle timestamp at zero', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(0n),
        end: Time.fromRaw(1000n),
        isDynamic: false,
      });

      expect(node.validate()).toBe(true);
      const query = node.getStructuredQuery();
      expect(query?.experimentalTimeRange?.ts).toBe(0);
      expect(query?.experimentalTimeRange?.dur).toBe(1000);
    });

    it('should serialize and deserialize round-trip correctly for static node', () => {
      const originalNode = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(12345n),
        end: Time.fromRaw(67890n),
        isDynamic: false,
      });

      const serialized = originalNode.serializeState();
      const deserializedState = TimeRangeSourceNode.deserializeState(
        createMockTrace(),
        serialized,
      );
      const newNode = new TimeRangeSourceNode(deserializedState);

      expect(newNode.state.start).toEqual(originalNode.state.start);
      expect(newNode.state.end).toEqual(originalNode.state.end);
      expect(newNode.state.isDynamic).toBe(originalNode.state.isDynamic);
    });

    it('should serialize and deserialize round-trip for dynamic node', () => {
      const originalNode = new TimeRangeSourceNode({
        trace: createMockTrace(),
        isDynamic: true,
      });

      const serialized = originalNode.serializeState();
      const deserializedState = TimeRangeSourceNode.deserializeState(
        createMockTrace(),
        serialized,
      );
      const newNode = new TimeRangeSourceNode(deserializedState);

      // Dynamic nodes get their start/end from trace selection, not serialized state
      expect(newNode.state.start).toEqual(originalNode.state.start);
      expect(newNode.state.end).toEqual(originalNode.state.end);
      expect(newNode.state.isDynamic).toBe(true);
    });
  });

  describe('dispose', () => {
    beforeEach(() => {
      jest.useFakeTimers();
    });

    afterEach(() => {
      jest.useRealTimers();
    });

    it('should clean up interval when dispose is called on dynamic node', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: true,
      });

      // Verify interval is set up
      expect(jest.getTimerCount()).toBeGreaterThan(0);

      node.dispose();

      // Verify interval is cleared
      expect(jest.getTimerCount()).toBe(0);
    });

    it('should not throw when dispose is called on static node', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: false,
      });

      expect(() => node.dispose()).not.toThrow();
    });

    it('should allow multiple calls to dispose', () => {
      const node = new TimeRangeSourceNode({
        trace: createMockTrace(),
        start: Time.fromRaw(100n),
        end: Time.fromRaw(500n),
        isDynamic: true,
      });

      node.dispose();
      expect(() => node.dispose()).not.toThrow();
      expect(() => node.dispose()).not.toThrow();
    });

    it('should stop polling after dispose', () => {
      const mockTrace = createMockTrace();
      let callCount = 0;
      mockTrace.selection.getTimeSpanOfSelection = jest.fn(() => {
        callCount++;
        return undefined;
      });

      const node = new TimeRangeSourceNode({
        trace: mockTrace,
        isDynamic: true,
      });

      // Advance time to trigger some polls (1 initial call + 3 from interval)
      jest.advanceTimersByTime(600);
      const pollsBeforeDispose = callCount;

      node.dispose();

      // Advance time again - should not trigger more polls
      jest.advanceTimersByTime(600);
      expect(callCount).toBe(pollsBeforeDispose);
    });
  });

  describe('dynamic mode behavior', () => {
    beforeEach(() => {
      jest.useFakeTimers();
    });

    afterEach(() => {
      jest.useRealTimers();
    });

    it('should update times from selection in dynamic mode', () => {
      const mockTrace = createMockTrace();
      let currentTime = 100n;
      mockTrace.selection.getTimeSpanOfSelection = jest.fn(() => {
        return new TimeSpan(
          Time.fromRaw(currentTime),
          Time.fromRaw(currentTime + 400n),
        );
      });

      const node = new TimeRangeSourceNode({
        trace: mockTrace,
        isDynamic: true,
      });

      // Values should be updated immediately on construction
      expect(node.state.start).toEqual(Time.fromRaw(100n));
      expect(node.state.end).toEqual(Time.fromRaw(500n));

      // Change the selection
      currentTime = 200n;

      // Advance timer to trigger poll
      jest.advanceTimersByTime(200);

      // Values should be updated
      expect(node.state.start).toEqual(Time.fromRaw(200n));
      expect(node.state.end).toEqual(Time.fromRaw(600n));

      node.dispose();
    });

    it('should not update times in static mode', () => {
      const mockTrace = createMockTrace();
      let currentTime = 100n;
      mockTrace.selection.getTimeSpanOfSelection = jest.fn(() => {
        return new TimeSpan(
          Time.fromRaw(currentTime),
          Time.fromRaw(currentTime + 400n),
        );
      });

      const node = new TimeRangeSourceNode({
        trace: mockTrace,
        start: Time.fromRaw(50n),
        end: Time.fromRaw(150n),
        isDynamic: false,
      });

      // Values should remain unchanged
      expect(node.state.start).toEqual(Time.fromRaw(50n));
      expect(node.state.end).toEqual(Time.fromRaw(150n));

      // Change the selection
      currentTime = 200n;

      // Advance timer (should have no effect in static mode)
      jest.advanceTimersByTime(1000);

      // Values should still be unchanged
      expect(node.state.start).toEqual(Time.fromRaw(50n));
      expect(node.state.end).toEqual(Time.fromRaw(150n));
    });

    it('should use full trace range when no selection exists in dynamic mode', () => {
      const mockTrace = createMockTrace();
      mockTrace.selection.getTimeSpanOfSelection = jest.fn(() => undefined);

      const node = new TimeRangeSourceNode({
        trace: mockTrace,
        isDynamic: true,
      });

      // Should fall back to full trace range immediately on construction
      expect(node.state.start).toEqual(mockTrace.traceInfo.start);
      expect(node.state.end).toEqual(mockTrace.traceInfo.end);

      node.dispose();
    });
  });
});

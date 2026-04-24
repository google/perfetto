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
  TimeRangeSourceNodeAttrs,
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
      const node = new TimeRangeSourceNode(
        {
          start: '100',
          end: '500',
          isDynamic: false,
        },
        {trace: createMockTrace()},
      );

      expect(node.start).toEqual(Time.fromRaw(100n));
      expect(node.end).toEqual(Time.fromRaw(500n));
      expect(node.isDynamic).toBe(false);
    });

    it('should create node with undefined start/end', () => {
      const node = new TimeRangeSourceNode(
        {isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.start).toBeUndefined();
      expect(node.end).toBeUndefined();
    });

    it('should default isDynamic to false', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500'},
        {trace: createMockTrace()},
      );

      expect(node.isDynamic).toBe(false);
    });

    it('should create node in dynamic mode and update from selection', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: true},
        {trace: createMockTrace()},
      );

      expect(node.isDynamic).toBe(true);
      // Dynamic mode immediately updates from selection (falls back to full trace)
      expect(node.start).toEqual(Time.fromRaw(0n));
      expect(node.end).toEqual(Time.fromRaw(1000000n));
    });
  });

  describe('validation', () => {
    it('should validate when start and end are set', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.validate()).toBe(true);
    });

    it('should invalidate when start is missing', () => {
      const node = new TimeRangeSourceNode(
        {end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.validate()).toBe(false);
    });

    it('should invalidate when end is missing', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.validate()).toBe(false);
    });

    it('should invalidate when both start and end are missing', () => {
      const node = new TimeRangeSourceNode(
        {isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.validate()).toBe(false);
    });

    it('should invalidate when end is before start', () => {
      const node = new TimeRangeSourceNode(
        {start: '500', end: '100', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.validate()).toBe(false);
    });

    it('should validate when end equals start (zero duration)', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '100', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.validate()).toBe(true);
    });
  });

  describe('getTimeRange', () => {
    it('should return TimeSpan when valid', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      const timeRange = node.getTimeRange();

      expect(timeRange).toBeDefined();
      expect(timeRange?.start).toEqual(Time.fromRaw(100n));
      expect(timeRange?.end).toEqual(Time.fromRaw(500n));
    });

    it('should return undefined when invalid', () => {
      const node = new TimeRangeSourceNode(
        {isDynamic: false},
        {trace: createMockTrace()},
      );

      const timeRange = node.getTimeRange();

      expect(timeRange).toBeUndefined();
    });
  });

  describe('getStructuredQuery', () => {
    it('should generate SQL with single row for valid time range', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      const query = node.getStructuredQuery();

      expect(query).toBeDefined();
      // Query should use experimentalTimeRange with STATIC mode (0)
      expect(query?.experimentalTimeRange?.mode).toBe(0); // STATIC
      expect(query?.experimentalTimeRange?.ts).toBe(100);
      expect(query?.experimentalTimeRange?.dur).toBe(400);
    });

    it('should return query with unset ts/dur for node without start/end', () => {
      const node = new TimeRangeSourceNode(
        {isDynamic: false},
        {trace: createMockTrace()},
      );

      const query = node.getStructuredQuery();

      // Should return a valid query with DYNAMIC mode - backend will use trace_start()/trace_dur()
      expect(query).toBeDefined();
      expect(query?.experimentalTimeRange).toBeDefined();
      expect(query?.experimentalTimeRange?.mode).toBe(1); // DYNAMIC
    });

    it('should handle zero duration', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '100', isDynamic: false},
        {trace: createMockTrace()},
      );

      const query = node.getStructuredQuery();

      expect(query).toBeDefined();
      expect(query?.experimentalTimeRange?.mode).toBe(0); // STATIC
      expect(query?.experimentalTimeRange?.ts).toBe(100);
      expect(query?.experimentalTimeRange?.dur).toBe(0);
    });

    it('should return undefined when only end is set without start', () => {
      const node = new TimeRangeSourceNode(
        {end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      const query = node.getStructuredQuery();

      // Cannot generate a query without a start point
      expect(query).toBeUndefined();
    });
  });

  describe('serialization', () => {
    it('should have start and end in attrs for static node', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.attrs.start).toBe('100');
      expect(node.attrs.end).toBe('500');
      expect(node.attrs.isDynamic).toBe(false);
    });

    it('should not have start/end in attrs for dynamic node', () => {
      const node = new TimeRangeSourceNode(
        {start: '200', end: '800', isDynamic: true},
        {trace: createMockTrace()},
      );

      // Dynamic nodes don't persist start/end - they're populated from selection on load
      expect(node.attrs.start).toBeUndefined();
      expect(node.attrs.end).toBeUndefined();
      expect(node.attrs.isDynamic).toBe(true);
    });

    it('should have undefined start/end in attrs when not set', () => {
      const node = new TimeRangeSourceNode(
        {isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.attrs.start).toBeUndefined();
      expect(node.attrs.end).toBeUndefined();
      expect(node.attrs.isDynamic).toBe(false);
    });

    it('attrs should be defined', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.attrs).toBeDefined();
    });
  });

  describe('deserialization', () => {
    it('should deserialize static node with start and end', () => {
      const serialized: TimeRangeSourceNodeAttrs = {
        start: '100',
        end: '500',
        isDynamic: false,
      };

      const node = new TimeRangeSourceNode(serialized, {
        trace: createMockTrace(),
      });

      expect(node.start).toEqual(Time.fromRaw(100n));
      expect(node.end).toEqual(Time.fromRaw(500n));
      expect(node.isDynamic).toBe(false);
    });

    it('should deserialize dynamic node without start/end', () => {
      // Dynamic nodes are serialized without start/end
      const serialized: TimeRangeSourceNodeAttrs = {
        isDynamic: true,
      };

      const node = new TimeRangeSourceNode(serialized, {
        trace: createMockTrace(),
      });

      // Dynamic node populates from selection/trace on construction
      expect(node.isDynamic).toBe(true);
    });

    it('should deserialize with undefined start/end', () => {
      const serialized: TimeRangeSourceNodeAttrs = {
        isDynamic: false,
      };

      const node = new TimeRangeSourceNode(serialized, {
        trace: createMockTrace(),
      });

      expect(node.start).toBeUndefined();
      expect(node.end).toBeUndefined();
      expect(node.isDynamic).toBe(false);
    });

    it('should default isDynamic to false when undefined', () => {
      const serialized: TimeRangeSourceNodeAttrs = {
        start: '100',
        end: '500',
      };

      const node = new TimeRangeSourceNode(serialized, {
        trace: createMockTrace(),
      });

      expect(node.isDynamic).toBe(false);
    });

    it('should deserialize state', () => {
      const serialized: TimeRangeSourceNodeAttrs = {
        start: '100',
        end: '500',
        isDynamic: false,
      };

      const node = new TimeRangeSourceNode(serialized, {
        trace: createMockTrace(),
      });
      expect(node).toBeDefined();
    });

    it('should preserve trace reference', () => {
      const mockTrace = createMockTrace();
      const serialized: TimeRangeSourceNodeAttrs = {
        start: '100',
        end: '500',
        isDynamic: false,
      };

      const node = new TimeRangeSourceNode(serialized, {trace: mockTrace});

      expect(node.context.trace).toBe(mockTrace);
    });
  });

  describe('clone', () => {
    it('should clone node as static snapshot', () => {
      const originalNode = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: true},
        {trace: createMockTrace()},
      );

      const clonedNode = originalNode.clone() as TimeRangeSourceNode;

      // Dynamic node's start/end are updated to trace range (0/1000000) on construction
      expect(clonedNode.start).toEqual(originalNode.start);
      expect(clonedNode.end).toEqual(originalNode.end);
      expect(clonedNode.start).toEqual(Time.fromRaw(0n));
      expect(clonedNode.end).toEqual(Time.fromRaw(1000000n));
      expect(clonedNode.isDynamic).toBe(false); // Clone is always static
      expect(clonedNode.nodeId).not.toBe(originalNode.nodeId);
    });

    it('should clone successfully', () => {
      const originalNode = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      const clonedNode = originalNode.clone() as TimeRangeSourceNode;
      expect(clonedNode).toBeDefined();
    });
  });

  describe('getTitle', () => {
    it('should return "Time range" for static mode', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.getTitle()).toBe('Time range');
    });

    it('should return "Current time range" for dynamic mode', () => {
      const node = new TimeRangeSourceNode(
        {isDynamic: true},
        {trace: createMockTrace()},
      );

      expect(node.getTitle()).toBe('Current time range');
    });
  });

  describe('finalCols', () => {
    it('should have id, ts, and dur columns', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.finalCols.length).toBe(3);
      expect(node.finalCols[0].name).toBe('id');
      expect(node.finalCols[1].name).toBe('ts');
      expect(node.finalCols[2].name).toBe('dur');
    });

    it('should have correct column types', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.finalCols.length).toBe(3);
      expect(node.finalCols[0].name).toBe('id');
      expect(node.finalCols[0].type).toEqual({kind: 'int'});
      expect(node.finalCols[1].name).toBe('ts');
      expect(node.finalCols[1].type).toEqual({kind: 'timestamp'});
      expect(node.finalCols[2].name).toBe('dur');
      expect(node.finalCols[2].type).toEqual({kind: 'duration'});
    });
  });

  describe('edge cases', () => {
    it('should handle very large timestamps', () => {
      const largeStart = '9223372036854775000';
      const largeEnd = '9223372036854775807'; // Near max int64

      const node = new TimeRangeSourceNode(
        {start: largeStart, end: largeEnd, isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.validate()).toBe(true);
      expect(node.attrs.start).toBe('9223372036854775000');
      expect(node.attrs.end).toBe('9223372036854775807');
    });

    it('should handle timestamp at zero', () => {
      const node = new TimeRangeSourceNode(
        {start: '0', end: '1000', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(node.validate()).toBe(true);
      const query = node.getStructuredQuery();
      expect(query?.experimentalTimeRange?.ts).toBe(0);
      expect(query?.experimentalTimeRange?.dur).toBe(1000);
    });

    it('should serialize and deserialize round-trip correctly for static node', () => {
      const originalNode = new TimeRangeSourceNode(
        {start: '12345', end: '67890', isDynamic: false},
        {trace: createMockTrace()},
      );

      const newNode = new TimeRangeSourceNode(originalNode.attrs, {
        trace: createMockTrace(),
      });

      expect(newNode.start).toEqual(originalNode.start);
      expect(newNode.end).toEqual(originalNode.end);
      expect(newNode.isDynamic).toBe(originalNode.isDynamic);
    });

    it('should serialize and deserialize round-trip for dynamic node', () => {
      const originalNode = new TimeRangeSourceNode(
        {isDynamic: true},
        {trace: createMockTrace()},
      );

      const newNode = new TimeRangeSourceNode(originalNode.attrs, {
        trace: createMockTrace(),
      });

      // Dynamic nodes get their start/end from trace selection, not serialized state
      expect(newNode.start).toEqual(originalNode.start);
      expect(newNode.end).toEqual(originalNode.end);
      expect(newNode.isDynamic).toBe(true);
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
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: true},
        {trace: createMockTrace()},
      );

      // Verify interval is set up
      expect(jest.getTimerCount()).toBeGreaterThan(0);

      node.dispose();

      // Verify interval is cleared
      expect(jest.getTimerCount()).toBe(0);
    });

    it('should not throw when dispose is called on static node', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: false},
        {trace: createMockTrace()},
      );

      expect(() => node.dispose()).not.toThrow();
    });

    it('should allow multiple calls to dispose', () => {
      const node = new TimeRangeSourceNode(
        {start: '100', end: '500', isDynamic: true},
        {trace: createMockTrace()},
      );

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

      const node = new TimeRangeSourceNode(
        {isDynamic: true},
        {trace: mockTrace},
      );

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

      const node = new TimeRangeSourceNode(
        {isDynamic: true},
        {trace: mockTrace},
      );

      // Values should be updated immediately on construction
      expect(node.start).toEqual(Time.fromRaw(100n));
      expect(node.end).toEqual(Time.fromRaw(500n));

      // Change the selection
      currentTime = 200n;

      // Advance timer to trigger poll
      jest.advanceTimersByTime(200);

      // Values should be updated
      expect(node.start).toEqual(Time.fromRaw(200n));
      expect(node.end).toEqual(Time.fromRaw(600n));

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

      const node = new TimeRangeSourceNode(
        {start: '50', end: '150', isDynamic: false},
        {trace: mockTrace},
      );

      // Values should remain unchanged
      expect(node.start).toEqual(Time.fromRaw(50n));
      expect(node.end).toEqual(Time.fromRaw(150n));

      // Change the selection
      currentTime = 200n;

      // Advance timer (should have no effect in static mode)
      jest.advanceTimersByTime(1000);

      // Values should still be unchanged
      expect(node.start).toEqual(Time.fromRaw(50n));
      expect(node.end).toEqual(Time.fromRaw(150n));
    });

    it('should use full trace range when no selection exists in dynamic mode', () => {
      const mockTrace = createMockTrace();
      mockTrace.selection.getTimeSpanOfSelection = jest.fn(() => undefined);

      const node = new TimeRangeSourceNode(
        {isDynamic: true},
        {trace: mockTrace},
      );

      // Should fall back to full trace range immediately on construction
      expect(node.start).toEqual(mockTrace.traceInfo.start);
      expect(node.end).toEqual(mockTrace.traceInfo.end);

      node.dispose();
    });
  });
});

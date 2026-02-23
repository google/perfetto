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

import {recentGraphsStorage, RecentGraphEntry} from './recent_graphs';

describe('RecentGraphsStorage', () => {
  beforeEach(() => {
    // Clear localStorage before each test
    window.localStorage.clear();
    // Reset the storage instance
    recentGraphsStorage.data = [];
  });

  // Helper to create a mock graph entry
  function createEntry(
    nodeCount: number,
    options?: Partial<RecentGraphEntry>,
  ): RecentGraphEntry {
    return {
      name: options?.name ?? `Graph ${nodeCount}`,
      json: options?.json ?? `{"nodes": ${nodeCount}}`,
      timestamp: options?.timestamp ?? Date.now(),
      nodeCount,
      labelCount: options?.labelCount ?? 0,
      starred: options?.starred ?? false,
    };
  }

  describe('data getter/setter', () => {
    test('should start with empty data after clear', () => {
      expect(recentGraphsStorage.data.length).toBe(0);
    });

    test('should allow setting data directly', () => {
      recentGraphsStorage.data = [createEntry(3)];
      expect(recentGraphsStorage.data.length).toBe(1);
      expect(recentGraphsStorage.data[0].nodeCount).toBe(3);
    });
  });

  describe('finalizeCurrentGraph', () => {
    test('should do nothing when there is no working graph', () => {
      recentGraphsStorage.finalizeCurrentGraph();
      expect(recentGraphsStorage.data.length).toBe(0);
    });

    test('should do nothing when working graph has zero nodes', () => {
      recentGraphsStorage.data = [createEntry(0)];
      recentGraphsStorage.finalizeCurrentGraph();
      // Should still be just one entry (unchanged)
      expect(recentGraphsStorage.data.length).toBe(1);
    });

    test('should finalize current graph and create new working slot', () => {
      recentGraphsStorage.data = [createEntry(3)];
      expect(recentGraphsStorage.data.length).toBe(1);

      recentGraphsStorage.finalizeCurrentGraph();

      // Should now have 2 entries: new working slot at 0, finalized graph at 1
      expect(recentGraphsStorage.data.length).toBe(2);
      expect(recentGraphsStorage.data[0].nodeCount).toBe(0); // New empty working slot
      expect(recentGraphsStorage.data[1].nodeCount).toBe(3); // Finalized graph
    });

    test('should enforce maxItems limit for unstarred graphs', () => {
      const originalMaxItems = recentGraphsStorage.maxItems;
      recentGraphsStorage.maxItems = 3;

      // Set up 3 existing finalized graphs (index 0 is working slot)
      recentGraphsStorage.data = [
        createEntry(4), // Working slot with content
        createEntry(3),
        createEntry(2),
        createEntry(1),
      ];

      // Finalize current - should remove oldest unstarred
      recentGraphsStorage.finalizeCurrentGraph();

      // Count unstarred items with content
      const unstarredWithContent = recentGraphsStorage.data.filter(
        (e) => !e.starred && (e.nodeCount ?? 0) > 0,
      );

      // Should have at most maxItems unstarred graphs with content
      expect(unstarredWithContent.length).toBeLessThanOrEqual(3);

      recentGraphsStorage.maxItems = originalMaxItems;
    });

    test('should not count starred graphs toward maxItems limit', () => {
      const originalMaxItems = recentGraphsStorage.maxItems;
      recentGraphsStorage.maxItems = 2;

      // Set up data with one starred graph and working slot
      recentGraphsStorage.data = [
        createEntry(4), // Working slot with content
        createEntry(3, {starred: true}), // Starred - should be preserved
        createEntry(2),
        createEntry(1),
      ];

      recentGraphsStorage.finalizeCurrentGraph();

      // Count starred and unstarred with content
      const starred = recentGraphsStorage.data.filter(
        (e) => e.starred && (e.nodeCount ?? 0) > 0,
      );
      const unstarred = recentGraphsStorage.data.filter(
        (e) => !e.starred && (e.nodeCount ?? 0) > 0,
      );

      // Should have 1 starred graph preserved
      expect(starred.length).toBe(1);

      // Should have at most maxItems unstarred graphs
      expect(unstarred.length).toBeLessThanOrEqual(2);

      recentGraphsStorage.maxItems = originalMaxItems;
    });
  });

  describe('getCurrentJson', () => {
    test('should return undefined when no graphs exist', () => {
      expect(recentGraphsStorage.getCurrentJson()).toBeUndefined();
    });

    test('should return undefined when only empty working slot exists', () => {
      recentGraphsStorage.data = [createEntry(0)];
      expect(recentGraphsStorage.getCurrentJson()).toBeUndefined();
    });

    test('should return first graph with content', () => {
      recentGraphsStorage.data = [createEntry(2, {json: '{"test": true}'})];

      const json = recentGraphsStorage.getCurrentJson();
      expect(json).toBe('{"test": true}');
    });

    test('should skip empty working slot and return finalized graph', () => {
      recentGraphsStorage.data = [
        createEntry(0), // Empty working slot
        createEntry(3, {json: '{"finalized": true}'}),
      ];

      const json = recentGraphsStorage.getCurrentJson();
      expect(json).toBe('{"finalized": true}');
    });
  });

  describe('setStarred', () => {
    test('should star a graph', () => {
      recentGraphsStorage.data = [createEntry(2)];
      recentGraphsStorage.setStarred(0, true);

      expect(recentGraphsStorage.data[0].starred).toBe(true);
    });

    test('should unstar a graph', () => {
      recentGraphsStorage.data = [createEntry(2, {starred: true})];
      recentGraphsStorage.setStarred(0, false);

      expect(recentGraphsStorage.data[0].starred).toBe(false);
    });
  });

  describe('rename', () => {
    test('should rename a graph', () => {
      recentGraphsStorage.data = [createEntry(2)];
      recentGraphsStorage.rename(0, 'My Custom Name');

      expect(recentGraphsStorage.data[0].name).toBe('My Custom Name');
    });

    test('should trim whitespace from name', () => {
      recentGraphsStorage.data = [createEntry(2)];
      recentGraphsStorage.rename(0, '  Trimmed Name  ');

      expect(recentGraphsStorage.data[0].name).toBe('Trimmed Name');
    });

    test('should keep original name if new name is empty', () => {
      recentGraphsStorage.data = [createEntry(2, {name: 'Original'})];
      recentGraphsStorage.rename(0, '   ');

      expect(recentGraphsStorage.data[0].name).toBe('Original');
    });
  });

  describe('getJson', () => {
    test('should return undefined for invalid index', () => {
      expect(recentGraphsStorage.getJson(-1)).toBeUndefined();
      expect(recentGraphsStorage.getJson(0)).toBeUndefined();
      expect(recentGraphsStorage.getJson(100)).toBeUndefined();
    });

    test('should return json for valid index', () => {
      recentGraphsStorage.data = [createEntry(2, {json: '{"test": 123}'})];
      const json = recentGraphsStorage.getJson(0);

      expect(json).toBe('{"test": 123}');
    });
  });

  describe('remove', () => {
    test('should remove a graph from history', () => {
      recentGraphsStorage.data = [
        createEntry(0), // Working slot
        createEntry(3),
        createEntry(2),
        createEntry(1),
      ];

      const initialLength = recentGraphsStorage.data.length;
      recentGraphsStorage.remove(2); // Remove middle entry

      expect(recentGraphsStorage.data.length).toBe(initialLength - 1);
      expect(recentGraphsStorage.data[2].nodeCount).toBe(1); // Last entry moved up
    });

    test('should remove first entry', () => {
      recentGraphsStorage.data = [createEntry(1), createEntry(2)];
      recentGraphsStorage.remove(0);

      expect(recentGraphsStorage.data.length).toBe(1);
      expect(recentGraphsStorage.data[0].nodeCount).toBe(2);
    });

    test('should remove last entry', () => {
      recentGraphsStorage.data = [createEntry(1), createEntry(2)];
      recentGraphsStorage.remove(1);

      expect(recentGraphsStorage.data.length).toBe(1);
      expect(recentGraphsStorage.data[0].nodeCount).toBe(1);
    });
  });

  describe('clear', () => {
    test('should clear all data', () => {
      recentGraphsStorage.data = [
        createEntry(1),
        createEntry(2),
        createEntry(3),
      ];
      recentGraphsStorage.clear();

      expect(recentGraphsStorage.data.length).toBe(0);
    });
  });

  describe('generateName', () => {
    test('should generate a name with date and time', () => {
      const name = recentGraphsStorage.generateName();

      // Should contain "Graph" and some date/time info
      expect(name).toContain('Graph');
      // Should match pattern like "Graph Jan 15 14:30"
      expect(name).toMatch(/Graph \w+ \d+ \d{2}:\d{2}/);
    });
  });

  describe('persistence', () => {
    test('should persist graphs to localStorage', () => {
      recentGraphsStorage.data = [createEntry(2)];
      // Trigger save by calling a mutating method
      recentGraphsStorage.setStarred(0, true);

      const stored = window.localStorage.getItem('recentExploreGraphs');
      expect(stored).not.toBeNull();

      const parsed = JSON.parse(stored!);
      expect(parsed.length).toBe(1);
      expect(parsed[0].nodeCount).toBe(2);
      expect(parsed[0].starred).toBe(true);
    });

    test('should persist renamed graph', () => {
      recentGraphsStorage.data = [createEntry(2)];
      recentGraphsStorage.rename(0, 'Custom Name');

      const stored = window.localStorage.getItem('recentExploreGraphs');
      const parsed = JSON.parse(stored!);
      expect(parsed[0].name).toBe('Custom Name');
    });

    test('should persist removal', () => {
      recentGraphsStorage.data = [createEntry(1), createEntry(2)];
      recentGraphsStorage.remove(0);

      const stored = window.localStorage.getItem('recentExploreGraphs');
      const parsed = JSON.parse(stored!);
      expect(parsed.length).toBe(1);
      expect(parsed[0].nodeCount).toBe(2);
    });
  });

  describe('historical graphs ordering', () => {
    test('starred graphs should be identifiable in data', () => {
      recentGraphsStorage.data = [
        createEntry(0), // Working slot
        createEntry(1, {starred: false}),
        createEntry(2, {starred: true}),
        createEntry(3, {starred: false}),
      ];

      const starred = recentGraphsStorage.data.filter(
        (e, i) => i > 0 && e.starred,
      );
      const unstarred = recentGraphsStorage.data.filter(
        (e, i) => i > 0 && !e.starred,
      );

      expect(starred.length).toBe(1);
      expect(starred[0].nodeCount).toBe(2);
      expect(unstarred.length).toBe(2);
    });
  });
});

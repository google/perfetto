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

import {queryHistoryStorage} from './query_history';

describe('HistoryStorage', () => {
  beforeEach(() => {
    // Clear localStorage before each test
    window.localStorage.clear();
    // Reset the storage instance by reloading data
    queryHistoryStorage.data = [];
  });

  describe('saveQuery', () => {
    it('should add new query at the front of history', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      expect(queryHistoryStorage.data.length).toBe(1);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 1');
      expect(queryHistoryStorage.data[0].starred).toBe(false);
    });

    it('should add multiple different queries with most recent first', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      queryHistoryStorage.saveQuery('SELECT 3');

      expect(queryHistoryStorage.data.length).toBe(3);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 3');
      expect(queryHistoryStorage.data[1].query).toBe('SELECT 2');
      expect(queryHistoryStorage.data[2].query).toBe('SELECT 1');
    });

    it('should move existing unstarred query to the front when rerun', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      queryHistoryStorage.saveQuery('SELECT 3');

      // Array is now: [SELECT 3, SELECT 2, SELECT 1]
      // Rerun SELECT 1 (currently at index 2)
      queryHistoryStorage.saveQuery('SELECT 1');

      expect(queryHistoryStorage.data.length).toBe(3);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 1');
      expect(queryHistoryStorage.data[1].query).toBe('SELECT 3');
      expect(queryHistoryStorage.data[2].query).toBe('SELECT 2');
    });

    it('should move existing starred query to the front and preserve starred status', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      queryHistoryStorage.saveQuery('SELECT 3');

      // Array is now: [SELECT 3, SELECT 2, SELECT 1]
      // Star SELECT 3 (at index 0)
      queryHistoryStorage.setStarred(0, true);
      expect(queryHistoryStorage.data[0].starred).toBe(true);

      // Rerun the starred query
      queryHistoryStorage.saveQuery('SELECT 3');

      expect(queryHistoryStorage.data.length).toBe(3);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 3');
      expect(queryHistoryStorage.data[0].starred).toBe(true);
      expect(queryHistoryStorage.data[1].query).toBe('SELECT 2');
      expect(queryHistoryStorage.data[2].query).toBe('SELECT 1');
    });

    it('should move query from middle of history to front', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      queryHistoryStorage.saveQuery('SELECT 3');
      queryHistoryStorage.saveQuery('SELECT 4');

      // Array is now: [SELECT 4, SELECT 3, SELECT 2, SELECT 1]
      // Rerun SELECT 2 (at index 2)
      queryHistoryStorage.saveQuery('SELECT 2');

      expect(queryHistoryStorage.data.length).toBe(4);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 2');
      expect(queryHistoryStorage.data[1].query).toBe('SELECT 4');
      expect(queryHistoryStorage.data[2].query).toBe('SELECT 3');
      expect(queryHistoryStorage.data[3].query).toBe('SELECT 1');
    });

    it('should handle rerunning the most recent query', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');

      // Array is now: [SELECT 2, SELECT 1]
      // Rerun SELECT 2 (already at index 0)
      queryHistoryStorage.saveQuery('SELECT 2');

      expect(queryHistoryStorage.data.length).toBe(2);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 2');
      expect(queryHistoryStorage.data[1].query).toBe('SELECT 1');
    });

    it('should enforce maxItems limit for unstarred queries', () => {
      // Set maxItems to a small value for testing
      const originalMaxItems = queryHistoryStorage.maxItems;
      queryHistoryStorage.maxItems = 3;

      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      queryHistoryStorage.saveQuery('SELECT 3');
      // Array is now: [SELECT 3, SELECT 2, SELECT 1]
      queryHistoryStorage.saveQuery('SELECT 4'); // Should remove SELECT 1 (oldest)

      expect(queryHistoryStorage.data.length).toBe(3);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 4');
      expect(queryHistoryStorage.data[1].query).toBe('SELECT 3');
      expect(queryHistoryStorage.data[2].query).toBe('SELECT 2');

      // Restore original maxItems
      queryHistoryStorage.maxItems = originalMaxItems;
    });

    it('should not count starred queries toward maxItems limit', () => {
      const originalMaxItems = queryHistoryStorage.maxItems;
      queryHistoryStorage.maxItems = 3;

      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      queryHistoryStorage.saveQuery('SELECT 3');
      // Array is now: [SELECT 3, SELECT 2, SELECT 1]

      // Star SELECT 3 (at index 0)
      queryHistoryStorage.setStarred(0, true);

      // Add one more query - now we have 3 unstarred queries total
      queryHistoryStorage.saveQuery('SELECT 4');
      // Array is now: [SELECT 4, SELECT 3 (starred), SELECT 2, SELECT 1]

      // Add another query - this should remove SELECT 1 (the oldest unstarred)
      queryHistoryStorage.saveQuery('SELECT 5');
      // Array should be: [SELECT 5, SELECT 4, SELECT 3 (starred), SELECT 2]

      // Should have 4 total: 1 starred + 3 unstarred
      expect(queryHistoryStorage.data.length).toBe(4);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 5');
      expect(queryHistoryStorage.data[1].query).toBe('SELECT 4');
      expect(queryHistoryStorage.data[2].query).toBe('SELECT 3');
      expect(queryHistoryStorage.data[2].starred).toBe(true);
      expect(queryHistoryStorage.data[3].query).toBe('SELECT 2');

      queryHistoryStorage.maxItems = originalMaxItems;
    });

    it('should handle moving starred query when at limit', () => {
      const originalMaxItems = queryHistoryStorage.maxItems;
      queryHistoryStorage.maxItems = 3;

      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      queryHistoryStorage.saveQuery('SELECT 3');
      queryHistoryStorage.saveQuery('SELECT 4');
      // Array is now: [SELECT 4, SELECT 3, SELECT 2] (SELECT 1 was removed)

      // Star SELECT 4 (at index 0)
      queryHistoryStorage.setStarred(0, true);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 4');

      // Rerun the starred query - it should stay at index 0
      queryHistoryStorage.saveQuery('SELECT 4');

      expect(queryHistoryStorage.data.length).toBe(3);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 4');
      expect(queryHistoryStorage.data[0].starred).toBe(true);
      expect(queryHistoryStorage.data[1].query).toBe('SELECT 3');
      expect(queryHistoryStorage.data[2].query).toBe('SELECT 2');

      queryHistoryStorage.maxItems = originalMaxItems;
    });
  });

  describe('setStarred', () => {
    it('should star a query', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.setStarred(0, true);

      expect(queryHistoryStorage.data[0].starred).toBe(true);
    });

    it('should unstar a query', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.setStarred(0, true);
      queryHistoryStorage.setStarred(0, false);

      expect(queryHistoryStorage.data[0].starred).toBe(false);
    });
  });

  describe('remove', () => {
    it('should remove a query from history', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      queryHistoryStorage.saveQuery('SELECT 3');
      // Array is now: [SELECT 3, SELECT 2, SELECT 1]

      queryHistoryStorage.remove(1); // Remove SELECT 2

      expect(queryHistoryStorage.data.length).toBe(2);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 3');
      expect(queryHistoryStorage.data[1].query).toBe('SELECT 1');
    });

    it('should remove first query', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      // Array is now: [SELECT 2, SELECT 1]

      queryHistoryStorage.remove(0); // Remove SELECT 2

      expect(queryHistoryStorage.data.length).toBe(1);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 1');
    });

    it('should remove last query', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      // Array is now: [SELECT 2, SELECT 1]

      queryHistoryStorage.remove(1); // Remove SELECT 1

      expect(queryHistoryStorage.data.length).toBe(1);
      expect(queryHistoryStorage.data[0].query).toBe('SELECT 2');
    });
  });

  describe('persistence', () => {
    it('should persist queries to localStorage', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.saveQuery('SELECT 2');
      // Array is now: [SELECT 2, SELECT 1]

      const stored = window.localStorage.getItem('queryHistory');
      expect(stored).not.toBeNull();

      const parsed = JSON.parse(stored!);
      expect(parsed.length).toBe(2);
      expect(parsed[0].query).toBe('SELECT 2');
      expect(parsed[1].query).toBe('SELECT 1');
    });

    it('should persist starred status', () => {
      queryHistoryStorage.saveQuery('SELECT 1');
      queryHistoryStorage.setStarred(0, true);

      const stored = window.localStorage.getItem('queryHistory');
      const parsed = JSON.parse(stored!);
      expect(parsed[0].starred).toBe(true);
    });
  });
});

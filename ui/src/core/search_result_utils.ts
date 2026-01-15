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

/**
 * Represents a single search result event.
 */
export interface SearchResultEvent {
  id: number;
  ts: bigint;
  trackUri: string;
}

/**
 * Detailed information about differences between two sets of search results.
 */
export interface SearchResultDifference {
  type: 'count' | 'content';
  result1Count: number;
  result2Count: number;
  missingInResult2: SearchResultEvent[];
  missingInResult1: SearchResultEvent[];
  different: Array<{
    id: number;
    result1Ts: bigint;
    result2Ts: bigint;
    result1TrackUri: string;
    result2TrackUri: string;
  }>;
}

/**
 * Compares two sets of search results and returns detailed differences.
 *
 * @param result1 - First set of search results
 * @param result2 - Second set of search results
 * @returns Detailed difference information
 */
export function compareSearchResults(
  result1: SearchResultEvent[],
  result2: SearchResultEvent[],
): SearchResultDifference {
  const map1 = new Map<number, {ts: bigint; trackUri: string}>();
  const map2 = new Map<number, {ts: bigint; trackUri: string}>();

  // Build maps keyed by ID
  result1.forEach((r) => map1.set(r.id, {ts: r.ts, trackUri: r.trackUri}));
  result2.forEach((r) => map2.set(r.id, {ts: r.ts, trackUri: r.trackUri}));

  const missingInResult2: SearchResultEvent[] = [];
  const missingInResult1: SearchResultEvent[] = [];
  const different: Array<{
    id: number;
    result1Ts: bigint;
    result2Ts: bigint;
    result1TrackUri: string;
    result2TrackUri: string;
  }> = [];

  // Find items in result1 but not in result2, or with differences
  map1.forEach((value, id) => {
    const result2Value = map2.get(id);
    if (!result2Value) {
      missingInResult2.push({id, ts: value.ts, trackUri: value.trackUri});
    } else if (
      value.ts !== result2Value.ts ||
      value.trackUri !== result2Value.trackUri
    ) {
      different.push({
        id,
        result1Ts: value.ts,
        result2Ts: result2Value.ts,
        result1TrackUri: value.trackUri,
        result2TrackUri: result2Value.trackUri,
      });
    }
  });

  // Find items in result2 but not in result1
  map2.forEach((value, id) => {
    if (!map1.has(id)) {
      missingInResult1.push({id, ts: value.ts, trackUri: value.trackUri});
    }
  });

  return {
    type:
      missingInResult2.length > 0 || missingInResult1.length > 0
        ? 'count'
        : 'content',
    result1Count: result1.length,
    result2Count: result2.length,
    missingInResult2,
    missingInResult1,
    different,
  };
}

/**
 * Checks if two sets of search results are identical.
 *
 * @param result1 - First set of search results
 * @param result2 - Second set of search results
 * @returns True if results are identical, false otherwise
 */
export function searchResultsAreEqual(
  result1: SearchResultEvent[],
  result2: SearchResultEvent[],
): boolean {
  const diff = compareSearchResults(result1, result2);
  return (
    diff.missingInResult2.length === 0 &&
    diff.missingInResult1.length === 0 &&
    diff.different.length === 0
  );
}

/**
 * Formats search result differences as a human-readable string.
 *
 * @param diff - The difference information
 * @param result1Name - Name for the first result set (e.g., "SQL Search")
 * @param result2Name - Name for the second result set (e.g., "Dataset Search")
 * @returns Formatted string describing the differences
 */
export function formatSearchResultDifference(
  diff: SearchResultDifference,
  result1Name: string = 'Result 1',
  result2Name: string = 'Result 2',
): string {
  if (
    diff.missingInResult2.length === 0 &&
    diff.missingInResult1.length === 0 &&
    diff.different.length === 0
  ) {
    return 'Results are identical ✓';
  }

  let output = 'Results differ ✗\n\n';

  if (diff.result1Count !== diff.result2Count) {
    output += `Result count mismatch: ${result1Name} found ${diff.result1Count} results, ${result2Name} found ${diff.result2Count} results\n\n`;
  }

  if (diff.missingInResult2.length > 0) {
    output += `${diff.missingInResult2.length} results in ${result1Name} but not in ${result2Name}:\n`;
    diff.missingInResult2.slice(0, 5).forEach((item) => {
      output += `  ID: ${item.id}, TS: ${item.ts}, Track: ${item.trackUri}\n`;
    });
    if (diff.missingInResult2.length > 5) {
      output += `  ...and ${diff.missingInResult2.length - 5} more\n`;
    }
    output += '\n';
  }

  if (diff.missingInResult1.length > 0) {
    output += `${diff.missingInResult1.length} results in ${result2Name} but not in ${result1Name}:\n`;
    diff.missingInResult1.slice(0, 5).forEach((item) => {
      output += `  ID: ${item.id}, TS: ${item.ts}, Track: ${item.trackUri}\n`;
    });
    if (diff.missingInResult1.length > 5) {
      output += `  ...and ${diff.missingInResult1.length - 5} more\n`;
    }
    output += '\n';
  }

  if (diff.different.length > 0) {
    output += `${diff.different.length} results with differences:\n`;

    diff.different.slice(0, 5).forEach((item) => {
      output += `  ID: ${item.id}\n`;
      output += `    ${result1Name}: TS=${item.result1Ts}, Track=${item.result1TrackUri}\n`;
      output += `    ${result2Name}: TS=${item.result2Ts}, Track=${item.result2TrackUri}\n`;
    });
    if (diff.different.length > 5) {
      output += `  ...and ${diff.different.length - 5} more\n`;
    }
  }

  return output;
}

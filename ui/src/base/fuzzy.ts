// Copyright (C) 2023 The Android Open Source Project
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

import fuzzysort from 'fuzzysort';

export interface FuzzySegment {
  readonly matching: boolean;
  readonly value: string;
}

export interface FuzzyResult<T> {
  readonly item: T;
  readonly segments: readonly FuzzySegment[];
  // Relevance score of the match, higher is better. Exact matches score higher
  // than fuzzy ones. Empty search terms produce a uniform score of 1.
  readonly score: number;
}

export interface FuzzyMultiResult<T> {
  readonly item: T;
  readonly segments: readonly (readonly FuzzySegment[])[];
  // Relevance score of the match, higher is better. Exact matches score higher
  // than fuzzy ones. Empty search terms produce a uniform score of 1.
  readonly score: number;
}

export type KeyLookup<T> = (x: T) => string;

/**
 * Performs character-subsequence fuzzy matching over a list of items using
 * fuzzysort.
 *
 * Accepts either a single key lookup function or an array of key lookup
 * functions. When multiple key lookups are provided, matches across any key
 * will be returned with per-key highlight segments and a unified match score.
 *
 * An empty `searchTerm` returns all items in original order with uniform scores
 * of 1.
 *
 * @param items List of candidate items to search through.
 * @param keyLookup Single key extraction function or array of key extraction
 * functions.
 * @param searchTerm Search query text to match against item keys.
 * @returns Array of matching items ordered by relevance score, with highlight
 * segments and match scores.
 */
export function fuzzySearch<T>(
  items: readonly T[],
  keyLookup: readonly KeyLookup<T>[],
  searchTerm: string,
): FuzzyMultiResult<T>[];
export function fuzzySearch<T>(
  items: readonly T[],
  keyLookup: KeyLookup<T>,
  searchTerm: string,
): FuzzyResult<T>[];
export function fuzzySearch<T>(
  items: readonly T[],
  keyLookup: KeyLookup<T> | readonly KeyLookup<T>[],
  searchTerm: string,
): FuzzyMultiResult<T>[] | FuzzyResult<T>[] {
  if (typeof keyLookup === 'function') {
    // Single key: delegate to multi-key search and unwrap segments.
    const result = fuzzySearchMultiKey(items, [keyLookup], searchTerm);
    return result.map((res) => ({
      item: res.item,
      segments: res.segments[0],
      score: res.score,
    }));
  } else {
    // Multi-key: return multi-key search results directly.
    return fuzzySearchMultiKey(items, keyLookup, searchTerm);
  }
}

function fuzzySearchMultiKey<T>(
  items: readonly T[],
  keyLookup: readonly KeyLookup<T>[],
  searchTerm: string,
): FuzzyMultiResult<T>[] {
  if (searchTerm === '') {
    return items.map((item) => {
      const segments = keyLookup.map((lookup) => [
        {matching: false, value: lookup(item)},
      ]);
      return {item, segments, score: 1};
    });
  }

  const results = fuzzysort.go(searchTerm, items, {
    keys: keyLookup,
  });

  return results.map((result) => {
    const segments = keyLookup.map((lookup, i) => {
      const text = lookup(result.obj);
      const keyMatch = result[i];
      const indexes =
        keyMatch?.indexes !== undefined
          ? keyMatch.indexes.toSorted((a, b) => a - b)
          : [];
      return indiciesToSegments(indexes, text);
    });
    return {
      item: result.obj,
      segments,
      score: result.score,
    };
  });
}

// Given a list of indicies of matching chars, and the original value, produce
// a list of match/nomatch segments.
function indiciesToSegments(indicies: number[], text: string): FuzzySegment[] {
  const segments: FuzzySegment[] = [];
  let nextIndex = 0;
  let match = '';
  for (const i of indicies) {
    if (nextIndex < i) {
      // If we had a match segment from before, add it now.
      if (match !== '') {
        segments.push({matching: true, value: match});
        match = '';
      }
      // Missed some indicies - wrap them up into a nomatch segment.
      segments.push({matching: false, value: text.slice(nextIndex, i)});
    }

    // Append this matching char to the previous match.
    match += text[i];
    nextIndex = i + 1;
  }

  // Add any lingering match segment.
  if (match !== '') {
    segments.push({matching: true, value: match});
  }

  // Add final nomatch segment if there is anything left.
  if (nextIndex < text.length) {
    segments.push({matching: false, value: text.slice(nextIndex)});
  }

  return segments;
}

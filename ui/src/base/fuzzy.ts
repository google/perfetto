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
import m from 'mithril';

export interface FuzzySegment {
  readonly matching: boolean;
  readonly value: string;
}

export function renderSegments(
  text: readonly FuzzySegment[] | string,
): m.Children {
  if (typeof text === 'string') {
    return text;
  }
  return text.map(({matching, value}) => (matching ? m('b', value) : value));
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

export interface IFuzzyFinderConstructor {
  new <T>(
    items: readonly T[],
    keyLookups: readonly KeyLookup<T>[],
  ): FuzzyFinder<T, FuzzyMultiResult<T>>;
  new <T>(
    items: readonly T[],
    keyLookup: KeyLookup<T>,
  ): FuzzyFinder<T, FuzzyResult<T>>;
}

// Finds approx matching in arbitrary lists of items.
// Uses fuzzysort for character-subsequence fuzzy matching and highlight segments.
class FuzzyFinderImpl<
  T,
  R extends FuzzyResult<T> | FuzzyMultiResult<T> = FuzzyResult<T>,
> {
  private readonly keyLookups: readonly KeyLookup<T>[];
  private readonly isMulti: boolean;

  constructor(
    private readonly items: readonly T[],
    keyLookup: KeyLookup<T> | readonly KeyLookup<T>[],
  ) {
    if (Array.isArray(keyLookup)) {
      this.keyLookups = keyLookup;
      this.isMulti = true;
    } else {
      this.keyLookups = [keyLookup as KeyLookup<T>];
      this.isMulti = false;
    }
  }

  // Return a list of items that match the search term.
  find(searchTerm: string): R[] {
    if (searchTerm === '') {
      if (this.isMulti) {
        const res: FuzzyMultiResult<T>[] = this.items.map((item) => {
          const segments = this.keyLookups.map((lookup) => [
            {matching: false, value: lookup(item)},
          ]);
          return {item, segments, score: 1};
        });
        return res as R[];
      } else {
        const res: FuzzyResult<T>[] = this.items.map((item) => {
          const text = this.keyLookups[0](item);
          return {item, segments: [{matching: false, value: text}], score: 1};
        });
        return res as R[];
      }
    }

    if (this.isMulti) {
      const results = fuzzysort.go(searchTerm, this.items, {
        keys: this.keyLookups,
      });

      const res: FuzzyMultiResult<T>[] = results.map((result) => {
        const segments = this.keyLookups.map((lookup, i) => {
          const text = lookup(result.obj);
          const keyMatch = result[i];
          const indexes =
            keyMatch?.indexes !== undefined
              ? Array.from(keyMatch.indexes).sort((a, b) => a - b)
              : [];
          return indiciesToSegments(indexes, text);
        });
        return {
          item: result.obj,
          segments,
          score: result.score,
        };
      });
      return res as R[];
    } else {
      const results = fuzzysort.go(searchTerm, this.items, {
        key: this.keyLookups[0],
      });

      const res: FuzzyResult<T>[] = results.map((result) => {
        const text = this.keyLookups[0](result.obj);
        return {
          item: result.obj,
          segments: indiciesToSegments(
            Array.from(result.indexes).sort((a, b) => a - b),
            text,
          ),
          score: result.score,
        };
      });
      return res as R[];
    }
  }
}

export const FuzzyFinder: IFuzzyFinderConstructor = FuzzyFinderImpl;
export type FuzzyFinder<
  T,
  R extends FuzzyResult<T> | FuzzyMultiResult<T> = FuzzyResult<T>,
> = FuzzyFinderImpl<T, R>;

// Given a query (possibly multi-word) and candidate text, compute highlight
// segments using fuzzysort matching.
export function computeHighlightSegments(
  query: string,
  text: string,
): readonly FuzzySegment[] {
  if (!query) {
    return [{matching: false, value: text}];
  }

  const res = fuzzysort.single(query, text);
  if (res) {
    const sortedIndexes = Array.from(res.indexes).sort((a, b) => a - b);
    return indiciesToSegments(sortedIndexes, text);
  }

  // Fallback for multi-word queries where words are separated by spaces.
  const tokens = query.split(/\s+/).filter(Boolean);
  const highlightedIndices: number[] = [];
  for (const token of tokens) {
    const tokenRes = fuzzysort.single(token, text);
    if (tokenRes) {
      highlightedIndices.push(...tokenRes.indexes);
    }
  }

  if (highlightedIndices.length > 0) {
    const unique = [...new Set(highlightedIndices)].sort((a, b) => a - b);
    return indiciesToSegments(unique, text);
  }

  return [{matching: false, value: text}];
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

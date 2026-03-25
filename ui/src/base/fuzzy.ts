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

import MiniSearch from 'minisearch';

export interface FuzzySegment {
  matching: boolean;
  value: string;
}

export interface FuzzyResult<T> {
  item: T;
  segments: FuzzySegment[];
}

export type KeyLookup<T> = (x: T) => string;

// Finds approx matching in arbitrary lists of items.
// Uses MiniSearch for word-order-independent fuzzy filtering, then computes
// character-level highlight segments for rendering.
export class FuzzyFinder<T> {
  private readonly miniSearch: MiniSearch;

  // Because we operate on arbitrary lists, a key lookup function is required to
  // so we know which part of the list is to be be searched. It should return
  // the relevant search string for each item.
  constructor(
    private readonly items: ReadonlyArray<T>,
    private readonly keyLookup: KeyLookup<T>,
  ) {
    const docs = items.map((item, i) => ({id: i, text: keyLookup(item)}));
    this.miniSearch = new MiniSearch({
      fields: ['text'],
      tokenize: camelCaseTokenize,
      searchOptions: {
        tokenize: camelCaseTokenize,
        // Allow 1 edit for short terms, ~20% for longer ones.
        fuzzy: (term: string) =>
          term.length <= 3 ? 1 : Math.ceil(term.length * 0.2),
        prefix: true,
        combineWith: 'AND',
      },
    });
    this.miniSearch.addAll(docs);
  }

  // Return a list of items that match any of the search terms.
  // Search terms separated by spaces are matched independently (any order).
  find(searchTerm: string): FuzzyResult<T>[] {
    if (searchTerm === '') {
      return this.items.map((item) => {
        const normalisedTerm = this.keyLookup(item);
        return {
          item,
          segments: [{matching: false, value: normalisedTerm}],
        };
      });
    }
    return this.miniSearch.search(searchTerm).map((result) => {
      const item = this.items[result.id as number];
      const text = this.keyLookup(item);
      return {item, segments: computeHighlightSegments(searchTerm, text)};
    });
  }
}

// Tokenize text by splitting on whitespace/punctuation AND camelCase boundaries.
// E.g. "dev.perfetto.LiveMemory" -> ["dev", "perfetto", "Live", "Memory"]
// This allows searching for "memory" to match "LiveMemory".
function camelCaseTokenize(text: string): string[] {
  // First split on non-alphanumeric characters (dots, spaces, underscores, etc.)
  const coarseTokens = text.split(/[^a-zA-Z0-9]+/).filter(Boolean);
  const tokens: string[] = [];
  for (const token of coarseTokens) {
    // Split camelCase: insert boundary before uppercase letter preceded by
    // a lowercase letter, or before an uppercase letter followed by a
    // lowercase letter when preceded by uppercase (e.g. "XMLParser" ->
    // ["XML", "Parser"]).
    const parts = token.split(/(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])/);
    tokens.push(...parts);
  }
  return tokens;
}

// Given a query (possibly multi-word) and candidate text, compute highlight
// segments. Each query token is first tried as a substring match, then falls
// back to sequential character matching.
function computeHighlightSegments(query: string, text: string): FuzzySegment[] {
  const tokens = query.split(/\s+/).filter(Boolean);
  const lowerText = text.toLowerCase();
  const highlightedIndices: number[] = [];

  for (const token of tokens) {
    const lowerToken = token.toLowerCase();
    const idx = lowerText.indexOf(lowerToken);
    if (idx !== -1) {
      // Substring match — highlight the whole substring.
      for (let i = idx; i < idx + lowerToken.length; i++) {
        highlightedIndices.push(i);
      }
    } else {
      // Fall back to sequential character match.
      let j = 0;
      for (const ch of lowerToken) {
        while (j < lowerText.length && lowerText[j] !== ch) j++;
        if (j < lowerText.length) {
          highlightedIndices.push(j);
          j++;
        }
      }
    }
  }

  const unique = [...new Set(highlightedIndices)].sort((a, b) => a - b);
  return indiciesToSegments(unique, text);
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

// Evaluate whether |searchTerm| is found in |text|.
// |indicies| is an array of numbers the same length as |searchTerm|, into which
// we place the indicies of the matching chars in |text|.
function match(searchTerm: string, text: string, indicies: number[]): boolean {
  let j = 0; // index into the searchTerm.
  let success: boolean = true;

  // For each char of the searchTerm...
  for (let i = 0; i < searchTerm.length; ++i) {
    const char = searchTerm[i].toLowerCase();
    // ...advance the text index until we find the char.
    for (; j < text.length; ++j) {
      // If we find it add it to the segment and move on.
      if (text[j].toLowerCase() === char) {
        indicies[i] = j;
        break;
      }
    }

    // Failed to find searchTerm[i] in text: give up.
    if (j === text.length) {
      success = false;
      break;
    }

    ++j;
  }

  return success;
}

export interface FuzzyMatch {
  matches: boolean;
  segments: FuzzySegment[];
}

// Fuzzy match a single piece of text against several search terms.
// If any of the terms match, the result of the match is true.
export function fuzzyMatch(
  text: string,
  ...searchTerms: ReadonlyArray<string>
): FuzzyMatch {
  for (const searchTerm of searchTerms) {
    const indicies: number[] = new Array(searchTerm.length);
    if (match(searchTerm, text, indicies)) {
      const segments = indiciesToSegments(indicies, text);
      return {
        matches: true,
        segments,
      };
    }
  }

  return {
    matches: false,
    segments: [],
  };
}

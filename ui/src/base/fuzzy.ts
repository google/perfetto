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
export class FuzzyFinder<T> {
  private readonly items: ReadonlyArray<T>;
  private readonly keyLookup: KeyLookup<T>;

  // Because we operate on arbitrary lists, a key lookup function is required to
  // so we know which part of the list is to be be searched. It should return
  // the relevant search string for each item.
  constructor(items: ReadonlyArray<T>, keyLookup: KeyLookup<T>) {
    this.items = items;
    this.keyLookup = keyLookup;
  }

  // Return a list of items that match any of the search terms.
  find(...searchTerms: string[]): FuzzyResult<T>[] {
    const result: FuzzyResult<T>[] = [];

    for (const item of this.items) {
      const key = this.keyLookup(item);
      for (const searchTerm of searchTerms) {
        const indicies: number[] = new Array(searchTerm.length);
        if (match(searchTerm, key, indicies)) {
          const segments = indiciesToSegments(indicies, key);
          result.push({item, segments});

          // Don't try to match any more...
          break;
        }
      }
    }

    return result;
  }
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

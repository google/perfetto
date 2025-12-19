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

// A function similar to Python's `range`.
export function range(n: number): number[] {
  if (n < 0) {
    throw new Error('range size should be non-negative!');
  }

  const result = new Array<number>(n);

  for (let i = 0; i < n; i++) {
    result[i] = i;
  }

  return result;
}

// Checks whether all the strings in the array are unique.
export function allUnique(x: string[]): boolean {
  return x.length == new Set(x).size;
}

// Check whether two arrays are identical.
export function arrayEquals<T>(a: ArrayLike<T>, b: ArrayLike<T>): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) return false;
  }
  return true;
}

export function isArrayOf<P, Q>(
  predicate: (x: P | Q) => x is P,
  xs: (P | Q)[],
): xs is P[] {
  return xs.every(predicate);
}

// Filter out falsy values from an array, leaving only the truthy ones
export function removeFalsyValues<T>(
  array: ReadonlyArray<T | false | null | undefined>,
): T[] {
  return array.filter(Boolean) as T[];
}

// Move an item from a given index in the array (`from`) to a new index (`to`).
// `from`: index in the current array in [0, len(array) - 1] range.
// `to`: new location, in [0, len(array)] range. The element will be inserted
//       in the position before current element at `to` index.
export function moveArrayItem<T>(array: T[], from: number, to: number) {
  if (from === to) return;
  const value = array[from];
  array.splice(from, 1);
  if (from < to) {
    // We have deleted an item, therefore we need to adjust the target index.
    --to;
  }
  array.splice(to, 0, value);
}

// If all values in the array are the same, returns that value. Otherwise
// returns undefined. If the array is empty, returns undefined.
export function valueIfAllEqual<T>(arr: ReadonlyArray<T>): T | undefined {
  if (arr.length === 0) return undefined;
  if (arr.every((val) => val === arr[0])) return arr[0];
  return undefined;
}

// Inserts a separator between each element of an array. Similar to
// Array.join(), but returns an array instead of a string.
export function intersperse<T, S>(arr: T[], separator: S): (T | S)[] {
  return arr.flatMap((item, i) =>
    i < arr.length - 1 ? [item, separator] : [item],
  );
}

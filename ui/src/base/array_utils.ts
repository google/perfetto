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

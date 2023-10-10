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

export type PathKey = string|number;
export type Path = PathKey[];

// Given an object, return a ref to the object or item at at a given path.
// A path is defined using an array of path-like elements: I.e. [string|number].
// Returns undefined if the path doesn't exist.
export function lookupPath<SubT, T>(value: T, path: Path): SubT|undefined {
  let o: any = value;
  for (const p of path) {
    if (p in o) {
      o = o[p];
    } else {
      return undefined;
    }
  }
  return o;
}

export function shallowEquals(a: any, b: any) {
  if (a === b) {
    return true;
  }
  if (a === undefined || b === undefined) {
    return false;
  }
  for (const key of Object.keys(a)) {
    if (a[key] !== b[key]) {
      return false;
    }
  }
  for (const key of Object.keys(b)) {
    if (a[key] !== b[key]) {
      return false;
    }
  }
  return true;
}

export function isString(s: unknown): s is string {
  return typeof s === 'string' || s instanceof String;
}

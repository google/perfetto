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

export function union<T>(xs: Set<T>, ys: Set<T>): Set<T> {
  if (xs.size === 0) {
    return ys;
  }
  if (ys.size === 0) {
    return xs;
  }
  const result = new Set<T>();
  for (const x of xs) {
    result.add(x);
  }
  for (const y of ys) {
    result.add(y);
  }
  return result;
}

export function intersect<T>(xs: Set<T>, ys: Set<T>): Set<T> {
  if (xs.size === 0) {
    return xs;
  }
  if (ys.size === 0) {
    return ys;
  }
  const result = new Set<T>();
  for (const x of xs) {
    if (ys.has(x)) {
      result.add(x);
    }
  }
  return result;
}

export function isSetEqual<T>(xs: Set<T>, ys: Set<T>): boolean {
  if (xs === ys) {
    return true;
  }
  if (xs.size !== ys.size) {
    return false;
  }
  for (const x of xs) {
    if (!ys.has(x)) {
      return false;
    }
  }
  return true;
}

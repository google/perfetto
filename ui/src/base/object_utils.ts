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

export function shallowEquals(a: unknown, b: unknown) {
  if (a === b) {
    return true;
  }
  if (a === undefined || b === undefined) {
    return false;
  }
  if (a === null || b === null) {
    return false;
  }
  const objA = a as {[_: string]: {}};
  const objB = b as {[_: string]: {}};
  for (const key of Object.keys(objA)) {
    if (objA[key] !== objB[key]) {
      return false;
    }
  }
  for (const key of Object.keys(objB)) {
    if (objA[key] !== objB[key]) {
      return false;
    }
  }
  return true;
}

export function isString(s: unknown): s is string {
  return typeof s === 'string' || s instanceof String;
}

// Given a string enum |enum|, check that |value| is a valid member of |enum|.
export function isEnumValue<T extends {}>(
  enm: T,
  value: unknown,
): value is T[keyof T] {
  return Object.values(enm).includes(value);
}

/**
 * Recursively freezes an object and all its properties to prevent mutation.
 */
export function deepFreeze<T>(obj: T): T {
  if (obj === null || typeof obj !== 'object') return obj;
  Object.freeze(obj);
  Object.values(obj).forEach(deepFreeze);
  return obj;
}

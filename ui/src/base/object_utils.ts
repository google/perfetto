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

import {assertExists} from './logging';
import {exists} from './utils';

export type PathKey = string | number;
export type Path = PathKey[];

/**
 * Gets the |value| at a |path| of |object|. If a portion of the path doesn't
 * exist, |undefined| is returned.
 *
 * Example:
 * const obj = {
 *   a: [
 *     {b: 'c'},
 *     {d: 'e', f: 123},
 *   ],
 * };
 * getPath(obj, ['a']) -> [{b: 'c'}, {d: 'e', f: 123}]
 * getPath(obj, ['a', 1]) -> {d: 'e', f: 123}
 * getPath(obj, ['a', 1, 'd']) -> 'e'
 * getPath(obj, ['g']) -> undefined
 * getPath(obj, ['g', 'h']) -> undefined
 *
 * Note: This is an appropriate use of `any`, as we are knowingly getting fast
 * and loose with the type system in this function: it's basically JavaScript.
 * Attempting to pretend it's anything else would result in superfluous type
 * assertions which would serve no benefit.
 */
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function getPath<T>(obj: any, path: Path): T | undefined {
  let x = obj;
  for (const node of path) {
    if (x === undefined) return undefined;
    x = x[node];
  }
  return x;
}

/**
 * Sets the |value| at |path| of |object|. If the final node of the path doesn't
 * exist, the value will be created. Otherwise, TypeError is thrown.
 *
 * Example:
 * const obj = {
 *   a: [
 *     {b: 'c'},
 *     {d: 'e', f: 123},
 *   ],
 * };
 * setPath(obj, ['a'], 'foo') -> {a: 'foo'}
 * setPath(obj, ['a', 1], 'foo') -> {a: [{b: 'c'}, 'foo']}
 * setPath(obj, ['g'], 'foo') -> {a: [...], g: 'foo'}
 * setPath(obj, ['g', 'h'], 'foo') -> TypeError!
 */
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function setPath<T>(obj: any, path: Path, value: T): void {
  const pathClone = [...path];
  let o = obj;
  while (pathClone.length > 1) {
    const p = assertExists(pathClone.shift());
    o = o[p];
  }

  const p = pathClone.shift();
  if (!exists(p)) {
    throw TypeError('Path array is empty');
  }
  o[p] = value;
}

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

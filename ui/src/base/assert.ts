// Copyright (C) 2026 The Android Open Source Project
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

// Assertion utilities for runtime validation and TypeScript type narrowing.
//
// These functions provide fail-fast semantics: if an assertion fails, an
// exception is thrown immediately. This makes bugs easier to catch and debug
// by surfacing issues at the point of failure rather than propagating invalid
// state.
//
// In addition to runtime checks, these assertions help TypeScript narrow types.
// For example, after calling assertExists(x), TypeScript knows x is non-null.

export function assertExists<A>(
  value: A | null | undefined,
  optMsg?: string,
): A {
  if (value === null || value === undefined) {
    throw new Error(optMsg ?? 'Value is null or undefined');
  }
  return value;
}

// assertExists trips over NULLs, but in many contexts NULL is a valid SQL value
// we have to work with.
export function assertDefined<T>(value: T | undefined, optMsg?: string): T {
  if (value === undefined) {
    throw new Error(optMsg ?? 'Value is undefined');
  }
  return value;
}

export function assertIsInstance<T>(
  value: unknown,
  clazz: Function,
  optMsg?: string,
): T {
  assertTrue(
    value instanceof clazz,
    optMsg ?? `Value is not an instance of ${clazz.name}`,
  );
  return value as T;
}

export function assertTrue(value: boolean, optMsg?: string) {
  if (!value) {
    throw new Error(optMsg ?? 'Failed assertion');
  }
}

export function assertFalse(value: boolean, optMsg?: string) {
  assertTrue(!value, optMsg);
}

// This function serves two purposes.
// 1) A runtime check - if we are ever called, we throw an exception.
// This is useful for checking that code we suspect should never be reached is
// actually never reached.
// 2) A compile time check where typescript asserts that the value passed can be
// cast to the "never" type.
// This is useful for ensuring we exhaustively check union types.
export function assertUnreachable(value: never, optMsg?: string): never {
  throw new Error(
    optMsg ?? `This code should not be reachable ${value as unknown}`,
  );
}

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

// Assertion and check utilities for runtime validation and TypeScript type
// narrowing.
//
// Two flavors:
// - assert*() — void. Narrows the type of the input variable in place for
//   subsequent code. Use as a standalone statement.
// - checkExists() — returns the narrowed value. Use in expressions,
//   assignments, and function arguments.
//
// All functions throw immediately on failure (fail-fast).

// Throws if |value| is null or undefined. Returns the value with null and
// undefined stripped from the type, for use in expressions.
export function checkExists<T>(value: T, msg?: string): NonNullable<T> {
  if (value === null || value === undefined) {
    throw new Error(msg ?? 'Value is null or undefined');
  }
  return value;
}

// Throws if |value| is null or undefined. Narrows the type of |value| to
// exclude null and undefined for all subsequent code.
export function assertExists<T>(
  value: T,
  msg?: string,
): asserts value is NonNullable<T> {
  if (value === null || value === undefined) {
    throw new Error(msg ?? 'Value is null or undefined');
  }
}

// Throws if |value| is undefined. Narrows the type of |value| to exclude
// undefined for all subsequent code. Unlike assertExists(), this permits null.
export function assertDefined<T>(
  value: T | undefined,
  msg?: string,
): asserts value is T {
  if (value === undefined) throw new Error(msg ?? 'Value is undefined');
}

// Throws if |value| is not an instance of |cls|. Narrows the type of |value|
// to |T| for all subsequent code.
export function assertInstanceOf<T>(
  value: unknown,
  cls: abstract new (...args: never[]) => T,
  msg?: string,
): asserts value is T {
  if (!(value instanceof cls)) {
    throw new Error(msg ?? `Value is not instance of '${cls.name}'`);
  }
}

export function checkInstanceOf<T>(
  value: unknown,
  cls: abstract new (...args: never[]) => T,
  msg?: string,
): T {
  if (!(value instanceof cls)) {
    throw new Error(msg ?? `Value is not instance of '${cls.name}'`);
  }
  return value;
}

// Throws if |value| is not truthy.
export function assertTrue(value: boolean, msg?: string) {
  if (!value) throw new Error(msg ?? 'Value is not truthy');
}

// Throws if |value| is truthy.
export function assertFalse(value: boolean, msg?: string) {
  if (value) throw new Error(msg ?? 'Value is not falsy');
}

// Throws unconditionally at runtime. At compile time, requires |value| to be
// of type 'never', ensuring exhaustive checks of union types and enums.
export function assertUnreachable(value: never, msg?: string): never {
  throw new Error(
    msg ?? `This code should not be reachable ${value as unknown}`,
  );
}

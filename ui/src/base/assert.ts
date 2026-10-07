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

type Nullable<T> = T | undefined | null; // T or nullish (null or undefined)
type Maybe<T> = T | undefined; // T or undefined
type Falsy = false | 0 | -0 | 0n | '' | null | undefined; // All falsy types

// Asserts that x is truthy, throwing at runtime otherwise. The `asserts x`
// return annotation narrows x in the caller, stripping falsy members from its
// type (e.g. `string | undefined` becomes `string`) for the rest of the scope.
export function assertTrue(x: unknown, msg?: string): asserts x {
  if (!Boolean(x)) {
    throw new Error(msg ?? 'Failed assertion');
  }
}

// Asserts that x is falsy, throwing at runtime otherwise. The `asserts x is
// Extract<T, Falsy>` annotation narrows x in the caller to just the falsy
// members of its type for the rest of the scope.
export function assertFalse<T>(
  x: T,
  msg?: string,
): asserts x is Extract<T, Falsy> {
  if (Boolean(x)) {
    throw new Error(msg ?? 'Failed assertion');
  }
}

// Throws at runtime if x is null or undefined, otherwise returns x with its
// type narrowed to non-nullable. Use this when you need the value as an
// expression (e.g. `const y = ensureExists(maybeY)`).
export function ensureExists<T>(x: Nullable<T>, msg?: string): T {
  if (x === null || x === undefined) {
    throw new Error(msg ?? 'Value is null or undefined');
  }
  return x;
}

// Asserts that x is neither null nor undefined, throwing at runtime otherwise.
// The `asserts x is T` annotation narrows x to non-nullable in the caller for
// the rest of the scope. Use this when x is already a variable you can keep
// using; prefer ensureExists when you need the value as an expression.
export function assertExists<T>(x: Nullable<T>, msg?: string): asserts x is T {
  if (x === null || x === undefined) {
    throw new Error(msg ?? 'Value is null or undefined');
  }
}

// Like ensureExists, but only undefined is rejected; null is allowed through.
// ensureExists/assertExists trip over nulls, but in many contexts null is a
// valid SQL value we have to work with.
export function ensureDefined<T>(x: Maybe<T>, msg?: string): T {
  if (x === undefined) {
    throw new Error(msg ?? 'Value is undefined');
  }
  return x;
}

// Like assertExists, but only undefined is rejected; null is allowed through.
// Narrows x to exclude undefined in the caller for the rest of the scope.
export function assertDefined<T>(x: Maybe<T>, msg?: string): asserts x is T {
  if (x === undefined) {
    throw new Error(msg ?? 'Value is undefined');
  }
}

// Throws at runtime unless x is an instance of clazz, otherwise returns x typed
// as that class. Use this when you need the value as an expression; prefer
// assertIsInstance when x is already a variable you can keep using.
export function ensureIsInstance<T>(
  x: unknown,
  clazz: abstract new (...args: never[]) => T,
  msg?: string,
): T {
  if (!(x instanceof clazz)) {
    throw new Error(msg ?? `Value is not an instance of ${clazz.name}`);
  }
  return x;
}

// Asserts that x is an instance of clazz, throwing at runtime otherwise. The
// `asserts x is T` annotation narrows x to that class in the caller for the
// rest of the scope.
export function assertIsInstance<T>(
  x: unknown,
  clazz: abstract new (...args: never[]) => T,
  msg?: string,
): asserts x is T {
  if (!(x instanceof clazz)) {
    throw new Error(msg ?? `Value is not an instance of ${clazz.name}`);
  }
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

/**
 * In TS 5.7, the TypedArrays and other array view and wrapper types were made
 * to be generic, in order to distinguish those backed by ArrayBuffers vs
 * SharedArrayBuffers, defaulting to a union of these two if the type is lefe
 * unspecified (ArrayBufferLike). This is a good thing in general as the two
 * buffer types have differing interfaces, however the main problem with this is
 * that a lot of libraries have not been updated to reflect which buffer type
 * they actually use and return.
 *
 * @see https://github.com/microsoft/TypeScript/issues/60579
 */
export function assertIsArrayBufferView(
  view: Uint8Array<ArrayBufferLike>,
): asserts view is Uint8Array<ArrayBuffer> {
  // SharedArrayBuffer is only defined in cross-origin isolated contexts; if the
  // global isn't there, the buffer can't possibly be one.
  if (
    typeof globalThis.SharedArrayBuffer !== 'undefined' &&
    view.buffer instanceof globalThis.SharedArrayBuffer
  ) {
    // Copy the underlying buffer into the array, trimming to the byte bounds of the view
    throw new Error(
      "Underlying view is a SAB. If this is a problem we could convert it but we're being defensive as this could be expensive",
    );
  }
}

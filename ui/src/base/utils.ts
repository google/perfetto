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

// Return true if value is not nullish - i.e. not null or undefined
// Allows doing the following
//   exists(val) && m('div', val)
// Even if val is a non-nullish falsey value like 0 or ''
export function exists<T>(value: T): value is NonNullable<T> {
  return value !== undefined && value !== null;
}

// Generic result type - similar to Rust's Result<T, E>
export type Result<T, E = {}> =
  | {success: true; result: T}
  | {success: false; error: E};

// Type util to make sure that exactly one of the passed keys is defined.
// Example usage:
// type FooOrBar = ExactlyOne<{foo: number; bar: number}>;
// const x : FooOrBar = {foo: 42};      // OK
// const x : FooOrBar = {bar: 42};      // OK
// const x : FooOrBar = {};             // Compiler error
// const x : FooOrBar = {foo:1, bar:2}; // Compiler error
export type ExactlyOne<T, K extends keyof T = keyof T> = K extends keyof T
  ? {[P in K]: T[P]} & {[P in Exclude<keyof T, K>]?: undefined}
  : never;

// Escape characters that are not allowed inside a css selector
export function escapeCSSSelector(selector: string): string {
  return selector.replace(/([!"#$%&'()*+,.\/:;<=>?@[\\\]^`{|}~])/g, '\\$1');
}

// Make field K required in T
export type RequiredField<T, K extends keyof T> = Omit<T, K> &
  Required<Pick<T, K>>;

// The lowest common denoninator between Map<> and WeakMap<>.
// This is just to avoid duplication of the getOrCreate below.
interface MapLike<K, V> {
  get(key: K): V | undefined;
  set(key: K, value: V): this;
}

export function getOrCreate<K, V>(
  map: MapLike<K, V>,
  key: K,
  factory: () => V,
): V {
  let value = map.get(key);
  if (value !== undefined) return value;
  value = factory();
  map.set(key, value);
  return value;
}

// Allows to take an existing class instance (`target`) and override some of its
// methods via `overrides`. We use this for cases where we want to expose a
// "manager" (e.g. TrackManager, SidebarManager) to the plugins, but we want to
// override few of its methods (e.g. to inject the pluginId in the args).
export function createProxy<T extends object>(
  target: T,
  overrides: Partial<T>,
): T {
  return new Proxy(target, {
    get: (target: T, prop: string | symbol, receiver) => {
      // If the property is overriden, use that; otherwise, use target
      const overrideValue = (overrides as {[key: symbol | string]: {}})[prop];
      if (overrideValue !== undefined) {
        return typeof overrideValue === 'function'
          ? overrideValue.bind(overrides)
          : overrideValue;
      }
      const baseValue = Reflect.get(target, prop, receiver);
      return typeof baseValue === 'function'
        ? baseValue.bind(target)
        : baseValue;
    },
  }) as T;
}

// Copyright (C) 2018 The Android Open Source Project
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

import {assertExists, assertFalse} from './logging';

export interface HasKind {
  kind: string;
}

export class RegistryError extends Error {
  constructor(message?: string) {
    super(message);
    this.name = this.constructor.name;
  }
}

export class Registry<T> {
  private key: (t: T) => string;
  protected registry: Map<string, T>;
  private _keyFilter?: (key: string) => boolean;
  private readonly keyFilter = (key: string) => this._keyFilter?.(key) ?? true;

  static kindRegistry<T extends HasKind>(): Registry<T> {
    return new Registry<T>((t) => t.kind);
  }

  constructor(key: (t: T) => string) {
    this.registry = new Map<string, T>();
    this.key = key;
  }

  /**
   * Set a filter to allow services to be registered only under certain matching keys.
   * This is intended for applications embedding the Perfetto UI to exclude services
   * that are inappropriate or otherwise unwanted in their contexts. Initially, a
   * registry has no filter.
   *
   * **Note** that a filter may only be set once. An attempt to replace or clear the
   * filter will throw an error.
   */
  setFilter(filter: (key: string) => boolean): void {
    assertFalse(this._keyFilter !== undefined, 'A key filter is already set.');
    this._keyFilter = assertExists(filter);

    // Run the filter to knock out anything already registered that does not pass it
    [...this.registry.keys()]
      .filter((key) => !filter(key))
      .forEach((key) => this.registry.delete(key));
  }

  register(registrant: T): Disposable {
    const kind = this.key(registrant);
    if (!this.keyFilter(kind)) {
      // Simply refuse to register the entry
      return {
        [Symbol.dispose]: () => undefined,
      };
    }
    if (this.registry.has(kind)) {
      throw new RegistryError(
        `Registrant ${kind} already exists in the registry`,
      );
    }
    this.registry.set(kind, registrant);

    return {
      [Symbol.dispose]: () => this.registry.delete(kind),
    };
  }

  has(kind: string): boolean {
    return this.registry.has(kind);
  }

  get(kind: string): T {
    const registrant = this.registry.get(kind);
    if (registrant === undefined) {
      throw new RegistryError(`${kind} has not been registered.`);
    }
    return registrant;
  }

  tryGet(kind: string): T | undefined {
    return this.registry.get(kind);
  }

  // Support iteration: for (const foo of fooRegistry.values()) { ... }
  *values() {
    yield* this.registry.values();
  }

  valuesAsArray(): ReadonlyArray<T> {
    return Array.from(this.values());
  }

  unregisterAllForTesting(): void {
    this.registry.clear();
  }

  createChild(): Registry<T>;
  createChild(id: string): Registry<T> & {id: string};
  createChild(id?: string): Registry<T> & {id?: string} {
    // A proxy is not sufficient because we need non-overridden
    // methods to delegate to overridden methods.
    const result = new (class ChildRegistry extends Registry<T> {
      constructor(private readonly parent: Registry<T>) {
        super(parent.key);
      }

      override setFilter(filter: (key: string) => boolean): void {
        // Dyamically delegate to whatever the parent filter is at the
        // time of filtering
        const parentFilter = this.parent.keyFilter.bind(this.parent);
        const combinedFilter = (key: string) =>
          filter(key) && parentFilter(key);

        super.setFilter(combinedFilter);
      }

      override has(kind: string): boolean {
        return (
          this.keyFilter(kind) &&
          (this.registry.has(kind) || this.parent.has(kind))
        );
      }

      override get(kind: string): T {
        if (!this.keyFilter(kind)) {
          return super.get(kind); // This will throw a consistent Error type
        }
        return this.tryGet(kind) ?? this.parent.get(kind);
      }

      override tryGet(kind: string): T | undefined {
        return !this.keyFilter(kind)
          ? undefined
          : this.registry.get(kind) ?? this.parent.tryGet(kind);
      }

      override *values() {
        // Yield own values first
        yield* this.registry.values();

        // Then yield parent values not shadowed by my keys and that pass my filter
        for (const value of this.parent.values()) {
          const kind = this.key(value);
          if (!this.registry.has(kind) && this.keyFilter(kind)) {
            yield value;
          }
        }
      }
    })(this);

    if (id) {
      Object.defineProperty(result, 'id', {
        enumerable: true,
        value: id,
        writable: false,
        configurable: false,
      });
    }

    return result;
  }
}

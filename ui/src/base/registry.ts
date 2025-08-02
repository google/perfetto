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

  static kindRegistry<T extends HasKind>(): Registry<T> {
    return new Registry<T>((t) => t.kind);
  }

  constructor(key: (t: T) => string) {
    this.registry = new Map<string, T>();
    this.key = key;
  }

  register(registrant: T): Disposable {
    const kind = this.key(registrant);
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
      constructor (private readonly parent: Registry<T>) {
        super(parent.key);
      }

      override has(kind: string): boolean {
        return this.registry.has(kind) || this.parent.has(kind);
      }

      override get(kind: string): T {
        return this.tryGet(kind) ?? this.parent.get(kind);
      }

      override tryGet(kind: string): T | undefined {
        return this.registry.get(kind) ?? this.parent.tryGet(kind);
      }

      override *values() {
        // Yield own values first
        yield* this.registry.values();

        // Then yield parent values not shadowed by my keys
        for (const value of this.parent.values()) {
          const kind = this.key(value);
          if (!this.registry.has(kind)) {
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

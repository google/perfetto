// Copyright (C) 2025 The Android Open Source Project
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

import {z} from 'zod';
import {Memento, MementoDescriptor, MementoManager} from '../public/memento';
import {Storage} from './storage';

export const PERFETTO_MEMENTO_STORAGE_KEY = 'perfettoMemento';

function deepFreeze<T>(obj: T): T {
  if (obj === null || typeof obj !== 'object') return obj;
  Object.freeze(obj);
  Object.values(obj).forEach(deepFreeze);
  return obj;
}

class MementoImpl<T> implements Memento<T> {
  private cache?: {rawValue: unknown; normalizedValue: T};

  constructor(
    private readonly manager: MementoManagerImpl,
    public readonly pluginId: string | undefined,
    public readonly id: string,
    public readonly defaultValue: T,
    public readonly schema: z.ZodType<T>,
  ) {}

  get isDefault(): boolean {
    const currentValue = this.get();
    return JSON.stringify(currentValue) === JSON.stringify(this.defaultValue);
  }

  get(): T {
    const rawValue = this.manager.getStore()[this.id];
    const cache = this.cache;
    if (cache !== undefined && cache.rawValue === rawValue) {
      return cache.normalizedValue;
    }
    const parseResult = this.schema.safeParse(rawValue);
    const normalizedValue = deepFreeze(
      parseResult.success ? parseResult.data : this.defaultValue,
    );
    this.cache = {rawValue, normalizedValue};
    return normalizedValue;
  }

  set(newValue: T): void {
    this.manager.updateStoredValue(this.id, newValue);
  }

  reset(): void {
    this.manager.clearStoredValue(this.id);
  }

  [Symbol.dispose](): void {
    this.manager.unregister(this.id);
  }
}

export class MementoManagerImpl implements MementoManager {
  private readonly registry = new Map<string, MementoImpl<unknown>>();
  private currentStoredValues: Readonly<Record<string, unknown>> = {};
  private readonly store: Storage;

  constructor(store: Storage) {
    this.store = store;
    this.load();
  }

  register<T>(memento: MementoDescriptor<T>, pluginId?: string): Memento<T> {
    if (this.registry.has(memento.id)) {
      throw new Error(`Memento with id "${memento.id}" already registered.`);
    }

    const mementoImpl = new MementoImpl<T>(
      this,
      pluginId,
      memento.id,
      memento.defaultValue,
      memento.schema,
    );

    this.registry.set(memento.id, mementoImpl as MementoImpl<unknown>);

    return mementoImpl;
  }

  unregister(id: string): void {
    this.registry.delete(id);
  }

  resetAll(): void {
    this.currentStoredValues = {};
    this.save();
  }

  // Internal method to get the store reference (for cache invalidation)
  getStore(): Readonly<Record<string, unknown>> {
    return this.currentStoredValues;
  }

  // Internal method to update stored values
  updateStoredValue(id: string, value: unknown): void {
    this.currentStoredValues = {...this.currentStoredValues, [id]: value};
    this.save();
  }

  clearStoredValue(id: string): void {
    const {[id]: _, ...rest} = this.currentStoredValues;
    this.currentStoredValues = rest;
    this.save();
  }

  private load(): void {
    try {
      this.currentStoredValues = this.store.load();
    } catch (e) {
      console.error('Failed to load mementos from store:', e);
      this.currentStoredValues = {};
    }

    // Re-validate existing registered mementos after load
    let needsUpdate = false;
    let updatedValues = this.currentStoredValues;
    for (const memento of this.registry.values()) {
      const storedValue = updatedValues[memento.id];
      const parseResult = memento.schema.safeParse(storedValue);

      if (!parseResult.success && storedValue !== undefined) {
        updatedValues = {
          ...updatedValues,
          [memento.id]: memento.defaultValue,
        };
        needsUpdate = true;
      }
    }
    if (needsUpdate) {
      this.currentStoredValues = updatedValues;
    }

    this.save();
  }

  private save(): void {
    try {
      this.store.save(this.currentStoredValues);
    } catch (e) {
      console.error('Failed to save mementos to store:', e);
    }
  }
}

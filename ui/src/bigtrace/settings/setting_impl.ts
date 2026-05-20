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

import type {z} from 'zod';
import m from 'mithril';
import type {LocalStorage} from '../../core/local_storage';
import type {Setting, SettingDescriptor, EnumOption} from './settings_types';

// Minimal interface both LocalSettingsStorage and BigTraceSettingsStorageImpl
// satisfy. Decouples SettingImpl from a specific storage class.
export interface ValueStorage {
  getStoredValue(id: string): unknown;
  setStoredValue(id: string, value: unknown): void;
}

// Single implementation of Setting<T>, used by both LocalSettingsStorage and
// BigTraceSettingsStorageImpl. Consolidates the two prior duplicate classes.
export class SettingImpl<T> implements Setting<T> {
  public readonly id: string;
  public readonly name: string;
  public readonly description: string;
  public readonly type:
    | 'string'
    | 'number'
    | 'boolean'
    | 'enum'
    | 'multi-select'
    | 'string-array';
  public readonly schema: z.ZodType<T>;
  public readonly defaultValue: T;
  public readonly category?: string;
  public readonly requiresReload?: boolean;
  public readonly options?: readonly (string | EnumOption)[];
  public readonly placeholder?: string;
  public readonly format?: 'sql';
  public readonly disabled: boolean;

  constructor(
    private readonly storage: ValueStorage,
    descriptor: SettingDescriptor<T>,
    private readonly disabledStateStorage?: LocalStorage,
  ) {
    this.id = descriptor.id;
    this.name = descriptor.name;
    this.description = descriptor.description;
    this.type = descriptor.type ?? 'string';
    this.schema = descriptor.schema;
    this.defaultValue = descriptor.defaultValue;
    this.category = descriptor.category;
    this.requiresReload = descriptor.requiresReload;
    this.options = descriptor.options;
    this.placeholder = descriptor.placeholder;
    this.format = descriptor.format;
    this.disabled = descriptor.disabled ?? false;

    // When the backend marks a setting as disabled by default, persist that
    // on first encounter so the toggle starts in the off position.
    if (this.disabled && this.disabledStateStorage) {
      const storedState = this.disabledStateStorage.load()[this.id];
      if (storedState === undefined) {
        this.setDisabled(true);
      }
    }
  }

  get isDefault(): boolean {
    return this.get() === this.defaultValue;
  }

  get(): T {
    const storedValue = this.storage.getStoredValue(this.id);
    const parsed = this.schema.safeParse(storedValue);
    return parsed.success ? parsed.data : this.defaultValue;
  }

  set(value: T): void {
    this.storage.setStoredValue(this.id, value);
    m.redraw();
  }

  reset(): void {
    this.storage.setStoredValue(this.id, this.defaultValue);
    m.redraw();
  }

  isDisabled(): boolean {
    if (!this.disabledStateStorage) return false;
    return Boolean(this.disabledStateStorage.load()[this.id]);
  }

  setDisabled(disabled: boolean): void {
    if (!this.disabledStateStorage) return;
    const data = this.disabledStateStorage.load();
    data[this.id] = disabled;
    this.disabledStateStorage.save(data);
    m.redraw();
  }

  [Symbol.dispose](): void {
    // No resources owned — values live in LocalStorage.
  }
}

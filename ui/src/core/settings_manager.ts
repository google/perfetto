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
import {
  Setting,
  SettingDescriptor,
  SettingRenderer,
  SettingsManager,
} from '../public/settings';
import {Storage} from './storage';

export const PERFETTO_SETTINGS_STORAGE_KEY = 'perfettoSettings';

// Implement the Setting interface for registered settings
export class SettingImpl<T> implements Setting<T> {
  readonly bootValue?: T;

  constructor(
    private readonly manager: SettingsManagerImpl,
    public readonly pluginId: string | undefined,
    public readonly id: string,
    public readonly name: string,
    public readonly description: string,
    public readonly defaultValue: T,
    public readonly schema: z.ZodType<T>,
    public readonly requiresReload: boolean = false,
    public readonly render?: SettingRenderer<T>,
  ) {
    // Record what the value was at startup. This is used to determine if a
    // reload is required.
    this.bootValue = this.get();
  }

  get isDefault(): boolean {
    const currentValue = this.get();
    return JSON.stringify(currentValue) === JSON.stringify(this.defaultValue);
  }

  get(): T {
    const storedValue = this.manager.getStoredValue(this.id);
    const parseResult = this.schema.safeParse(storedValue);
    return parseResult.success ? parseResult.data : this.defaultValue;
  }

  set(newValue: T): void {
    const parseResult = this.schema.safeParse(newValue);
    if (!parseResult.success) {
      console.error(
        `Invalid value for setting "${this.id}":`,
        newValue,
        'Error:',
        parseResult.error,
      );
      return;
    }

    const validatedValue = parseResult.data;
    if (this.get() !== validatedValue) {
      this.manager.updateStoredValue(this.id, validatedValue);
    }
  }

  reset(): void {
    this.manager.clearStoredValue(this.id);
  }

  [Symbol.dispose](): void {
    // Use the stored disposable if available
    this.manager.unregister(this.id);
  }
}

export class SettingsManagerImpl implements SettingsManager {
  private readonly registry = new Map<string, SettingImpl<unknown>>();
  private currentStoredValues: Record<string, unknown> = {};
  private readonly store: Storage;

  constructor(store: Storage) {
    this.store = store;
    this.load();
  }

  get<T>(id: string): Setting<T> | undefined {
    return this.registry.get(id) as Setting<T> | undefined;
  }

  register<T>(setting: SettingDescriptor<T>, pluginId?: string): Setting<T> {
    // Determine the initial value: stored value if valid, otherwise default.
    const storedValue = this.currentStoredValues[setting.id];
    const parseResult = setting.schema.safeParse(storedValue);

    // If the stored value was invalid, update storage with the default.
    if (!parseResult.success && storedValue !== undefined) {
      this.currentStoredValues[setting.id] = setting.defaultValue;
      this.save();
    }

    if (this.registry.has(setting.id)) {
      throw new Error(`Setting with id "${setting.id}" already registered.`);
    }

    const settingImpl = new SettingImpl<T>(
      this,
      pluginId,
      setting.id,
      setting.name,
      setting.description,
      setting.defaultValue,
      setting.schema,
      setting.requiresReload,
      setting.render,
    );

    this.registry.set(setting.id, settingImpl as SettingImpl<unknown>);

    return settingImpl;
  }

  unregister(id: string): void {
    this.registry.delete(id);
  }

  resetAll(): void {
    this.currentStoredValues = {};
    this.save();
  }

  getAllSettings(): ReadonlyArray<SettingImpl<unknown>> {
    const settings = Array.from(this.registry.values());
    settings.sort((a, b) => a.name.localeCompare(b.name));
    return settings;
  }

  isReloadRequired(): boolean {
    // Check if any setting that requires reload has changed from its original value
    for (const setting of this.registry.values()) {
      if (setting.requiresReload) {
        const bootValue = setting.bootValue;
        const currentValue = setting.get();

        // Different serialization might cause false differences, so use JSON comparison
        if (JSON.stringify(currentValue) !== JSON.stringify(bootValue)) {
          return true;
        }
      }
    }
    return false;
  }

  // Internal method to get stored values
  getStoredValue(id: string): unknown {
    return this.currentStoredValues[id];
  }

  // Internal method to update stored values
  updateStoredValue(id: string, value: unknown): void {
    this.currentStoredValues[id] = value;
    this.save();
  }

  clearStoredValue(id: string): void {
    delete this.currentStoredValues[id];
    this.save();
  }

  private load(): void {
    try {
      this.currentStoredValues = this.store.load();
    } catch (e) {
      console.error('Failed to load settings from store:', e);
      this.currentStoredValues = {};
    }

    // Re-validate existing registered settings after load
    for (const runtime of this.registry.values()) {
      const setting = runtime;
      const storedValue = this.currentStoredValues[setting.id];
      const parseResult = setting.schema.safeParse(storedValue);

      // Ensure storage reflects the potentially corrected value
      if (!parseResult.success && storedValue !== undefined) {
        this.currentStoredValues[setting.id] = setting.defaultValue;
      }

      // Don't overwrite originalValues here since they'll be set during registration
    }

    // Save potentially corrected values back to storage
    this.save();
  }

  private save(): void {
    try {
      this.store.save(this.currentStoredValues);
    } catch (e) {
      console.error('Failed to save settings to store:', e);
    }
  }
}

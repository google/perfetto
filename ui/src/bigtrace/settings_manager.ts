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

import {z} from 'zod';
import {Setting, SettingDescriptor, SettingsManager} from '../public/settings';
import m from 'mithril';

class SettingImpl<T> implements Setting<T> {
  constructor(
    private settingsManager: SettingsManagerImpl,
    public readonly id: string,
    public readonly name: string,
    public readonly description: string,
    public readonly schema: z.ZodType<T>,
    public readonly defaultValue: T,
  ) {}

  get isDefault(): boolean {
    return this.get() === this.defaultValue;
  }

  get(): T {
    const storedValue = this.settingsManager.getStoredValue(this.id);
    const parsed = this.schema.safeParse(storedValue);
    if (parsed.success) {
      return parsed.data;
    }
    return this.defaultValue;
  }

  set(value: T): void {
    this.settingsManager.setStoredValue(this.id, value);
    m.redraw();
  }

  reset(): void {
    this.settingsManager.setStoredValue(this.id, this.defaultValue);
    m.redraw();
  }

  [Symbol.dispose](): void {
    // Not implemented
  }
}

class SettingsManagerImpl implements SettingsManager {
  private settings = new Map<string, Setting<unknown>>();
  private storage = new Map<string, unknown>();

  register<T>(descriptor: SettingDescriptor<T>): Setting<T> {
    const setting = new SettingImpl(
        this,
        descriptor.id,
        descriptor.name,
        descriptor.description,
        descriptor.schema,
        descriptor.defaultValue,
    );
    this.settings.set(descriptor.id, setting);
    return setting;
  }

  get<T>(id: string): Setting<T> | undefined {
    return this.settings.get(id) as Setting<T> | undefined;
  }

  getAllSettings(): ReadonlyArray<Setting<unknown>> {
    return Array.from(this.settings.values());
  }

  getStoredValue(id: string): unknown {
    return this.storage.get(id);
  }

  setStoredValue(id: string, value: unknown): void {
    this.storage.set(id, value);
  }

  resetAll(): void {
    this.storage.clear();
    m.redraw();
  }

  isReloadRequired(): boolean {
    return false;
  }
}

export const settingsManager = new SettingsManagerImpl();

settingsManager.register({
    id: 'theme',
    name: 'UI Theme',
    description: 'Changes the color palette used throughout the UI.',
    schema: z.enum(['light', 'dark']),
    defaultValue: 'light',
});

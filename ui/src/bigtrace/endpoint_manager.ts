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
import {LocalStorage} from '../core/local_storage';
import {BIGTRACE_SETTINGS_STORAGE_KEY} from './settings_manager';

class SettingImpl<T> implements Setting<T> {
  public readonly requiresReload?: boolean;
  constructor(
    private settingsManager: SettingsManagerImpl,
    public readonly id: string,
    public readonly name: string,
    public readonly description: string,
    public readonly schema: z.ZodType<T>,
    public readonly defaultValue: T,
    requiresReload?: boolean,
  ) {
    this.requiresReload = requiresReload;
  }

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
  private initialValues = new Map<string, unknown>();
  private storage: LocalStorage;

  constructor(storage: LocalStorage) {
    this.storage = storage;
  }

  register<T>(descriptor: SettingDescriptor<T>): Setting<T> {
    const setting = new SettingImpl(
        this,
        descriptor.id,
        descriptor.name,
        descriptor.description,
        descriptor.schema,
        descriptor.defaultValue,
        descriptor.requiresReload,
    );
    this.settings.set(descriptor.id, setting);
    this.initialValues.set(descriptor.id, setting.get());
    return setting;
  }

  get<T>(id: string): Setting<T> | undefined {
    return this.settings.get(id) as Setting<T> | undefined;
  }

  getAllSettings(): ReadonlyArray<Setting<unknown>> {
    return Array.from(this.settings.values());
  }

  getStoredValue(id: string): unknown {
    return this.storage.load()[id];
  }

  setStoredValue(id: string, value: unknown): void {
    const data = this.storage.load();
    data[id] = value;
    this.storage.save(data);
  }

  resetAll(): void {
    this.storage.save({});
    m.redraw();
  }

  isReloadRequired(): boolean {
    for (const setting of this.settings.values()) {
      if (setting.requiresReload) {
        const current = setting.get();
        const initial = this.initialValues.get(setting.id);
        if (current !== initial) {
          return true;
        }
      }
    }
    return false;
  }
}

export const endpointManager = new SettingsManagerImpl(new LocalStorage(BIGTRACE_SETTINGS_STORAGE_KEY));

endpointManager.register({
    id: 'bigtraceEndpoint',
    name: 'BigTrace Endpoint',
    description: 'The URL of the BigTrace backend service.',
    schema: z.string(),
    defaultValue: 'https://autopush-brush-googleapis.corp.google.com',
    requiresReload: true,
});
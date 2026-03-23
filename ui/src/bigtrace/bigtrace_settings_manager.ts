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
import {Setting, SettingDescriptor, EnumOption, SettingFilter, SettingCategory} from './settings_types';
import m from 'mithril';

class SettingImpl<T> implements Setting<T> {
  public readonly id: string;
  public readonly name: string;
  public readonly description: string;
  public readonly type: 'string' | 'number' | 'boolean' | 'enum' | 'multi-select' | 'string-array';
  public readonly schema: z.ZodType<T>;
  public readonly defaultValue: T;
  public readonly category?: string;
  public readonly requiresReload?: boolean;
  public readonly options?: readonly (string | EnumOption)[];
  public readonly placeholder?: string;
  public readonly format?: 'sql';
  public readonly disabled: boolean;

  constructor(
    private settingsManager: SettingsManagerImpl,
    descriptor: SettingDescriptor<T>,
    private disabledStateStorage: LocalStorage,
  ) {
    this.id = descriptor.id;
    this.name = descriptor.name;
    this.description = descriptor.description;
    this.type = descriptor.type;
    this.schema = descriptor.schema;
    this.defaultValue = descriptor.defaultValue;
    this.category = descriptor.category;
    this.requiresReload = descriptor.requiresReload;
    this.options = descriptor.options;
    this.placeholder = descriptor.placeholder;
    this.format = descriptor.format;
    this.disabled = descriptor.disabled ?? false;

    if (this.disabled === true) {
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

  isDisabled(): boolean {
    const state = this.disabledStateStorage.load()[this.id];
    return state === undefined ? false : Boolean(state);
  }

  setDisabled(disabled: boolean): void {
    const data = this.disabledStateStorage.load();
    data[this.id] = disabled;
    this.disabledStateStorage.save(data);
    m.redraw();
  }

  [Symbol.dispose](): void {
    // Not implemented
  }
}

import {bigTraceSettingsService} from './bigtrace_settings_service';
import {LocalStorage} from '../core/local_storage';
import {BIGTRACE_SETTINGS_STORAGE_KEY} from './settings_manager';

export interface SettingsManager {
  register<T>(setting: SettingDescriptor<T>): Setting<T>;
  resetAll(): void;
  getAllSettings(): ReadonlyArray<Setting<unknown>>;
  isReloadRequired(): boolean;
  get<T>(id: string): Setting<T> | undefined;
  reloadMetadataSettings(): Promise<void>;
  loadSettings(force?: boolean): Promise<void>;
  readonly loadError: string | undefined;
}

class SettingsManagerImpl implements SettingsManager {
  private settings = new Map<string, Setting<unknown>>();
  private storage: LocalStorage;
  public isLoading = false;
  public isMetadataLoading = false;
  public loadError: string | undefined = undefined;
  private hasLoaded = false;
  private loadPromise: Promise<void> | null = null;
  private lastLoadedMetadataFilters: string | null = null;

  constructor(storage: LocalStorage) {
    this.storage = storage;
  }

  async loadSettings(force = false): Promise<void> {
    if (this.hasLoaded && !force) return;
    if (this.loadPromise) return this.loadPromise;

    this.loadPromise = this._loadSettingsInternal();
    try {
      await this.loadPromise;
    } finally {
      this.loadPromise = null;
    }
  }

  private async _loadSettingsInternal(): Promise<void> {
    this.isLoading = true;
    this.loadError = undefined;
    m.redraw();
    
    try {
      this.settings.clear();
      
      const execSettings = await bigTraceSettingsService.getExecutionSettings();
      for (const setting of execSettings) {
        this.register(setting);
      }
      
      this.isLoading = false;
      this.hasLoaded = true;
      this.isMetadataLoading = true;
      m.redraw();

      const filters = this.buildSettingFilters();
      this.lastLoadedMetadataFilters = JSON.stringify(filters.filter(f => f.category === 'TRACE_ADDRESS'));
      
      const metadataSettings = await bigTraceSettingsService.getMetadataSettings(filters);
      for (const setting of metadataSettings) {
        this.register(setting);
      }
      
    } catch (e) {
      this.loadError = e instanceof Error ? e.message : String(e);
      this.hasLoaded = true;
    } finally {
      this.isLoading = false;
      this.isMetadataLoading = false;
      m.redraw();
    }
  }

  async reloadMetadataSettings(): Promise<void> {
    this.isMetadataLoading = true;
    m.redraw();

    const existingIds = Array.from(this.settings.keys());
    for (const id of existingIds) {
      const setting = this.settings.get(id);
      if (setting && setting.category === 'TRACE_METADATA') {
        this.settings.delete(id);
      }
    }

    try {
      const filters = this.buildSettingFilters();
      this.lastLoadedMetadataFilters = JSON.stringify(filters.filter(f => f.category === 'TRACE_ADDRESS'));

      const metadataSettings = await bigTraceSettingsService.getMetadataSettings(filters);
      for (const setting of metadataSettings) {
        this.register(setting);
      }
    } catch (e) {
      this.loadError = e instanceof Error ? e.message : String(e);
    } finally {
      this.isMetadataLoading = false;
      m.redraw();
    }
  }

  buildSettingFilters(): SettingFilter[] {
    const filters: SettingFilter[] = [];
    for (const setting of this.getAllSettings()) {
      if (!setting.isDisabled() && setting.category !== undefined) {
        let values: string[] = [];
        const val = setting.get();
        if (Array.isArray(val)) {
          values = val.map(String);
        } else {
          values = [String(val)];
        }

        if (setting.options && setting.options.length > 0) {
          const validOptionValues = new Set(
            setting.options.map(opt => typeof opt === 'string' ? opt : String(opt.value))
          );
          values = values.filter(v => validOptionValues.has(v));
        }

        filters.push({
          settingId: setting.id,
          category: setting.category as SettingCategory,
          values,
        });
      }
    }
    return filters;
  }

  register<T>(descriptor: SettingDescriptor<T>): Setting<T> {
    const setting = new SettingImpl(this, descriptor, disabledStateStorage);
    this.settings.set(descriptor.id, setting as Setting<unknown>);
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
    if (!this.hasLoaded || this.lastLoadedMetadataFilters === null) return false;
    const currentFilters = this.buildSettingFilters().filter(f => f.category === 'TRACE_ADDRESS');
    return JSON.stringify(currentFilters) !== this.lastLoadedMetadataFilters;
  }
}

export const bigTraceSettingsManager = new SettingsManagerImpl(new LocalStorage(BIGTRACE_SETTINGS_STORAGE_KEY));
const SETTINGS_DISABLED_STATE_STORAGE_KEY = 'bigtraceSettingsDisabledState';
const disabledStateStorage = new LocalStorage(SETTINGS_DISABLED_STATE_STORAGE_KEY);

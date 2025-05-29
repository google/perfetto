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
import {Storage} from './storage';
import {SettingsManagerImpl} from './settings_manager';

// Mock Storage implementation for testing
class MockStorage implements Storage {
  private data: Record<string, unknown> = {};

  load(): Record<string, unknown> {
    // Return a copy to prevent direct modification
    return JSON.parse(JSON.stringify(this.data));
  }

  save(data: Record<string, unknown>): void {
    // Store a copy
    this.data = JSON.parse(JSON.stringify(data));
  }

  // Helper for tests to inspect stored data
  getStoredData(): Record<string, unknown> {
    return this.data;
  }

  // Helper for tests to set initial data
  setInitialData(data: Record<string, unknown>): void {
    this.data = data;
  }
}

describe('SettingsManagerImpl', () => {
  let mockStorage: MockStorage;
  let settingsManager: SettingsManagerImpl;

  const stringSettingDesc = {
    id: 'test.string',
    name: 'Test String',
    description: 'A test string setting',
    defaultValue: 'default',
    schema: z.string(),
  };

  const numberSettingDesc = {
    id: 'test.number',
    name: 'Test Number',
    description: 'A test number setting',
    defaultValue: 42,
    schema: z.number(),
    requiresReload: true,
  };

  const boolSettingDesc = {
    id: 'test.bool',
    name: 'Test Boolean',
    description: 'A test boolean setting',
    defaultValue: false,
    schema: z.boolean(),
  };

  beforeEach(() => {
    mockStorage = new MockStorage();
    settingsManager = new SettingsManagerImpl(mockStorage);
  });

  test('should register a setting', () => {
    const setting = settingsManager.register(stringSettingDesc);
    expect(setting).toBeDefined();
    expect(setting.id).toBe(stringSettingDesc.id);
    expect(settingsManager.getAllSettings().length).toBe(1);
  });

  test('should get default value if not set', () => {
    const setting = settingsManager.register(stringSettingDesc);
    expect(setting.get()).toBe(stringSettingDesc.defaultValue);
    expect(setting.isDefault).toBe(true);
  });

  test('should set and get a value', () => {
    const setting = settingsManager.register(stringSettingDesc);
    const newValue = 'new value';
    setting.set(newValue);
    expect(setting.get()).toBe(newValue);
    expect(setting.isDefault).toBe(false);
    // Check if saved to mock storage
    expect(mockStorage.getStoredData()[stringSettingDesc.id]).toBe(newValue);
  });

  test('should reset a value to default', () => {
    const setting = settingsManager.register(stringSettingDesc);
    const newValue = 'new value';
    setting.set(newValue);
    expect(setting.get()).toBe(newValue); // Verify it was set

    setting.reset();
    expect(setting.get()).toBe(stringSettingDesc.defaultValue);
    expect(setting.isDefault).toBe(true);
    // Check if removed from mock storage
    expect(mockStorage.getStoredData()[stringSettingDesc.id]).toBeUndefined();
  });

  test('should load existing value from storage on init', () => {
    const initialValue = 'stored value';
    mockStorage.setInitialData({[stringSettingDesc.id]: initialValue});
    // Recreate manager to trigger load
    settingsManager = new SettingsManagerImpl(mockStorage);
    const setting = settingsManager.register(stringSettingDesc);

    expect(setting.get()).toBe(initialValue);
    expect(setting.isDefault).toBe(false);
  });

  test('should handle invalid value in storage during load', () => {
    mockStorage.setInitialData({[stringSettingDesc.id]: 123}); // Invalid type
    settingsManager = new SettingsManagerImpl(mockStorage); // Load happens here
    const setting = settingsManager.register(stringSettingDesc);

    // Should fall back to default
    expect(setting.get()).toBe(stringSettingDesc.defaultValue);
    expect(setting.isDefault).toBe(true);
    // Storage should be corrected
    expect(mockStorage.getStoredData()[stringSettingDesc.id]).toBe(
      stringSettingDesc.defaultValue,
    );
  });

  test('should ignore invalid value during set', () => {
    const setting = settingsManager.register(numberSettingDesc);
    const initialValue = setting.get();
    setting.set('not a number' as unknown as number); // Invalid type
    expect(setting.get()).toBe(initialValue); // Value should not change
    expect(mockStorage.getStoredData()[numberSettingDesc.id]).toBeUndefined(); // Should not be saved
  });

  test('should return all registered settings sorted by id', () => {
    settingsManager.register(numberSettingDesc);
    settingsManager.register(stringSettingDesc);
    settingsManager.register(boolSettingDesc);

    const allSettings = settingsManager.getAllSettings();
    expect(allSettings.length).toBe(3);
    expect(allSettings[0].id).toBe(boolSettingDesc.id); // test.bool
    expect(allSettings[1].id).toBe(numberSettingDesc.id); // test.number
    expect(allSettings[2].id).toBe(stringSettingDesc.id); // test.string
  });

  test('isReloadRequired should be false initially', () => {
    settingsManager.register(stringSettingDesc); // requiresReload: false
    settingsManager.register(numberSettingDesc); // requiresReload: true
    expect(settingsManager.isReloadRequired()).toBe(false);
  });

  test('isReloadRequired should be true if reloadable setting changed', () => {
    const setting = settingsManager.register(numberSettingDesc); // requiresReload: true
    setting.set(100);
    expect(settingsManager.isReloadRequired()).toBe(true);
  });

  test('isReloadRequired should be false if non-reloadable setting changed', () => {
    const setting = settingsManager.register(stringSettingDesc); // requiresReload: false
    setting.set('new value');
    expect(settingsManager.isReloadRequired()).toBe(false);
  });

  test('isReloadRequired should be false if reloadable setting changed then reset', () => {
    const setting = settingsManager.register(numberSettingDesc); // requiresReload: true
    setting.set(100);
    setting.reset();
    expect(settingsManager.isReloadRequired()).toBe(false);
  });

  test('resetAll should clear all settings and storage', () => {
    const s1 = settingsManager.register(stringSettingDesc);
    const s2 = settingsManager.register(numberSettingDesc);
    s1.set('val1');
    s2.set(99);

    expect(s1.isDefault).toBe(false);
    expect(s2.isDefault).toBe(false);
    expect(Object.keys(mockStorage.getStoredData()).length).toBe(2);

    settingsManager.resetAll();

    expect(s1.get()).toBe(stringSettingDesc.defaultValue);
    expect(s2.get()).toBe(numberSettingDesc.defaultValue);
    expect(s1.isDefault).toBe(true);
    expect(s2.isDefault).toBe(true);
    expect(Object.keys(mockStorage.getStoredData()).length).toBe(0); // Storage cleared
  });
});

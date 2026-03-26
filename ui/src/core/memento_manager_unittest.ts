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
import {MementoManagerImpl} from './memento_manager';

class MockStorage implements Storage {
  private data: Record<string, unknown> = {};

  load(): Record<string, unknown> {
    return JSON.parse(JSON.stringify(this.data));
  }

  save(data: Record<string, unknown>): void {
    this.data = JSON.parse(JSON.stringify(data));
  }

  getStoredData(): Record<string, unknown> {
    return this.data;
  }

  setInitialData(data: Record<string, unknown>): void {
    this.data = data;
  }
}

describe('MementoManagerImpl', () => {
  let mockStorage: MockStorage;
  let mementoManager: MementoManagerImpl;

  const stringMementoDesc = {
    id: 'test.string',
    defaultValue: 'default',
    schema: z.string(),
  };

  const numberMementoDesc = {
    id: 'test.number',
    defaultValue: 42,
    schema: z.number(),
  };

  const boolMementoDesc = {
    id: 'test.bool',
    defaultValue: false,
    schema: z.boolean(),
  };

  beforeEach(() => {
    mockStorage = new MockStorage();
    mementoManager = new MementoManagerImpl(mockStorage);
  });

  test('should register a memento', () => {
    const memento = mementoManager.register(stringMementoDesc);
    expect(memento).toBeDefined();
    expect(memento.id).toBe(stringMementoDesc.id);
  });

  test('should get default value if not set', () => {
    const memento = mementoManager.register(stringMementoDesc);
    expect(memento.get()).toBe(stringMementoDesc.defaultValue);
    expect(memento.isDefault).toBe(true);
  });

  test('should set and get a value', () => {
    const memento = mementoManager.register(stringMementoDesc);
    const newValue = 'new value';
    memento.set(newValue);
    expect(memento.get()).toBe(newValue);
    expect(memento.isDefault).toBe(false);
    expect(mockStorage.getStoredData()[stringMementoDesc.id]).toBe(newValue);
  });

  test('should reset a value to default', () => {
    const memento = mementoManager.register(stringMementoDesc);
    memento.set('new value');
    memento.reset();
    expect(memento.get()).toBe(stringMementoDesc.defaultValue);
    expect(memento.isDefault).toBe(true);
    expect(mockStorage.getStoredData()[stringMementoDesc.id]).toBeUndefined();
  });

  test('should load existing value from storage on init', () => {
    const initialValue = 'stored value';
    mockStorage.setInitialData({[stringMementoDesc.id]: initialValue});
    mementoManager = new MementoManagerImpl(mockStorage);
    const memento = mementoManager.register(stringMementoDesc);
    expect(memento.get()).toBe(initialValue);
    expect(memento.isDefault).toBe(false);
  });

  test('should throw on duplicate registration', () => {
    mementoManager.register(stringMementoDesc);
    expect(() => mementoManager.register(stringMementoDesc)).toThrow(
      /already registered/,
    );
  });

  test('resetAll should clear all mementos and storage', () => {
    const m1 = mementoManager.register(stringMementoDesc);
    const m2 = mementoManager.register(numberMementoDesc);
    m1.set('val1');
    m2.set(99);

    expect(m1.isDefault).toBe(false);
    expect(m2.isDefault).toBe(false);

    mementoManager.resetAll();

    expect(m1.get()).toBe(stringMementoDesc.defaultValue);
    expect(m2.get()).toBe(numberMementoDesc.defaultValue);
    expect(m1.isDefault).toBe(true);
    expect(m2.isDefault).toBe(true);
    expect(Object.keys(mockStorage.getStoredData()).length).toBe(0);
  });

  test('get() returns cached value without re-parsing', () => {
    const memento = mementoManager.register(stringMementoDesc);
    const spy = jest.spyOn(stringMementoDesc.schema, 'safeParse');

    memento.get();
    memento.get();
    memento.get();

    expect(spy).toHaveBeenCalledTimes(1);
    spy.mockRestore();
  });

  test('cache invalidates after set()', () => {
    const memento = mementoManager.register(stringMementoDesc);
    const spy = jest.spyOn(stringMementoDesc.schema, 'safeParse');

    memento.get();
    memento.set('new value');
    memento.get();

    expect(spy).toHaveBeenCalledTimes(2);
    spy.mockRestore();
  });

  test('cache invalidates after resetAll()', () => {
    const memento = mementoManager.register(stringMementoDesc);
    memento.set('new value');

    expect(memento.get()).toBe('new value');
    mementoManager.resetAll();
    expect(memento.get()).toBe(stringMementoDesc.defaultValue);
  });

  test('mutating a returned object throws at runtime', () => {
    const memento = mementoManager.register({
      id: 'test.object',
      defaultValue: {a: 1, b: 2},
      schema: z.object({a: z.number(), b: z.number()}),
    });

    const value = memento.get();
    expect(() => {
      value.a = 42;
    }).toThrow();
  });

  test('falls back to default for invalid stored values', () => {
    mockStorage.setInitialData({[numberMementoDesc.id]: 'not a number'});
    mementoManager = new MementoManagerImpl(mockStorage);
    const memento = mementoManager.register(numberMementoDesc);
    expect(memento.get()).toBe(numberMementoDesc.defaultValue);
  });

  test('dispose unregisters the memento', () => {
    const memento = mementoManager.register(boolMementoDesc);
    memento[Symbol.dispose]();
    // Should be able to re-register after disposal
    const memento2 = mementoManager.register(boolMementoDesc);
    expect(memento2).toBeDefined();
  });
});

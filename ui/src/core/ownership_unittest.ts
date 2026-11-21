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

import {carryOwner, getOwner, setOwner} from './ownership';

describe('setOwner / getOwner', () => {
  test('plain object', () => {
    const obj = {uri: 'a'};
    setOwner(obj, 'com.example.A');
    expect(getOwner(obj)).toBe('com.example.A');
  });

  test('unmarked object has no owner', () => {
    expect(getOwner({})).toBeUndefined();
  });

  test('first owner wins', () => {
    const obj = {};
    setOwner(obj, 'com.example.A');
    setOwner(obj, 'com.example.B');
    expect(getOwner(obj)).toBe('com.example.A');
  });

  test('frozen, sealed and non-extensible objects are skipped', () => {
    const frozen = Object.freeze({});
    const sealed = Object.seal({});
    const locked = Object.preventExtensions({});
    expect(() => setOwner(frozen, 'com.example.A')).not.toThrow();
    expect(() => setOwner(sealed, 'com.example.A')).not.toThrow();
    expect(() => setOwner(locked, 'com.example.A')).not.toThrow();
    expect(getOwner(frozen)).toBeUndefined();
    expect(getOwner(sealed)).toBeUndefined();
    expect(getOwner(locked)).toBeUndefined();
  });

  test('class instance keeps its methods and gets an owner', () => {
    class MyTab {
      private readonly title = 'Mine';
      getTitle() {
        return this.title;
      }
    }
    const tab = new MyTab();
    setOwner(tab, 'com.example.A');
    expect(getOwner(tab)).toBe('com.example.A');
    expect(tab.getTitle()).toBe('Mine');
    expect(tab).toBeInstanceOf(MyTab);
  });

  test('owner survives object spread', () => {
    const item = {text: 'Item'};
    setOwner(item, 'com.example.A');
    const copy = {...item, id: 'sidebar_1'};
    expect(getOwner(copy)).toBe('com.example.A');
  });

  test('owner is not visible to string-keyed iteration or JSON', () => {
    const obj = {a: 1};
    setOwner(obj, 'com.example.A');
    expect(Object.keys(obj)).toEqual(['a']);
    expect(JSON.stringify(obj)).toBe('{"a":1}');
  });
});

describe('carryOwner', () => {
  test('copies the owner to a rebuilt object', () => {
    const original = {id: 'x'};
    setOwner(original, 'com.example.A');
    const rebuilt = carryOwner(original, {id: 'x', render: () => undefined});
    expect(getOwner(rebuilt)).toBe('com.example.A');
  });

  test('does nothing when the source has no owner', () => {
    expect(getOwner(carryOwner({}, {}))).toBeUndefined();
  });

  test('keeps an owner already set on the target', () => {
    const original = {};
    setOwner(original, 'com.example.A');
    const target = {};
    setOwner(target, 'com.example.B');
    expect(getOwner(carryOwner(original, target))).toBe('com.example.B');
  });
});

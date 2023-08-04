// Copyright (C) 2023 The Android Open Source Project
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

import {checkHotkey, Hotkey, parseHotkey} from './hotkeys';

test('parseHotkey', () => {
  expect(parseHotkey('A'))
      .toEqual({key: 'A', allowInEditable: false, modifier: ''});
  expect(parseHotkey('!A'))
      .toEqual({key: 'A', allowInEditable: true, modifier: ''});
  expect(parseHotkey('Shift+A'))
      .toEqual({key: 'A', allowInEditable: false, modifier: 'Shift+'});
  expect(parseHotkey('Mod+Shift+A'))
      .toEqual({key: 'A', allowInEditable: false, modifier: 'Mod+Shift+'});
  expect(parseHotkey('!Mod+Shift+A'))
      .toEqual({key: 'A', allowInEditable: true, modifier: 'Mod+Shift+'});
});

describe('checkHotkey', () => {
  test('A', () => {
    expect(checkHotkey('A', {key: 'a'})).toBe(true);
    expect(checkHotkey('A', {key: 'A', shiftKey: true})).toBe(false);
    expect(checkHotkey('A', {key: 'a', ctrlKey: true})).toBe(false);
    expect(checkHotkey('A', {key: 'a', altKey: true})).toBe(false);
  });

  test('Special', () => {
    expect(checkHotkey('Enter', {key: 'Enter'})).toBe(true);
    expect(checkHotkey('Escape', {key: 'Escape'})).toBe(true);
  });

  test('Shift+A', () => {
    const hotkey: Hotkey = 'Shift+A';
    expect(checkHotkey(hotkey, {key: 'a'})).toBe(false);
    expect(checkHotkey(hotkey, {key: 'A', shiftKey: true})).toBe(true);
    expect(checkHotkey(hotkey, {key: 'a', ctrlKey: true})).toBe(false);
    expect(checkHotkey(hotkey, {key: 'a', altKey: true})).toBe(false);
  });

  test('Mod+A on PC', () => {
    const hotkey: Hotkey = 'Mod+A';
    expect(checkHotkey(hotkey, {key: 'a'}, 'PC')).toBe(false);
    expect(checkHotkey(hotkey, {key: 'A', shiftKey: true}, 'PC')).toBe(false);
    expect(checkHotkey(hotkey, {key: 'a', ctrlKey: true}, 'PC')).toBe(true);
    expect(checkHotkey(hotkey, {key: 'a', metaKey: true}, 'PC')).toBe(false);
  });

  test('Mod+A on Mac', () => {
    const hotkey: Hotkey = 'Mod+A';
    expect(checkHotkey(hotkey, {key: 'a'}, 'Mac')).toBe(false);
    expect(checkHotkey(hotkey, {key: 'A', shiftKey: true}, 'Mac')).toBe(false);
    expect(checkHotkey(hotkey, {key: 'a', ctrlKey: true}, 'Mac')).toBe(false);
    expect(checkHotkey(hotkey, {key: 'a', metaKey: true}, 'Mac')).toBe(true);
  });

  test('allow in editable', () => {
    const el = document.createElement('input');
    el.setAttribute('type', 'text');

    expect(checkHotkey('X', {key: 'x'})).toBe(true);
    expect(checkHotkey('!X', {key: 'x'})).toBe(true);

    expect(checkHotkey('X', {key: 'x', target: el})).toBe(false);
    expect(checkHotkey('!X', {key: 'x', target: el})).toBe(true);
  });
});

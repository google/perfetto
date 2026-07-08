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

import type {Command} from '../public/commands';
import {type HotkeyOverlay, remapHotkeys} from './command_manager';

describe('remapHotkeys', () => {
  test('single layer', () => {
    const cmd: Command = {
      id: 'cmd1',
      name: '',
      callback: () => {},
      defaultHotkey: 'P',
    };
    const overlay: HotkeyOverlay = {
      cmd1: 'X',
    };
    const remapped = remapHotkeys([cmd], [overlay]);

    expect(remapped[0].defaultHotkey).toBe('X');
  });

  test('double layer', () => {
    const cmd: Command = {
      id: 'cmd1',
      name: '',
      callback: () => {},
      defaultHotkey: 'P',
    };
    const overlay1: HotkeyOverlay = {
      cmd1: 'X',
    };
    const overlay2: HotkeyOverlay = {
      cmd1: 'Z',
    };
    const remapped = remapHotkeys([cmd], [overlay1, overlay2]);

    expect(remapped[0].defaultHotkey).toBe('Z');
  });

  test('not defined in layer', () => {
    const cmd: Command = {
      id: 'cmd1',
      name: '',
      callback: () => {},
      defaultHotkey: 'P',
    };
    const overlay1: HotkeyOverlay = {
      anotherCmd: 'X',
    };
    const remapped = remapHotkeys([cmd], [overlay1]);

    expect(remapped[0].defaultHotkey).toBe('P');
  });

  test('no overlays', () => {
    const cmd: Command = {
      id: 'cmd1',
      name: '',
      callback: () => {},
      defaultHotkey: 'P',
    };
    const remapped = remapHotkeys([cmd], []);

    expect(remapped[0].defaultHotkey).toBe('P');
  });

  test('cmd without default hotkey gets remapped', () => {
    const cmd: Command = {
      id: 'cmd1',
      name: '',
      callback: () => {},
    };
    const overlay: HotkeyOverlay = {
      cmd1: 'X',
    };
    const remapped = remapHotkeys([cmd], [overlay]);

    expect(remapped[0].defaultHotkey).toBe('X');
  });
});

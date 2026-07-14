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

import {CommandError, type Command} from '../public/commands';
import {
  CommandManagerImpl,
  type HotkeyOverlay,
  remapHotkeys,
} from './command_manager';
import {OmniboxManagerImpl} from './omnibox_manager';

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

describe('CommandManagerImpl error wrapping', () => {
  let commandManager: CommandManagerImpl;

  beforeEach(() => {
    const omnibox = new OmniboxManagerImpl();
    commandManager = new CommandManagerImpl(omnibox);
  });

  test('wraps callback error in CommandError', async () => {
    commandManager.registerCommand({
      id: 'cmd.fail',
      name: 'Failing Command',
      source: 'Test Source',
      callback: () => {
        throw new Error('Root error message');
      },
    });

    await expect(commandManager.runCommand('cmd.fail')).rejects.toThrow(
      new CommandError(
        'cmd.fail',
        'Failing Command',
        'Test Source',
        new Error('Root error message'),
      ),
    );
  });

  test('wraps recursively for nested commands', async () => {
    commandManager.registerCommand({
      id: 'cmd.leaf',
      name: 'Leaf Command',
      source: 'Leaf Source',
      callback: () => {
        throw new Error('Root error');
      },
    });

    commandManager.registerCommand({
      id: 'cmd.parent',
      name: 'Parent Command',
      source: 'Parent Source',
      callback: async () => {
        await commandManager.runCommand('cmd.leaf');
      },
    });

    let threw = false;
    try {
      await commandManager.runCommand('cmd.parent');
    } catch (err) {
      threw = true;
      expect(err).toBeInstanceOf(CommandError);
      const parentErr = err as CommandError;
      expect(parentErr.commandId).toBe('cmd.parent');
      expect(parentErr.commandName).toBe('Parent Command');

      expect(parentErr.cause).toBeInstanceOf(CommandError);
      const leafErr = parentErr.cause as CommandError;
      expect(leafErr.commandId).toBe('cmd.leaf');
      expect(leafErr.commandName).toBe('Leaf Command');

      expect(leafErr.cause).toBeInstanceOf(Error);
      expect(leafErr.cause.message).toBe('Root error');

      expect(parentErr.toString()).toContain(
        'Command/Macro: Parent Command (cmd.parent)',
      );
      expect(parentErr.toString()).toContain(
        'Caused by: Command/Macro: Leaf Command (cmd.leaf)',
      );
      expect(parentErr.toString()).toContain('Caused by: Error: Root error');
    }
    expect(threw).toBe(true);
  });
});

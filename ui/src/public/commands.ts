// Copyright (C) 2024 The Android Open Source Project
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

import type {Hotkey} from '../base/hotkeys';

export interface Command {
  // A unique id for this command.
  id: string;
  // A human-friendly name for this command.
  name: string;
  // Callback is called when the command is invoked.
  callback: (...args: unknown[]) => unknown;
  // Default hotkey for this command.
  // Note: this is just the default and may be changed by the user.
  // Examples:
  // - 'P'
  // - 'Shift+P'
  // - '!Mod+Shift+P'
  // See hotkeys.ts for guidance on hotkey syntax.
  defaultHotkey?: Hotkey;
  // A human-readable label shown as a left-side chip in the command palette,
  // indicating where this command came from (e.g. extension module name).
  source?: string;
}

export interface CommandManager {
  // Register a command. Throws if a command with the same id already exists.
  // Dispose the returned handle to unregister.
  registerCommand(command: Command): Disposable;
  // Returns true if a command with the given id is registered.
  hasCommand(commandId: string): boolean;
  // Look up a registered command by id. Returns the command, or undefined if
  // not found.
  getCommand(commandId: string): Command | undefined;
  // Returns all registered commands.
  getCommands(): readonly Command[];
  // Invoke a registered command by id, forwarding any extra args to its
  // callback. Returns whatever the callback returns.
  runCommand(id: string, ...args: unknown[]): unknown;
}

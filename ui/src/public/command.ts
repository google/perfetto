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

import {Hotkey} from '../base/hotkeys';

/**
 * Manages the registration and execution of commands.
 *
 * Commands are user-invokable actions that can be triggered via hotkeys,
 * the command palette, or programmatically. Use this to register plugin
 * commands that users can execute.
 */
export interface CommandManager {
  /**
   * Registers a new command.
   *
   * The command is uniquely identified by its `id`. If a command with the same
   * `id` is already registered, this method will throw an error.
   *
   * @param command The command to register.
   */
  registerCommand(command: Command): void;

  /**
   * Checks if a command with the given `id` is registered.
   *
   * @param commandId The unique identifier of the command.
   * @returns `true` if the command is registered, `false` otherwise.
   */
  hasCommand(commandId: string): boolean;

  /**
   * Executes a command by its `id`.
   *
   * Any additional arguments are passed to the command's `callback` function.
   *
   * @param id The unique identifier of the command to run.
   * @param args A list of arguments to pass to the command's callback.
   * @returns The result of the command's callback, if any.
   */
  runCommand(id: string, ...args: unknown[]): unknown;
}

/**
 * Represents a command that can be executed within the application.
 *
 * A command is a self-contained unit of work that can be invoked by the user
 * or programmatically. It includes a unique identifier, a human-readable name,
 * a callback function to execute, and an optional default hotkey.
 */
export interface Command {
  /**
   * A unique identifier for this command.
   *
   * This `id` is used to register, unregister, and execute the command. It is
   * recommended to use a namespace to avoid collisions (e.g.,
   * `myPlugin.myCommand`).
   */
  readonly id: string;

  /**
   * A human-friendly name for this command.
   *
   * This name is displayed to the user in the command palette and other UI
   * elements. It should be concise and descriptive.
   */
  readonly name: string;

  /**
   * The function to call when the command is invoked.
   *
   * This function receives any arguments passed to `runCommand` and can return
   * a value.
   */
  callback(...args: unknown[]): unknown;

  /**
   * The default hotkey for this command.
   *
   * This is just the default and may be changed by the user in the settings.
   * See `hotkeys.ts` for guidance on hotkey syntax.
   *
   * @example
   * - 'P'
   * - 'Shift+P'
   * - '!Mod+Shift+P'
   */
  readonly defaultHotkey?: Hotkey;
}

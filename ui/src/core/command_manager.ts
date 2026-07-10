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

import {z} from 'zod';
import {Registry} from '../base/registry';
import type {Command, CommandManager} from '../public/commands';
import {raf} from './raf_scheduler';
import type {OmniboxManagerImpl} from './omnibox_manager';
import {STARTUP_COMMAND_ALLOWLIST_SET} from './startup_command_allowlist';
import {DisposableStack} from '../base/disposable_stack';
import type {Hotkey} from '../base/hotkeys';
import {ActiveCommandInfo, QueryError} from '../trace_processor/query_result';

// A map of command id -> hotkey.
export type HotkeyOverlay = Record<string, Hotkey>;

// A hotkey overlay for firefox browser.
const firefoxOverlay: HotkeyOverlay = {
  'dev.perfetto.OpenCommandPalette': '!F1', // Mod+Shift+P is not overridable in firefox
};

// Work out whether we are running inside firefox or not. Safe to evaluate this
// at module scope because the browser cannot change.
// TODO(stevegolton): We should move this to a common place.
const isFirefox =
  typeof navigator !== 'undefined' && navigator.userAgent.includes('Firefox');

// The list of overlays for this environment.
// For now - only firefox has mods, but this could include other browsers or
// keyboard mappings in the future.
const hotkeyOverlays = isFirefox ? [firefoxOverlay] : [];

/**
 * Remaps command hotkeys using one or more overlays. Overlays are a map of
 * command id -> hotkey. Overlays are applied sequentially so the later overlays
 * take precedence.
 *
 * @param cmds Commands to remap.
 * @param overlays Overlays to apply.
 * @returns Remapped commands - 'cmd.defaultHotkey's updated.
 */
export function remapHotkeys(
  cmds: readonly Command[],
  overlays: readonly HotkeyOverlay[],
): readonly Command[] {
  if (overlays.length === 0) {
    return cmds;
  }
  return cmds.map((cmd) => {
    const overriddenHotkey = overlays.reduce(
      (hotkey: Hotkey | undefined, overlay: HotkeyOverlay) => {
        const overriddenHotkey = overlay[cmd.id];
        return overriddenHotkey ?? hotkey;
      },
      undefined,
    );
    if (!overriddenHotkey) return cmd;
    return {
      ...cmd,
      defaultHotkey: overriddenHotkey,
    };
  });
}

/**
 * Zod schema for a single command invocation.
 * Used for programmatic command execution like startup commands.
 */
export const commandInvocationSchema = z.object({
  /** The command ID to execute (e.g., 'perfetto.CoreCommands#RunQueryAllProcesses'). */
  id: z.string(),
  /** Arguments to pass to the command. */
  args: z.array(z.string()),
});

/**
 * Specification for invoking a command with arguments.
 * Inferred from the Zod schema to keep types in sync.
 */
export type CommandInvocation = z.infer<typeof commandInvocationSchema>;

/**
 * Zod schema for validating CommandInvocation arrays.
 * Used by settings that store lists of commands to execute.
 */
export const commandInvocationArraySchema = z.array(commandInvocationSchema);

/**
 * Zod schema for a macro configuration.
 */
export const macroSchema = z.object({
  // Id of the macro.
  id: z.string(),

  // Name of the macro.
  name: z.string(),

  // List of command invocations to run when this macro is executed.
  run: z.array(commandInvocationSchema).readonly(),
});

/** Type representing a macro configuration. */
export type Macro = z.infer<typeof macroSchema>;

/**
 * Thrown by runCommand() when a command is rejected because it's not on the
 * startup allowlist and the command manager is currently running startup
 * commands. Callers driving the startup loop catch this to collect the set
 * of blocked IDs.
 */
export class StartupCommandNotAllowedError extends Error {
  constructor(readonly commandId: string) {
    super(
      `Startup command "${commandId}" is not on the allowlist and was blocked`,
    );
    this.name = 'StartupCommandNotAllowedError';
  }
}

/**
 * Parses URL commands parameter from route args.
 * @param commandsParam URL commands parameter (JSON-encoded string)
 * @returns Parsed commands array or undefined if parsing fails
 */
export function parseUrlCommands(
  commandsParam: string | undefined,
): CommandInvocation[] | undefined {
  if (!commandsParam) {
    return undefined;
  }

  try {
    const parsed = JSON.parse(commandsParam);
    return commandInvocationArraySchema.parse(parsed);
  } catch {
    return undefined;
  }
}

export class CommandManagerImpl implements CommandManager {
  private readonly registry = new Registry<Command>((cmd) => cmd.id);
  private readonly macros = new Registry<string>((macroId) => macroId);
  private isExecutingStartupCommands = false;

  constructor(private omnibox: OmniboxManagerImpl) {}

  getCommand(commandId: string): Command | undefined {
    const cmd = this.registry.tryGet(commandId);
    if (!cmd) return undefined;
    return remapHotkeys([cmd], hotkeyOverlays)[0];
  }

  hasCommand(commandId: string): boolean {
    return this.registry.has(commandId);
  }

  getCommands(): readonly Command[] {
    return remapHotkeys(this.registry.valuesAsArray(), hotkeyOverlays);
  }

  registerCommand(cmd: Command): Disposable {
    return this.registry.register(cmd);
  }

  async runCommand(id: string, ...args: unknown[]): Promise<unknown> {
    if (this.isExecutingStartupCommands && !this.isStartupCommandAllowed(id)) {
      throw new StartupCommandNotAllowedError(id);
    }
    const cmd = this.registry.get(id);
    try {
      return await cmd.callback(...args);
    } catch (err) {
      if (err instanceof QueryError) {
        err.queryErrorInfo.activeCommand = new ActiveCommandInfo(
          cmd.id,
          cmd.name,
          cmd.source,
        );
      }
      throw err;
    } finally {
      raf.scheduleFullRedraw();
    }
  }

  // Internal API: not part of the public CommandManager interface.

  registerMacro({id, name, run}: Macro, source?: string) {
    const stack = new DisposableStack();
    stack.use(this.macros.register(id));
    stack.use(
      this.registerCommand({
        id,
        name,
        source,
        callback: async () => {
          // Macros could run multiple commands, some of which might prompt the
          // user in an optional way. But macros should be self-contained
          // so we disable prompts during their execution.
          using _ = this.omnibox.disablePrompts();
          for (const command of run) {
            await this.runCommand(command.id, ...command.args);
          }
        },
      }),
    );
    return stack;
  }

  setExecutingStartupCommands(isExecuting: boolean) {
    this.isExecutingStartupCommands = isExecuting;
  }

  private isStartupCommandAllowed(commandId: string): boolean {
    // First check for exact match (fastest)
    if (STARTUP_COMMAND_ALLOWLIST_SET.has(commandId)) {
      return true;
    }

    // Special case: allow all user-defined macros
    if (this.macros.has(commandId)) {
      return true;
    }

    return false;
  }
}

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
import {FuzzyFinder, FuzzySegment} from '../base/fuzzy';
import {Registry} from '../base/registry';
import {Command, CommandManager} from '../public/command';
import {raf} from './raf_scheduler';
import {OmniboxManagerImpl} from './omnibox_manager';
import {STARTUP_COMMAND_ALLOWLIST_SET} from './startup_command_allowlist';
import {DisposableStack} from '../base/disposable_stack';

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

export interface CommandWithMatchInfo extends Command {
  segments: FuzzySegment[];
}

export class CommandManagerImpl implements CommandManager {
  private readonly registry = new Registry<Command>((cmd) => cmd.id);
  private readonly macros = new Registry<string>((macroId) => macroId);
  private isExecutingStartupCommands = false;

  constructor(private omnibox: OmniboxManagerImpl) {}

  getCommand(commandId: string): Command {
    return this.registry.get(commandId);
  }

  hasCommand(commandId: string): boolean {
    return this.registry.has(commandId);
  }

  get commands(): Command[] {
    return Array.from(this.registry.values());
  }

  registerCommand(cmd: Command): Disposable {
    return this.registry.register(cmd);
  }

  runCommand(id: string, ...args: unknown[]): unknown {
    if (this.isExecutingStartupCommands && !this.isStartupCommandAllowed(id)) {
      console.warn(`Command ${id} is not allowed in current execution context`);
      return;
    }
    const cmd = this.registry.get(id);
    const res = cmd.callback(...args);
    Promise.resolve(res).finally(() => raf.scheduleFullRedraw());
    return res;
  }

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

  // Returns a list of commands that match the search term, along with a list
  // of segments which describe which parts of the command name match and
  // which don't.
  fuzzyFilterCommands(searchTerm: string): CommandWithMatchInfo[] {
    const finder = new FuzzyFinder(this.commands, ({name}) => name);
    return finder.find(searchTerm).map((result) => {
      return {segments: result.segments, ...result.item};
    });
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

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
  private allowlistCheckFn: (id: string) => boolean = () => true;

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

  setAllowlistCheck(checkFn: (id: string) => boolean): void {
    this.allowlistCheckFn = checkFn;
  }

  runCommand(id: string, ...args: unknown[]): unknown {
    if (!this.allowlistCheckFn(id)) {
      console.warn(`Command ${id} is not allowed in current execution context`);
      return;
    }
    const cmd = this.registry.get(id);
    const res = cmd.callback(...args);
    Promise.resolve(res).finally(() => raf.scheduleFullRedraw());
    return res;
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
}

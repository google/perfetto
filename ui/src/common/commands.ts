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

import {Disposable} from '../base/disposable';
import {FuzzyFinder, FuzzySegment} from '../base/fuzzy';
import {Registry} from '../base/registry';
import {Command} from '../public';

export interface CommandWithMatchInfo extends Command {
  segments: FuzzySegment[];
}

export class CommandManager {
  private readonly registry = new Registry<Command>((cmd) => cmd.id);

  get commands(): Command[] {
    return Array.from(this.registry.values());
  }

  registerCommand(cmd: Command): Disposable {
    return this.registry.register(cmd);
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  runCommand(id: string, ...args: any[]): any {
    const cmd = this.registry.get(id);
    return cmd.callback(...args);
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

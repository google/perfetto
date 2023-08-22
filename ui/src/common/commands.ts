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
import {Command} from '../public';

export interface CommandSource {
  commands(): Command[];
}

export interface CommandWithMatchInfo extends Command {
  segments: FuzzySegment[];
}

export class CommandManager {
  private commandSources = new Set<CommandSource>();

  registerCommandSource(cs: CommandSource): Disposable {
    this.commandSources.add(cs);
    return {
      dispose: () => {
        this.commandSources.delete(cs);
      },
    };
  }

  get commands(): Command[] {
    const sourcesArray = Array.from(this.commandSources);
    return sourcesArray.flatMap((source) => source.commands());
  }

  runCommand(id: string, ...args: any[]): void {
    const cmd = this.commands.find((cmd) => cmd.id === id);
    if (cmd) {
      cmd.callback(...args);
    } else {
      console.error(`No such command: ${id}`);
    }
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
